#include "store/database.h"

#include <stdexcept>
#include <utility>

namespace redis_clone {

Object* Database::find(std::string_view key) {
    return keys_.find(key);
}

const Object* Database::find(std::string_view key) const {
    return keys_.find(key);
}

Object& Database::findOrCreate(std::string key) {
    if (Object* existing = keys_.find(key)) {
        return *existing;
    }
    const std::size_t addedBytes = key.size() + sizeof(Object);
    Object* inserted = keys_.insert(std::move(key), Object{});
    if (inserted == nullptr) {
        throw std::logic_error("key insertion unexpectedly failed");
    }
    usedBytes_ += addedBytes;
    return *inserted;
}

void Database::setObject(std::string key, Object object) {
    if (Object* existing = keys_.find(key)) {
        const std::size_t oldSize = existing->logicalSize(key.size());
        const std::size_t newSize = object.logicalSize(key.size());
        *existing = std::move(object);
        usedBytes_ = usedBytes_ - oldSize + newSize;
        return;
    }
    const std::size_t addedBytes = object.logicalSize(key.size());
    if (keys_.insert(std::move(key), std::move(object)) == nullptr) {
        throw std::logic_error("key insertion unexpectedly failed");
    }
    usedBytes_ += addedBytes;
}

void Database::setString(std::string key, std::string value) {
    setObject(std::move(key), Object{std::move(value)});
}

bool Database::erase(std::string_view key) {
    return eraseInternal(key, false);
}

bool Database::removeExpired(std::string_view key) {
    return eraseInternal(key, true);
}

bool Database::removeForEviction(std::string_view key) {
    return eraseInternal(key, true);
}

bool Database::eraseInternal(std::string_view key, bool notify) {
    const Object* object = keys_.find(key);
    if (object == nullptr) {
        return false;
    }
    const std::size_t removedBytes = object->logicalSize(key.size());
    const std::string removedKey(key);
    if (!keys_.erase(key)) {
        throw std::logic_error("key disappeared during erase");
    }
    usedBytes_ -= removedBytes;
    if (notify && removalSink_) {
        removalSink_(removedKey);
    }
    return true;
}

bool Database::rename(std::string_view source, std::string destination) {
    const Object* existing = keys_.find(source);
    if (existing == nullptr) {
        return false;
    }
    if (source == destination) {
        return true;
    }
    Object value = *existing;
    erase(destination);
    erase(source);
    setObject(std::move(destination), std::move(value));
    return true;
}

std::size_t Database::size() const noexcept {
    return keys_.size();
}

std::size_t Database::usedBytes() const noexcept {
    return usedBytes_;
}

std::optional<std::string> Database::sampleKey(
    std::uint64_t randomValue) const {
    return keys_.sample(randomValue);
}

void Database::flush() noexcept {
    keys_.clear();
    usedBytes_ = 0;
}

void Database::setRemovalSink(RemovalSink sink) {
    removalSink_ = std::move(sink);
}

std::size_t Database::scan(std::size_t cursor, std::size_t count,
                           std::vector<std::string>& results) const {
    return keys_.scan(cursor, count, results);
}

const std::vector<std::string>& Database::keys() const noexcept {
    return keys_.keys();
}

}  // namespace redis_clone
