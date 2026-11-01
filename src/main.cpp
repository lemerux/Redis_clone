#include "net/event_loop.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <system_error>

namespace {

volatile std::sig_atomic_t stopping = 0;

void handleSignal(int) {
    stopping = 1;
}

unsigned short parsePort(int argc, char* argv[]) {
    if (argc > 2) {
        throw std::invalid_argument("usage: redis-server [port]");
    }
    if (argc == 1) {
        return 6379;
    }

    char* end = nullptr;
    errno = 0;
    const long port = std::strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || port < 1 ||
        port > std::numeric_limits<unsigned short>::max()) {
        throw std::invalid_argument("port must be an integer from 1 to 65535");
    }
    return static_cast<unsigned short>(port);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        struct sigaction action{};
        action.sa_handler = handleSignal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, nullptr) < 0 ||
            sigaction(SIGTERM, &action, nullptr) < 0) {
            throw std::system_error(errno, std::generic_category(), "sigaction");
        }

        const unsigned short port = parsePort(argc, argv);
        redis_clone::EventLoop eventLoop(port);
        std::cout << "Listening on port " << port << std::endl;

        while (!stopping) {
            eventLoop.runOnce();
        }
    } catch (const std::exception& error) {
        std::cerr << "Server failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}