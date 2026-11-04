#include "store/key_table.h"

#include <utility>

namespace redis_clone {

Object* KeyTable::find(std::string_view key) {
    const auto found = entries_.find(std::string(key));
    return found == entries_.end() ? nullptr : &found->second.object;
}

const Object* KeyTable::find(std::string_view key) const {
    const auto found = entries_.find(std::string(key));
    return found == entries_.end() ? nullptr : &found->second.object;
}

Object* KeyTable::insert(std::string key, Object object) {
    const auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        return nullptr;
    }

    const std::size_t index = keys_.size();
    keys_.push_back(key);
    try {
        const auto inserted =
            entries_.emplace(std::move(key), Entry{std::move(object), index});
        if (!inserted.second) {
            keys_.pop_back();
            return nullptr;
        }
        return &inserted.first->second.object;
    } catch (...) {
        keys_.pop_back();
        throw;
    }
}

bool KeyTable::erase(std::string_view key) {
    const auto found = entries_.find(std::string(key));
    if (found == entries_.end()) {
        return false;
    }

    const std::size_t index = found->second.keyIndex;
    if (index + 1 != keys_.size()) {
        keys_[index] = std::move(keys_.back());
        const auto moved = entries_.find(keys_[index]);
        moved->second.keyIndex = index;
    }
    keys_.pop_back();
    entries_.erase(found);
    return true;
}

void KeyTable::clear() noexcept {
    entries_.clear();
    keys_.clear();
}

std::size_t KeyTable::size() const noexcept {
    return entries_.size();
}

std::optional<std::string> KeyTable::sample(std::uint64_t randomValue) const {
    if (keys_.empty()) {
        return std::nullopt;
    }
    return keys_[static_cast<std::size_t>(randomValue % keys_.size())];
}

std::size_t KeyTable::scan(std::size_t cursor, std::size_t count,
                           std::vector<std::string>& results) const {
    results.clear();
    if (cursor >= keys_.size() || count == 0) {
        return 0;
    }
    const std::size_t end = cursor + (count > keys_.size() - cursor
                                          ? keys_.size() - cursor
                                          : count);
    results.insert(results.end(),
                   keys_.begin() + static_cast<std::ptrdiff_t>(cursor),
                   keys_.begin() + static_cast<std::ptrdiff_t>(end));
    return end == keys_.size() ? 0 : end;
}

const std::vector<std::string>& KeyTable::keys() const noexcept {
    return keys_;
}

}  // namespace redis_clone
