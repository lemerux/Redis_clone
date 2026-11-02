#include "net/event_loop.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <exception>
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

class EventLoopThread {
public:
    EventLoopThread() : port_(availablePort()), eventLoop_(port_) {
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
        EventLoopThread server;
        testPipeliningAndErrors(server);
        testFragmentedCommand(server);
        testQuitAndHalfClose(server);
        testConcurrentClients(server);
        server.stop();
        testMidCommandDisconnectAndSameBatchAccept();
    } catch (const std::exception& error) {
        std::cerr << "Event loop test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
