#include "net/event_loop.h"

#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include <sys/epoll.h>
#include <unistd.h>

namespace redis_clone {
namespace {

constexpr std::size_t kReadBufferSize = 4096;
constexpr std::size_t kMaxEvents = 1024;
constexpr int kTimerTickMilliseconds = 100;

[[noreturn]] void throwSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void epollControl(int epollFd, int operation, int fd, std::uint32_t events,
                  std::uint64_t eventToken) {
    epoll_event event{};
    event.events = events;
    event.data.u64 = eventToken;
    if (::epoll_ctl(epollFd, operation, fd, &event) < 0) {
        throwSystemError("epoll_ctl");
    }
}

void appendCommandError(Buffer& output, std::string_view message) {
    RespWriter::writeError(output, message);
}

void logCommand(const Connection& connection, const RespValue& value) {
    const auto* arguments = std::get_if<RespValue::Array>(&value.data);
    const auto* name = arguments != nullptr && !arguments->empty()
                           ? std::get_if<BulkString>(&arguments->front().data)
                           : nullptr;

    std::clog << "command client=" << connection.clientId << " name=\"";
    if (name == nullptr) {
        std::clog << "<malformed>";
    } else {
        for (const unsigned char character : name->value) {
            if (character >= 0x20 && character <= 0x7e && character != '\\' &&
                character != '"') {
                std::clog << static_cast<char>(character);
            } else {
                std::clog << "\\x" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<unsigned int>(character)
                          << std::dec << std::setfill(' ');
            }
        }
    }
    std::clog << "\"\n";
}

}  // namespace

Connection::Connection(Socket clientSocket, std::uint64_t id,
                       std::uint64_t token)
    : socket(std::move(clientSocket)), clientId(id), eventToken(token) {}

EventLoop::EventLoop(unsigned short port) : listener_(Socket::createTcp()) {
    listener_.setNonBlocking();
    listener_.bindIpv4Any(port);
    listener_.listen(128);

    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        throwSystemError("epoll_create1");
    }
    try {
        epollControl(epollFd_, EPOLL_CTL_ADD, listener_.nativeHandle(), EPOLLIN, 0);
    } catch (...) {
        ::close(epollFd_);
        epollFd_ = -1;
        throw;
    }
}

EventLoop::~EventLoop() {
    if (epollFd_ >= 0) {
        ::close(epollFd_);
    }
}

void EventLoop::runOnce() {
    std::array<epoll_event, kMaxEvents> events{};
    const int eventCount =
        ::epoll_wait(epollFd_, events.data(), static_cast<int>(events.size()),
                     kTimerTickMilliseconds);
    if (eventCount < 0) {
        if (errno == EINTR) {
            return;
        }
        throwSystemError("epoll_wait");
    }

    for (int i = 0; i < eventCount; ++i) {
        const std::uint64_t token = events[static_cast<std::size_t>(i)].data.u64;
        if (token == 0) {
            acceptClients();
        } else {
            const auto found = clientFds_.find(token);
            if (found != clientFds_.end()) {
                handleClientEvent(found->second,
                                  events[static_cast<std::size_t>(i)].events);
            }
        }
    }
    finishDeferredCloses();
}

void EventLoop::acceptClients() {
    while (true) {
        Socket client;
        try {
            client = listener_.accept();
        } catch (const std::system_error& error) {
            if (error.code().value() == EAGAIN ||
                error.code().value() == EWOULDBLOCK) {
                return;
            }
            throw;
        }

        client.setNonBlocking();
        client.setTcpNoDelay();
        const int fd = client.nativeHandle();
        const std::uint64_t token = nextEventToken_++;
        if (token == 0) {
            throw std::overflow_error("epoll event token space exhausted");
        }
        auto connection = std::make_unique<Connection>(
            std::move(client), nextClientId_++, token);
        const auto index = static_cast<std::size_t>(fd);
        if (connections_.size() <= index) {
            connections_.resize(index + 1);
        }
        epollControl(epollFd_, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLRDHUP, token);
        clientFds_.emplace(token, fd);
        connections_[index] = std::move(connection);
    }
}

void EventLoop::handleClientEvent(int fd, std::uint32_t events) {
    if (fd < 0 || static_cast<std::size_t>(fd) >= connections_.size()) {
        return;
    }
    const auto& current = connections_[static_cast<std::size_t>(fd)];
    if (!current || current->closing) {
        return;
    }
    Connection& connection = *current;

    if ((events & EPOLLERR) != 0) {
        closeConnection(connection);
        return;
    }
    if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
        readFromClient(connection);
    }
    if (!connection.closing && (events & EPOLLOUT) != 0) {
        writeToClient(connection);
    }
}

void EventLoop::readFromClient(Connection& connection) {
    std::array<char, kReadBufferSize> buffer{};
    while (true) {
        ssize_t bytesRead;
        try {
            bytesRead = connection.socket.readSome(buffer.data(), buffer.size());
        } catch (const std::system_error& error) {
            if (error.code().value() == EAGAIN ||
                error.code().value() == EWOULDBLOCK) {
                break;
            }
            closeConnection(connection);
            return;
        }

        if (bytesRead == 0) {
            connection.closeAfterReply = true;
            break;
        }
        connection.input.append(std::string_view(
            buffer.data(), static_cast<std::size_t>(bytesRead)));
        dispatchCommands(connection);
        if (connection.closing || connection.closeAfterReply) {
            break;
        }
    }
    if (!connection.closing) {
        if (connection.closeAfterReply && connection.output.empty()) {
            closeConnection(connection);
        } else {
            updateInterest(connection);
        }
    }
}

void EventLoop::dispatchCommands(Connection& connection) {
    while (!connection.input.empty()) {
        ParseResult result = parser_.parse(connection.input);
        if (result.status == ParseStatus::NeedMoreData) {
            return;
        }
        if (result.status == ParseStatus::ProtocolError) {
            appendCommandError(connection.output, "ERR protocol error");
            connection.closeAfterReply = true;
            return;
        }
        dispatchCommand(connection, result.value);
        if (connection.closeAfterReply) {
            return;
        }
    }
}

void EventLoop::dispatchCommand(Connection& connection, const RespValue& value) {
    logCommand(connection, value);

    const auto* arguments = std::get_if<RespValue::Array>(&value.data);
    if (arguments == nullptr || arguments->empty()) {
        appendCommandError(connection.output, "ERR invalid command format");
        return;
    }

    for (const RespValue& argument : *arguments) {
        if (std::get_if<BulkString>(&argument.data) == nullptr) {
            appendCommandError(connection.output, "ERR invalid command format");
            return;
        }
    }

    const auto& name = std::get<BulkString>(arguments->front().data).value;
    const CommandSpec* command = commandTable_.find(name);
    if (command == nullptr) {
        appendCommandError(connection.output, "ERR unknown command");
        return;
    }

    if (arguments->size() < command->minimumArguments ||
        arguments->size() > command->maximumArguments) {
        std::string lowerName(command->name);
        for (char& character : lowerName) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        const std::string message =
            "ERR wrong number of arguments for '" + lowerName + "' command";
        appendCommandError(connection.output, message);
        return;
    }

    CommandContext context{connection.output, connection.closeAfterReply,
                           database_};
    command->handler(context, *arguments);
}

void EventLoop::writeToClient(Connection& connection) {
    const std::string_view pending = connection.output.readable();
    std::size_t written = 0;
    while (written < pending.size()) {
        ssize_t bytesWritten;
        try {
            bytesWritten = connection.socket.writeSome(
                pending.data() + written, pending.size() - written);
        } catch (const std::system_error& error) {
            if (error.code().value() == EAGAIN ||
                error.code().value() == EWOULDBLOCK) {
                break;
            }
            closeConnection(connection);
            return;
        }
        if (bytesWritten == 0) {
            closeConnection(connection);
            return;
        }
        written += static_cast<std::size_t>(bytesWritten);
    }
    connection.output.consume(written);

    if (connection.output.empty() && connection.closeAfterReply) {
        closeConnection(connection);
    } else {
        updateInterest(connection);
    }
}

void EventLoop::updateInterest(Connection& connection) {
    std::uint32_t events = EPOLLRDHUP;
    if (!connection.closeAfterReply) {
        events |= EPOLLIN;
    }
    if (!connection.output.empty()) {
        events |= EPOLLOUT;
    }
    epollControl(epollFd_, EPOLL_CTL_MOD, connection.socket.nativeHandle(),
                 events, connection.eventToken);
}

void EventLoop::closeConnection(Connection& connection) {
    if (connection.closing) {
        return;
    }
    connection.closing = true;
    const int fd = connection.socket.nativeHandle();
    if (::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) < 0 &&
        errno != ENOENT && errno != EBADF) {
        std::cerr << "epoll_ctl DEL failed for client " << connection.clientId
                  << ": " << std::system_error(errno, std::generic_category()).what()
                  << '\n';
    }
    clientFds_.erase(connection.eventToken);
    deferredCloses_.push_back({fd, connection.clientId});
}

void EventLoop::finishDeferredCloses() {
    for (const DeferredClose& deferred : deferredCloses_) {
        const auto index = static_cast<std::size_t>(deferred.fd);
        if (index < connections_.size() && connections_[index] != nullptr &&
            connections_[index]->clientId == deferred.clientId) {
            connections_[index].reset();
        }
    }
    deferredCloses_.clear();
}

}  // namespace redis_clone
