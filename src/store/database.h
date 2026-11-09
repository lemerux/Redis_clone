#pragma once

#include "store/expiry_index.h"
#include "store/key_table.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redis_clone {

class Database {
public:
    using RemovalSink = std::function<void(std::string_view)>;

    Object* find(std::string_view key);
    const Object* find(std::string_view key) const;
    Object& findOrCreate(std::string key);
    void setObject(std::string key, Object object, bool keepTtl = false);
    void setString(std::string key, std::string value, bool keepTtl = false);
    bool setExpireAt(std::string_view key, std::int64_t deadlineMs);
    bool persist(std::string_view key);
    std::int64_t ttl(std::string_view key, bool milliseconds);
    std::size_t activeExpireCycle(std::chrono::steady_clock::duration timeBudget);
    void setLoading(bool loading);
    bool erase(std::string_view key);
    bool removeExpired(std::string_view key);
    bool removeForEviction(std::string_view key);
    bool rename(std::string_view source, std::string destination);
    std::size_t size() const;
    std::size_t usedBytes() const noexcept;
    std::optional<std::string> sampleKey(std::uint64_t randomValue) const;
    void flush() noexcept;
    void setRemovalSink(RemovalSink sink);
    const std::vector<std::string>& keys() const noexcept;

private:
    bool eraseInternal(std::string_view key, bool notify);
    std::pair<std::size_t, std::size_t> expireSample(
        std::chrono::steady_clock::time_point deadline);
    void expireAll();

    KeyTable keys_;
    ExpiryIndex expiries_;
    std::size_t usedBytes_ = 0;
    RemovalSink removalSink_;
    std::mt19937_64 randomEngine_{std::random_device{}()};
    bool loading_ = false;
};

}  // namespace redis_clone
