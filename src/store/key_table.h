#pragma once

#include "store/object.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace redis_clone {

class KeyTable {
public:
    Object* find(std::string_view key);
    const Object* find(std::string_view key) const;
    Object* insert(std::string key, Object object);
    bool erase(std::string_view key);
    void clear() noexcept;
    std::size_t size() const noexcept;
    std::optional<std::string> sample(std::uint64_t randomValue) const;
    const std::vector<std::string>& keys() const noexcept;

private:
    struct Entry {
        Object object;
        std::size_t keyIndex;
    };

    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> keys_;
};

}  // namespace redis_clone
