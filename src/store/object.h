#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "store/skip_list.h"

namespace redis_clone {

enum class ObjType {
    String,
    Hash,
    List,
    Set,
    ZSet,
};

struct Hash {
    std::unordered_map<std::string, std::string> values;

    std::size_t memoryUsage() const noexcept {
        std::size_t bytes = 0;
        if (!values.empty()) {
            bytes += (values.bucket_count() + 1) * sizeof(void*);
        }
        bytes += values.size() * 80;
        for (const auto& [field, value] : values) {
            bytes += field.capacity() + value.capacity();
        }
        return bytes;
    }
};

class ListValues {
public:
    void push_back(std::string value) {
        growIfFull();
        storage_[(head_ + size_) % storage_.size()] = std::move(value);
        ++size_;
    }

    void push_front(std::string value) {
        growIfFull();
        head_ = (head_ + storage_.size() - 1) % storage_.size();
        storage_[head_] = std::move(value);
        ++size_;
    }

    void pop_back() {
        const std::size_t index = (head_ + size_ - 1) % storage_.size();
        storage_[index] = std::string{};
        --size_;
        if (size_ == 0) {
            head_ = 0;
        }
    }

    void pop_front() {
        storage_[head_] = std::string{};
        --size_;
        if (size_ == 0) {
            head_ = 0;
        } else {
            head_ = (head_ + 1) % storage_.size();
        }
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    std::string& operator[](std::size_t index) noexcept {
        return storage_[(head_ + index) % storage_.size()];
    }

    const std::string& operator[](std::size_t index) const noexcept {
        return storage_[(head_ + index) % storage_.size()];
    }

    std::size_t memoryUsage() const noexcept {
        std::size_t bytes = storage_.capacity() * sizeof(std::string);
        for (std::size_t i = 0; i < size_; ++i) {
            bytes += (*this)[i].capacity();
        }
        return bytes;
    }

private:
    void growIfFull() {
        if (size_ < storage_.size()) {
            return;
        }
        const std::size_t maximum = storage_.max_size();
        const std::size_t newCapacity =
            storage_.empty()
                ? 1
                : (storage_.size() > maximum / 2 ? maximum
                                                  : storage_.size() * 2);
        std::vector<std::string> expanded(newCapacity);
        for (std::size_t index = 0; index < size_; ++index) {
            expanded[index] = std::move((*this)[index]);
        }
        storage_ = std::move(expanded);
        head_ = 0;
    }

    std::vector<std::string> storage_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

struct List {
    ListValues values;

    std::size_t memoryUsage() const noexcept {
        return values.memoryUsage();
    }
};

struct Set {
    std::unordered_set<std::string> values;

    std::size_t memoryUsage() const noexcept {
        std::size_t bytes = 0;
        if (!values.empty()) {
            bytes += (values.bucket_count() + 1) * sizeof(void*);
        }
        bytes += values.size() * 48;
        for (const auto& value : values) {
            bytes += value.capacity();
        }
        return bytes;
    }
};

struct Object {
    using Data = std::variant<std::string, Hash, List, Set, std::unique_ptr<ZSet>>;

    Object() : type(ObjType::String), data(std::string{}) {}
    explicit Object(std::string value)
        : type(ObjType::String), data(std::move(value)) {}
    explicit Object(Hash value) : type(ObjType::Hash), data(std::move(value)) {}
    explicit Object(List value) : type(ObjType::List), data(std::move(value)) {}
    explicit Object(Set value) : type(ObjType::Set), data(std::move(value)) {}
    explicit Object(ZSet value)
        : type(ObjType::ZSet),
          data(std::make_unique<ZSet>(std::move(value))) {}
    explicit Object(std::unique_ptr<ZSet> value)
        : type(ObjType::ZSet), data(std::move(value)) {}

    Object(const Object& other)
        : type(other.type), access_meta(other.access_meta) {
        std::visit(
            [this](const auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ZSet>>) {
                    data = val ? std::make_unique<ZSet>(*val) : nullptr;
                } else {
                    data = val;
                }
            },
            other.data);
    }

    Object& operator=(const Object& other) {
        if (this != &other) {
            type = other.type;
            access_meta = other.access_meta;
            std::visit(
                [this](const auto& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ZSet>>) {
                        data = val ? std::make_unique<ZSet>(*val) : nullptr;
                    } else {
                        data = val;
                    }
                },
                other.data);
        }
        return *this;
    }

    Object(Object&&) noexcept = default;
    Object& operator=(Object&&) noexcept = default;
    ~Object() = default;

    std::size_t logicalSize(std::size_t keySize) const noexcept {
        std::size_t size = keySize + sizeof(Object);
        if (const auto* string = std::get_if<std::string>(&data)) {
            return size + string->capacity();
        }
        if (const auto* hash = std::get_if<Hash>(&data)) {
            return size + hash->memoryUsage();
        }
        if (const auto* list = std::get_if<List>(&data)) {
            return size + list->memoryUsage();
        }
        if (const auto* set = std::get_if<Set>(&data)) {
            return size + set->memoryUsage();
        }
        if (const auto* sortedSet =
                       std::get_if<std::unique_ptr<ZSet>>(&data)) {
            if (*sortedSet) {
                return size + sizeof(ZSet) + (*sortedSet)->memoryUsage();
            }
        }
        return size;
    }

    ObjType type;
    Data data;
    std::uint32_t access_meta = 0;
};

}  // namespace redis_clone
