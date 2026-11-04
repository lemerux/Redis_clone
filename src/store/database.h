#pragma once

#include "store/key_table.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redis_clone {

class Database {
public:
    using RemovalSink = std::function<void(std::string_view)>;

    Object* find(std::string_view key);
    const Object* find(std::string_view key) const;
    Object& findOrCreate(std::string key);
    void setObject(std::string key, Object object);
    void setString(std::string key, std::string value);
    bool erase(std::string_view key);
    bool removeExpired(std::string_view key);
    bool removeForEviction(std::string_view key);
    bool rename(std::string_view source, std::string destination);
    std::size_t size() const noexcept;
    std::size_t usedBytes() const noexcept;
    std::optional<std::string> sampleKey(std::uint64_t randomValue) const;
    void flush() noexcept;
    void setRemovalSink(RemovalSink sink);
    std::size_t scan(std::size_t cursor, std::size_t count,
                     std::vector<std::string>& results) const;
    const std::vector<std::string>& keys() const noexcept;

private:
    bool eraseInternal(std::string_view key, bool notify);

    KeyTable keys_;
    std::size_t usedBytes_ = 0;
    RemovalSink removalSink_;
};

}  // namespace redis_clone
