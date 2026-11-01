#include "socket.h"

#include <cerrno>
#include <system_error>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace redis_clone {
namespace {

[[noreturn]] void throwSocketError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

}  // namespace

Socket::Socket(int fd) noexcept : fd_(fd) {}

Socket::~Socket() {
    reset();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Socket Socket::createTcp() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throwSocketError("socket");
    }
    return Socket(fd);
}

int Socket::nativeHandle() const noexcept {
    return fd_;
}

void Socket::setNonBlocking() const {
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        throwSocketError("fcntl");
    }
}

void Socket::setTcpNoDelay() const {
    const int enabled = 1;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0) {
        throwSocketError("setsockopt TCP_NODELAY");
    }
}

void Socket::bindIpv4Any(unsigned short port) const {
    const int reuseAddress = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuseAddress,
                     sizeof(reuseAddress)) < 0) {
        throwSocketError("setsockopt");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throwSocketError("bind");
    }
}

void Socket::listen(int backlog) const {
    if (::listen(fd_, backlog) < 0) {
        throwSocketError("listen");
    }
}

Socket Socket::accept() const {
    const int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) {
        throwSocketError("accept");
    }
    return Socket(clientFd);
}

ssize_t Socket::readSome(char* buffer, std::size_t capacity) const {
    ssize_t bytesRead;
    do {
        bytesRead = ::recv(fd_, buffer, capacity, 0);
    } while (bytesRead < 0 && errno == EINTR);

    if (bytesRead < 0) {
        throwSocketError("recv");
    }
    return bytesRead;
}

ssize_t Socket::writeSome(const char* data, std::size_t size) const {
    ssize_t bytesWritten;
    do {
        bytesWritten = ::send(fd_, data, size, MSG_NOSIGNAL);
    } while (bytesWritten < 0 && errno == EINTR);

    if (bytesWritten < 0) {
        throwSocketError("send");
    }
    return bytesWritten;
}

ssize_t Socket::read(char* buffer, std::size_t capacity) const {
    return readSome(buffer, capacity);
}

void Socket::writeAll(const char* data, std::size_t size) const {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = writeSome(data + written, size - written);
        if (result == 0) {
            throw std::system_error(EPIPE, std::generic_category(), "send");
        }
        written += static_cast<std::size_t>(result);
    }
}

void Socket::reset() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace redis_clone