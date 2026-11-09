#include "alloc/alloc_stats.h"

#include <dlfcn.h>
#include <malloc.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace redis_clone {
namespace {

using Mallctl = int (*)(const char*, void*, std::size_t*, void*, std::size_t);

Mallctl findMallctl() noexcept {
    return reinterpret_cast<Mallctl>(::dlsym(RTLD_DEFAULT, "mallctl"));
}

std::size_t readJemallocStat(Mallctl mallctl, const char* name) {
    std::size_t value = 0;
    std::size_t valueSize = sizeof(value);
    if (mallctl(name, &value, &valueSize, nullptr, 0) != 0 ||
        valueSize != sizeof(value)) {
        throw std::runtime_error(std::string("could not read jemalloc ") +
                                 name);
    }
    return value;
}

}  // namespace

AllocatorStats readAllocatorStats() {
    if (const Mallctl mallctl = findMallctl(); mallctl != nullptr) {
        std::uint64_t epoch = 1;
        std::size_t epochSize = sizeof(epoch);
        if (mallctl("epoch", &epoch, &epochSize, &epoch, sizeof(epoch)) != 0) {
            throw std::runtime_error("could not refresh jemalloc statistics");
        }
        return {"jemalloc", readJemallocStat(mallctl, "stats.allocated"),
                readJemallocStat(mallctl, "stats.resident")};
    }

    const struct mallinfo2 stats = ::mallinfo2();
    return {"glibc", stats.uordblks + stats.hblkhd,
            stats.arena + stats.hblkhd};
}

}  // namespace redis_clone
