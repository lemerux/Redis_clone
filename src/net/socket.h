#pragma once

#include <cstddef>
#include <sys/types.h>

namespace redis_clone {

class Socket {
public:
    Socket() noexcept = default;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    static Socket createTcp();

    void bindIpv4Any(unsigned short port) const;
    void listen(int backlog) const;
    Socket accept() const;
    ssize_t read(char* buffer, std::size_t capacity) const;
    void writeAll(const char* data, std::size_t size) const;

private:
    explicit Socket(int fd) noexcept;
    void reset() noexcept;

    int fd_ = -1;
};

}  // namespace redis_clone