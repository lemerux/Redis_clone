#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace redis_clone {

class ExpiryIndex {
public:
    void set(std::string key, std::int64_t deadlineMs);
    bool erase(std::string_view key);
    void clear() noexcept;
    std::optional<std::int64_t> deadline(std::string_view key) const;
    std::optional<std::string> sample(std::uint64_t randomValue) const;
    std::size_t size() const noexcept;

private:
    struct Entry {
        std::int64_t deadlineMs;
        std::size_t keyIndex;
    };

    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> keys_;
};

}  // namespace redis_clone
