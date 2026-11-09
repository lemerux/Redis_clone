#pragma once

#include <cstddef>
#include <string_view>

namespace redis_clone {

struct AllocatorStats {
    std::string_view name;
    std::size_t allocatedBytes;
    std::size_t residentBytes;
};

AllocatorStats readAllocatorStats();

}  // namespace redis_clone
