#pragma once

#include <chrono>
#include <cstdint>

namespace redis_clone {

inline std::int64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace redis_clone
