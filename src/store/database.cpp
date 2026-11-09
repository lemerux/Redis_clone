#include "store/database.h"

#include "store/clock.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace redis_clone {

Object* Database::find(std::string_view key) {
    if (!loading_) {
        const auto deadline = expiries_.deadline(key);
        if (deadline.has_value() && *deadline <= nowMs()) {
            removeExpired(key);
            return nullptr;
        }
    }
    return keys_.find(key);
}

const Object* Database::find(std::string_view key) const {
    return const_cast<Database*>(this)->find(key);
}

Object& Database::findOrCreate(std::string key) {
    if (Object* existing = find(key)) {
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

void Database::setObject(std::string key, Object object, bool keepTtl) {
    std::optional<std::int64_t> expiry;
    if (keepTtl) {
        find(key);
        expiry = expiries_.deadline(key);
    }
    if (Object* existing = find(key)) {
        const std::size_t oldSize = existing->logicalSize(key.size());
        const std::size_t newSize = object.logicalSize(key.size());
        *existing = std::move(object);
        usedBytes_ = usedBytes_ - oldSize + newSize;
        expiries_.erase(key);
        if (expiry.has_value()) {
            expiries_.set(key, *expiry);
        }
        return;
    }
    const std::size_t addedBytes = object.logicalSize(key.size());
    if (keys_.insert(key, std::move(object)) == nullptr) {
        throw std::logic_error("key insertion unexpectedly failed");
    }
    usedBytes_ += addedBytes;
    if (expiry.has_value()) {
        expiries_.set(key, *expiry);
    }
}

void Database::setString(std::string key, std::string value, bool keepTtl) {
    setObject(std::move(key), Object{std::move(value)}, keepTtl);
}

bool Database::setExpireAt(std::string_view key, std::int64_t deadlineMs) {
    if (find(key) == nullptr) {
        return false;
    }
    if (!loading_ && deadlineMs <= nowMs()) {
        removeExpired(key);
    } else {
        expiries_.set(std::string(key), deadlineMs);
    }
    return true;
}

bool Database::persist(std::string_view key) {
    if (find(key) == nullptr) {
        return false;
    }
    return expiries_.erase(key);
}

std::int64_t Database::ttl(std::string_view key, bool milliseconds) {
    if (find(key) == nullptr) {
        return -2;
    }
    const auto deadline = expiries_.deadline(key);
    if (!deadline.has_value()) {
        return -1;
    }

    const std::int64_t currentTime = nowMs();
    if (*deadline <= currentTime) {
        if (loading_) {
            return 0;
        }
        removeExpired(key);
        return -2;
    }
    const std::int64_t remaining = *deadline - currentTime;
    return milliseconds ? remaining : remaining / 1000;
}

std::size_t Database::activeExpireCycle(
    std::chrono::steady_clock::duration timeBudget) {
    if (loading_ || timeBudget <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    const auto deadline = startedAt + timeBudget;
    std::size_t removed = 0;
    while (expiries_.size() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        const auto [expired, sampled] = expireSample(deadline);
        removed += expired;
        if (sampled == 0 || expired * 4 <= sampled) {
            break;
        }
    }
    return removed;
}

void Database::setLoading(bool loading) {
    if (loading_ && !loading) {
        loading_ = false;
        expireAll();
        return;
    }
    loading_ = loading;
}

bool Database::erase(std::string_view key) {
    if (find(key) == nullptr) {
        return false;
    }
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
        expiries_.erase(key);
        return false;
    }
    const std::size_t removedBytes = object->logicalSize(key.size());
    const std::string removedKey(key);
    if (!keys_.erase(key)) {
        throw std::logic_error("key disappeared during erase");
    }
    usedBytes_ -= removedBytes;
    expiries_.erase(key);
    if (notify && removalSink_) {
        removalSink_(removedKey);
    }
    return true;
}

bool Database::rename(std::string_view source, std::string destination) {
    const Object* existing = find(source);
    if (existing == nullptr) {
        return false;
    }
    if (source == destination) {
        return true;
    }
    Object value = *existing;
    const auto expiry = expiries_.deadline(source);
    erase(destination);
    erase(source);
    setObject(destination, std::move(value));
    if (expiry.has_value()) {
        setExpireAt(destination, *expiry);
    }
    return true;
}

std::size_t Database::size() const {
    if (expiries_.size() == 0) {
        return keys_.size();
    }
    const std::vector<std::string> currentKeys = keys_.keys();
    std::size_t liveKeys = 0;
    for (const std::string& key : currentKeys) {
        liveKeys += find(key) != nullptr ? 1 : 0;
    }
    return liveKeys;
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
    expiries_.clear();
    usedBytes_ = 0;
}

void Database::setRemovalSink(RemovalSink sink) {
    removalSink_ = std::move(sink);
}

const std::vector<std::string>& Database::keys() const noexcept {
    return keys_.keys();
}

std::pair<std::size_t, std::size_t> Database::expireSample(
    std::chrono::steady_clock::time_point deadline) {
    constexpr std::size_t kSampleSize = 20;
    const std::size_t sampleCount = std::min(kSampleSize, expiries_.size());
    std::size_t expired = 0;
    const std::int64_t currentTime = nowMs();
    std::size_t sampled = 0;
    for (std::size_t i = 0; i < sampleCount &&
                            std::chrono::steady_clock::now() < deadline;
         ++i) {
        const auto key = expiries_.sample(randomEngine_());
        if (!key.has_value()) {
            break;
        }
        ++sampled;
        const auto deadline = expiries_.deadline(*key);
        if (deadline.has_value() && *deadline <= currentTime) {
            expired += removeExpired(*key) ? 1 : 0;
        }
    }
    return {expired, sampled};
}

void Database::expireAll() {
    const std::vector<std::string> expiringKeys = keys_.keys();
    const std::int64_t currentTime = nowMs();
    for (const std::string& key : expiringKeys) {
        const auto deadline = expiries_.deadline(key);
        if (deadline.has_value() && *deadline <= currentTime) {
            removeExpired(key);
        }
    }
}

}  // namespace redis_clone
