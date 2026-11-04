#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace redis_clone {

enum class ObjType {
    String,
};

struct Object {
    using Data = std::variant<std::string>;

    Object() : type(ObjType::String), data(std::string{}) {}
    explicit Object(std::string value)
        : type(ObjType::String), data(std::move(value)) {}

    std::size_t logicalSize(std::size_t keySize) const noexcept {
        return keySize + sizeof(Object) + std::get<std::string>(data).size();
    }

    ObjType type;
    Data data;
    std::uint32_t access_meta = 0;
};

}  // namespace redis_clone
