#include "net/event_loop.h"
#include "user/user_registry.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScopedLogCapture {
public:
    ScopedLogCapture() : previous_(std::clog.rdbuf(output_.rdbuf())) {}
    ~ScopedLogCapture() {
        std::clog.rdbuf(previous_);
    }

    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    std::string str() const {
        return output_.str();
    }

private:
    std::ostringstream output_;
    std::streambuf* previous_;
};

unsigned short availablePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("could not create test port socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const int error = errno;
        ::close(fd);
        throw std::runtime_error(std::string("could not bind test port: ") +
                                 std::strerror(error));
    }

    socklen_t addressLength = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &addressLength) < 0) {
        const int error = errno;
        ::close(fd);
        throw std::runtime_error(std::string("could not inspect test port: ") +
                                 std::strerror(error));
    }
    ::close(fd);
    return ntohs(address.sin_port);
}

redis_clone::UserRegistry makeTestUsers() {
    redis_clone::UserRegistry users;
    users.addUser("pipeline", "pipeline-pass");
    users.addUser("strings", "strings-pass");
    users.addUser("alpha", "alpha-pass");
    users.addUser("beta", "beta-pass");
    return users;
}

void testUsersFileLoading() {
    char pathTemplate[] = "/tmp/redis-clone-users-test-XXXXXX";
    const int fd = ::mkstemp(pathTemplate);
    if (fd < 0) {
        throw std::runtime_error("could not create users-file test fixture");
    }
    ::close(fd);
    const std::string path(pathTemplate);
    try {
        {
            std::ofstream usersFile(path);
            if (!usersFile) {
                throw std::runtime_error(
                    "could not create users-file test fixture");
            }
            usersFile << "# test accounts\nfile-user file-password\n";
            if (!usersFile) {
                throw std::runtime_error(
                    "could not write users-file test fixture");
            }
        }
        const redis_clone::UserRegistry users =
            redis_clone::UserRegistry::loadFromFile(path);
        const redis_clone::User* user = users.find("file-user");
        expect(user != nullptr &&
                   users.verifyPassword(*user, "file-password"),
               "users file did not configure the expected account");
    } catch (...) {
        std::remove(path.c_str());
        throw;
    }
    if (std::remove(path.c_str()) != 0) {
        throw std::runtime_error("could not remove users-file test fixture");
    }
}

class EventLoopThread {
public:
    EventLoopThread()
        : port_(availablePort()), eventLoop_(port_, makeTestUsers()) {
        thread_ = std::thread([this] {
            try {
                while (running_) {
                    eventLoop_.runOnce();
                }
            } catch (...) {
                error_ = std::current_exception();
                running_ = false;
            }
        });
    }

    ~EventLoopThread() {
        stop();
    }

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    unsigned short port() const noexcept {
        return port_;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

private:
    unsigned short port_;
    redis_clone::EventLoop eventLoop_;
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::exception_ptr error_;
};

class Client {
public:
    explicit Client(unsigned short port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("could not create test client socket");
        }

        timeval timeout{};
        timeout.tv_sec = 3;
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            const int error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(std::string("could not set receive timeout: ") +
                                     std::strerror(error));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) < 0) {
            const int error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(std::string("could not connect test client: ") +
                                     std::strerror(error));
        }
    }

    ~Client() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    Client& operator=(Client&&) = delete;

    void send(std::string_view bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t sent =
                ::send(fd_, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            if (sent <= 0) {
                throw std::runtime_error(std::string("test client send failed: ") +
                                         std::strerror(errno));
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    void authenticate(std::string_view username, std::string_view password) {
        std::string command = "*3\r\n$4\r\nAUTH\r\n$";
        command.append(std::to_string(username.size()));
        command.append("\r\n");
        command.append(username);
        command.append("\r\n$");
        command.append(std::to_string(password.size()));
        command.append("\r\n");
        command.append(password);
        command.append("\r\n");
        send(command);
        expect(readExact(5) == "+OK\r\n", "AUTH failed for configured test user");
    }

    std::string readExact(std::size_t length) {
        std::string result(length, '\0');
        std::size_t offset = 0;
        while (offset < length) {
            const ssize_t received =
                ::recv(fd_, result.data() + offset, length - offset, 0);
            if (received < 0 && errno == EINTR) {
                continue;
            }
            if (received <= 0) {
                throw std::runtime_error(std::string("test client receive failed: ") +
                                         (received == 0 ? "connection closed"
                                                        : std::strerror(errno)));
            }
            offset += static_cast<std::size_t>(received);
        }
        return result;
    }

    void shutdownWrite() {
        if (::shutdown(fd_, SHUT_WR) < 0) {
            throw std::runtime_error("test client could not half-close");
        }
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool hasPendingResult() const {
        pollfd descriptor{fd_, POLLIN, 0};
        int result;
        do {
            result = ::poll(&descriptor, 1, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            throw std::runtime_error(std::string("test client poll failed: ") +
                                     std::strerror(errno));
        }
        return result > 0;
    }

private:
    int fd_ = -1;
};

void testPipeliningAndErrors(EventLoopThread& server) {
    ScopedLogCapture log;
    Client client(server.port());
    client.authenticate("pipeline", "pipeline-pass");
    client.send("*1\r\n$4\r\nPING\r\n"
                "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n"
                "*1\r\n$9\r\nNOTACMDxx\r\n"
                "*1\r\n$4\r\nECHO\r\n");
    const std::string expected =
        "+PONG\r\n"
        "$5\r\nhello\r\n"
        "-ERR unknown command\r\n"
        "-ERR wrong number of arguments for 'echo' command\r\n";
    expect(client.readExact(expected.size()) == expected,
           "pipelined commands or command errors returned unexpected replies");
    const std::string loggedCommands = log.str();
    expect(loggedCommands.find("name=\"PING\"") != std::string::npos &&
               loggedCommands.find("name=\"ECHO\"") != std::string::npos &&
               loggedCommands.find("name=\"NOTACMDxx\"") != std::string::npos,
           "command log did not include every pipelined command name");
    expect(loggedCommands.find("hello") == std::string::npos,
           "command log unexpectedly included command arguments");
    expect(loggedCommands.find("pipeline-pass") == std::string::npos,
           "command log unexpectedly included an AUTH password");
}

void testFragmentedCommand(EventLoopThread& server) {
    Client client(server.port());
    client.send("*1\r\n");
    client.send("$4\r");
    client.send("\nPI");
    client.send("NG\r\n");
    expect(client.readExact(7) == "+PONG\r\n",
           "fragmented PING returned an unexpected reply");
}

void testQuitAndHalfClose(EventLoopThread& server) {
    {
        Client client(server.port());
        client.send("*1\r\n$4\r\nQUIT\r\n");
        expect(client.readExact(5) == "+OK\r\n", "QUIT did not return +OK");
    }
    {
        Client client(server.port());
        client.send("*1\r\n$4\r\nPING\r\n");
        client.shutdownWrite();
        expect(client.readExact(7) == "+PONG\r\n",
               "server did not flush the reply after a client half-close");
    }
}

void testConcurrentClients(EventLoopThread& server) {
    constexpr std::size_t clientCount = 64;
    std::vector<Client> clients;
    clients.reserve(clientCount);
    for (std::size_t i = 0; i < clientCount; ++i) {
        clients.emplace_back(server.port());
    }
    for (Client& client : clients) {
        client.send("*1\r\n$4\r\nPING\r\n");
    }
    for (Client& client : clients) {
        expect(client.readExact(7) == "+PONG\r\n",
               "concurrent client received an unexpected reply");
    }
}

void testStringAndKeyCommands(EventLoopThread& server) {
    Client client(server.port());
    client.authenticate("strings", "strings-pass");
    client.send("*3\r\n$3\r\nSET\r\n$1\r\nn\r\n$2\r\n41\r\n"
                "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n"
                "*3\r\n$6\r\nINCRBY\r\n$1\r\nn\r\n$1\r\n8\r\n"
                "*2\r\n$4\r\nDECR\r\n$1\r\nn\r\n"
                "*2\r\n$3\r\nGET\r\n$1\r\nn\r\n"
                "*3\r\n$6\r\nAPPEND\r\n$1\r\nn\r\n$3\r\nxyz\r\n"
                "*2\r\n$6\r\nSTRLEN\r\n$1\r\nn\r\n");
    const std::string stringReplies =
        "+OK\r\n:42\r\n:50\r\n:49\r\n$2\r\n49\r\n:5\r\n:5\r\n";
    expect(client.readExact(stringReplies.size()) == stringReplies,
           "string commands returned unexpected replies");

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nmax\r\n$19\r\n"
                "9223372036854775807\r\n"
                "*2\r\n$4\r\nINCR\r\n$3\r\nmax\r\n"
                "*2\r\n$3\r\nGET\r\n$3\r\nmax\r\n"
                "*3\r\n$6\r\nINCRBY\r\n$3\r\nmax\r\n$2\r\n-1\r\n");
    const std::string overflowReplies =
        "+OK\r\n-ERR increment or decrement would overflow\r\n"
        "$19\r\n9223372036854775807\r\n:9223372036854775806\r\n";
    expect(client.readExact(overflowReplies.size()) == overflowReplies,
           "integer overflow should fail without changing the stored value");

    client.send("*5\r\n$4\r\nMSET\r\n$1\r\na\r\n$3\r\none\r\n"
                "$1\r\nb\r\n$3\r\ntwo\r\n"
                "*4\r\n$4\r\nMGET\r\n$1\r\na\r\n$1\r\nb\r\n$7\r\nmissing\r\n"
                "*3\r\n$3\r\nDEL\r\n$1\r\na\r\n$7\r\nmissing\r\n"
                "*3\r\n$6\r\nEXISTS\r\n$1\r\nb\r\n$1\r\nb\r\n"
                "*2\r\n$4\r\nTYPE\r\n$1\r\nn\r\n"
                "*1\r\n$6\r\nDBSIZE\r\n");
    const std::string keyReplies =
        "+OK\r\n*3\r\n$3\r\none\r\n$3\r\ntwo\r\n$-1\r\n"
        ":1\r\n:2\r\n+string\r\n:3\r\n";
    expect(client.readExact(keyReplies.size()) == keyReplies,
           "multi-key and key-inspection commands returned unexpected replies");

    client.send("*2\r\n$4\r\nKEYS\r\n$1\r\n*\r\n"
                "*1\r\n$4\r\nSCAN\r\n");
    const std::string enumerationReplies =
        "*3\r\n$1\r\nn\r\n$3\r\nmax\r\n$1\r\nb\r\n"
        "-ERR unknown command\r\n";
    expect(client.readExact(enumerationReplies.size()) == enumerationReplies,
           "KEYS returned unexpected results or removed SCAN is still registered");

    client.send("*3\r\n$6\r\nRENAME\r\n$1\r\nn\r\n$7\r\nrenamed\r\n"
                "*2\r\n$3\r\nGET\r\n$7\r\nrenamed\r\n"
                "*3\r\n$6\r\nCONFIG\r\n$3\r\nGET\r\n$9\r\nmaxmemory\r\n"
                "*1\r\n$8\r\nFLUSHALL\r\n"
                "*1\r\n$6\r\nDBSIZE\r\n");
    const std::string managementReplies =
        "+OK\r\n$5\r\n49xyz\r\n*2\r\n$9\r\nmaxmemory\r\n$1\r\n0\r\n"
        "+OK\r\n:0\r\n";
    expect(client.readExact(managementReplies.size()) == managementReplies,
           "rename, CONFIG, or flush commands returned unexpected replies");
}

void testAuthAndUserIsolation(EventLoopThread& server) {
    const std::string noAuthReply = "-NOAUTH Authentication required.\r\n";
    Client unauthenticated(server.port());
    unauthenticated.send("*2\r\n$3\r\nGET\r\n$7\r\nbalance\r\n");
    expect(unauthenticated.readExact(noAuthReply.size()) == noAuthReply,
           "unauthenticated data command was not rejected");

    Client invalidCredentials(server.port());
    invalidCredentials.send(
        "*3\r\n$4\r\nAUTH\r\n$5\r\nalpha\r\n$9\r\nwrongpass\r\n");
    const std::string wrongPasswordReply =
        "-WRONGPASS invalid username-password pair or user is disabled\r\n";
    expect(invalidCredentials.readExact(wrongPasswordReply.size()) ==
               wrongPasswordReply,
           "invalid credentials were accepted");

    Client alpha(server.port());
    alpha.authenticate("alpha", "alpha-pass");
    alpha.send("*1\r\n$6\r\nWHOAMI\r\n");
    expect(alpha.readExact(11) == "$5\r\nalpha\r\n",
           "WHOAMI did not return the authenticated username");
    alpha.send("*3\r\n$4\r\nAUTH\r\n$4\r\nbeta\r\n$9\r\nbeta-pass\r\n"
               "*3\r\n$3\r\nSET\r\n$7\r\nbalance\r\n$2\r\n10\r\n"
               "*3\r\n$3\r\nSET\r\n$10\r\npersistent\r\n$2\r\n42\r\n");
    const std::string alphaReplies =
        "-ERR already authenticated on this connection\r\n+OK\r\n+OK\r\n";
    expect(alpha.readExact(alphaReplies.size()) == alphaReplies,
           "a second AUTH switched the authenticated user");

    Client duplicateAlpha(server.port());
    duplicateAlpha.send("*3\r\n$4\r\nAUTH\r\n$5\r\nalpha\r\n$10\r\nalpha-pass\r\n");
    const std::string busyReply =
        "-ERR user already has an active session\r\n";
    expect(duplicateAlpha.readExact(busyReply.size()) == busyReply,
           "a second live session for a user was not refused");
    duplicateAlpha.send("*2\r\n$3\r\nGET\r\n$7\r\nbalance\r\n");
    expect(duplicateAlpha.readExact(noAuthReply.size()) == noAuthReply,
           "failed AUTH unexpectedly authenticated the connection");

    Client beta(server.port());
    beta.authenticate("beta", "beta-pass");
    beta.send("*3\r\n$3\r\nSET\r\n$7\r\nbalance\r\n$2\r\n20\r\n"
              "*2\r\n$3\r\nGET\r\n$7\r\nbalance\r\n");
    expect(beta.readExact(13) == "+OK\r\n$2\r\n20\r\n",
           "user beta did not access its own database");

    alpha.send("*2\r\n$3\r\nGET\r\n$7\r\nbalance\r\n");
    expect(alpha.readExact(8) == "$2\r\n10\r\n",
           "user alpha did not access its own database");
    alpha.send("*2\r\n$3\r\nDEL\r\n$7\r\nbalance\r\n"
               "*2\r\n$4\r\nKEYS\r\n$1\r\n*\r\n"
               "*1\r\n$6\r\nDBSIZE\r\n"
               "*1\r\n$8\r\nFLUSHALL\r\n"
               "*2\r\n$3\r\nGET\r\n$10\r\npersistent\r\n"
               "*3\r\n$3\r\nSET\r\n$10\r\npersistent\r\n$2\r\n42\r\n");
    const std::string alphaIsolationReplies =
        ":1\r\n*1\r\n$10\r\npersistent\r\n:1\r\n+OK\r\n$-1\r\n+OK\r\n";
    expect(alpha.readExact(alphaIsolationReplies.size()) ==
               alphaIsolationReplies,
           "keyspace management commands did not remain user-scoped");
    beta.send("*2\r\n$3\r\nGET\r\n$7\r\nbalance\r\n");
    expect(beta.readExact(8) == "$2\r\n20\r\n",
           "alpha's DEL or FLUSHALL affected beta's database");

    alpha.send("*1\r\n$4\r\nQUIT\r\n");
    expect(alpha.readExact(5) == "+OK\r\n", "QUIT did not release alpha's session");
    Client reconnectedAlpha(server.port());
    reconnectedAlpha.authenticate("alpha", "alpha-pass");
    reconnectedAlpha.send("*2\r\n$3\r\nGET\r\n$10\r\npersistent\r\n");
    expect(reconnectedAlpha.readExact(8) == "$2\r\n42\r\n",
           "user database was not retained across authenticated connections");
}

void testMidCommandDisconnectAndSameBatchAccept() {
    const unsigned short port = availablePort();
    redis_clone::EventLoop eventLoop(port);

    Client incompleteCommand(port);
    eventLoop.runOnce();
    incompleteCommand.send("*2\r\n$4\r\nECHO\r\n$100\r\npartial");
    eventLoop.runOnce();
    incompleteCommand.close();

    Client replacement(port);
    replacement.send("*1\r\n$4\r\nPING\r\n");

    // Both the old connection's disconnect and the new connection are ready
    // before this wait, so their events are handled in the same epoll batch.
    eventLoop.runOnce();
    for (int attempt = 0; attempt < 10 && !replacement.hasPendingResult(); ++attempt) {
        eventLoop.runOnce();
    }
    expect(replacement.readExact(7) == "+PONG\r\n",
           "new connection was affected by a mid-command disconnect");
}

}  // namespace

int main() {
    try {
        testUsersFileLoading();
        EventLoopThread server;
        testPipeliningAndErrors(server);
        testFragmentedCommand(server);
        testQuitAndHalfClose(server);
        testConcurrentClients(server);
        testStringAndKeyCommands(server);
        testAuthAndUserIsolation(server);
        server.stop();
        testMidCommandDisconnectAndSameBatchAccept();
    } catch (const std::exception& error) {
        std::cerr << "Event loop test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
