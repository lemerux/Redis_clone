#include "store/expiry_index.h"

#include <utility>

namespace redis_clone {

void ExpiryIndex::set(std::string key, std::int64_t deadlineMs) {
    const auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        existing->second.deadlineMs = deadlineMs;
        return;
    }

    const std::size_t index = keys_.size();
    keys_.push_back(key);
    try {
        const auto inserted =
            entries_.emplace(std::move(key), Entry{deadlineMs, index});
        if (!inserted.second) {
            keys_.pop_back();
        }
    } catch (...) {
        keys_.pop_back();
        throw;
    }
}

bool ExpiryIndex::erase(std::string_view key) {
    const auto found = entries_.find(std::string(key));
    if (found == entries_.end()) {
        return false;
    }

    const std::size_t index = found->second.keyIndex;
    if (index + 1 != keys_.size()) {
        keys_[index] = std::move(keys_.back());
        entries_.find(keys_[index])->second.keyIndex = index;
    }
    keys_.pop_back();
    entries_.erase(found);
    return true;
}

void ExpiryIndex::clear() noexcept {
    entries_.clear();
    keys_.clear();
}

std::optional<std::int64_t> ExpiryIndex::deadline(
    std::string_view key) const {
    const auto found = entries_.find(std::string(key));
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->second.deadlineMs;
}

std::optional<std::string> ExpiryIndex::sample(
    std::uint64_t randomValue) const {
    if (keys_.empty()) {
        return std::nullopt;
    }
    return keys_[static_cast<std::size_t>(randomValue % keys_.size())];
}

std::size_t ExpiryIndex::size() const noexcept {
    return keys_.size();
}

}  // namespace redis_clone
