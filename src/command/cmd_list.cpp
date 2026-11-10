#include "command/command.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace redis_clone {
namespace {

const std::string& stringArgument(const RespValue::Array& arguments,
                                  std::size_t index) {
    return std::get<BulkString>(arguments[index].data).value;
}

bool parseInteger(std::string_view text, std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

void writeBulkView(Buffer& output, std::string_view value) {
    RespWriter::writeBulk(output, std::string(value));
}

std::optional<std::size_t> normalizeIndex(std::int64_t index,
                                          std::size_t length) {
    if (index >= 0) {
        const auto positive = static_cast<std::uint64_t>(index);
        return positive < length
                   ? std::optional<std::size_t>(static_cast<std::size_t>(positive))
                   : std::nullopt;
    }
    const auto magnitude = static_cast<std::uint64_t>(-(index + 1)) + 1;
    if (magnitude > length) {
        return std::nullopt;
    }
    return length - static_cast<std::size_t>(magnitude);
}

// Helper returning non-const pointers for in-place mutation
bool getMutableList(CommandContext& context, const std::string& key,
                    Object*& object, List*& list) {
    object = context.database.find(key);
    list = object == nullptr ? nullptr : std::get_if<List>(&object->data);
    if (object != nullptr && list == nullptr) {
        writeWrongTypeError(context.output);
        return false;
    }
    return true;
}

void push(CommandContext& context, const RespValue::Array& arguments,
          bool left) {
    const std::string& key = stringArgument(arguments, 1);
    Object* object = nullptr;
    List* list = nullptr;
    if (!getMutableList(context, key, object, list)) {
        return;
    }
    if (object == nullptr) {
        object = &context.database.findOrCreate(key);
        object->type = ObjType::List;
        object->data = List{};
        list = std::get_if<List>(&object->data);
    }
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        if (left) {
            list->values.push_front(stringArgument(arguments, i));
        } else {
            list->values.push_back(stringArgument(arguments, i));
        }
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(list->values.size()));
}

void pop(CommandContext& context, const RespValue::Array& arguments,
         bool left) {
    const std::string& key = stringArgument(arguments, 1);
    const bool withCount = arguments.size() == 3;
    std::int64_t count = 1;
    if (withCount &&
        (!parseInteger(stringArgument(arguments, 2), count) || count < 0)) {
        RespWriter::writeError(context.output,
                               "ERR value is out of range, must be positive");
        return;
    }
    Object* object = nullptr;
    List* list = nullptr;
    if (!getMutableList(context, key, object, list)) {
        return;
    }
    if (list == nullptr || list->values.empty()) {
        if (withCount) {
            RespWriter::writeArrayHeader(context.output, 0);
        } else {
            RespWriter::writeNull(context.output);
        }
        return;
    }
    const std::size_t popped = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(count), list->values.size());
    if (withCount) {
        RespWriter::writeArrayHeader(context.output, popped);
    }
    std::string scratch;
    for (std::size_t i = 0; i < popped; ++i) {
        const std::size_t idx = left ? 0 : list->values.size() - 1;
        const std::string_view valueView = list->values.peek(idx, scratch);
        RespWriter::writeBulk(context.output, std::string(valueView));
        if (left) {
            list->values.pop_front();
        } else {
            list->values.pop_back();
        }
    }
    if (list->values.empty()) {
        context.database.erase(key);
    }
}

}  // namespace

void cmd_lpush(CommandContext& context, const RespValue::Array& arguments) {
    push(context, arguments, true);
}

void cmd_rpush(CommandContext& context, const RespValue::Array& arguments) {
    push(context, arguments, false);
}

void cmd_lpop(CommandContext& context, const RespValue::Array& arguments) {
    pop(context, arguments, true);
}

void cmd_rpop(CommandContext& context, const RespValue::Array& arguments) {
    pop(context, arguments, false);
}

void cmd_llen(CommandContext& context, const RespValue::Array& arguments) {
    Object* object = nullptr;
    List* list = nullptr;
    if (!getMutableList(context, stringArgument(arguments, 1), object, list)) {
        return;
    }
    RespWriter::writeInt(
        context.output,
        list == nullptr ? 0 : static_cast<std::int64_t>(list->values.size()));
}

void cmd_lrange(CommandContext& context, const RespValue::Array& arguments) {
    std::int64_t startIndex = 0;
    std::int64_t stopIndex = 0;
    if (!parseInteger(stringArgument(arguments, 2), startIndex) ||
        !parseInteger(stringArgument(arguments, 3), stopIndex)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    Object* object = nullptr;
    List* list = nullptr;
    if (!getMutableList(context, stringArgument(arguments, 1), object, list)) {
        return;
    }
    if (list == nullptr || list->values.empty()) {
        RespWriter::writeArrayHeader(context.output, 0);
        return;
    }
    const std::size_t length = list->values.size();
    std::optional<std::size_t> start = normalizeIndex(startIndex, length);
    if (!start.has_value() && startIndex < 0) {
        start = 0; // Fixed: Clamp out-of-bounds negative start index to 0
    }
    if (!start.has_value()) {
        RespWriter::writeArrayHeader(context.output, 0);
        return;
    }
    std::optional<std::size_t> stop = normalizeIndex(stopIndex, length);
    if (!stop.has_value() && stopIndex >= 0) {
        stop = length - 1;
    }
    if (!stop.has_value() || *start > *stop) {
        RespWriter::writeArrayHeader(context.output, 0);
        return;
    }
    const std::size_t count = *stop - *start + 1;
    RespWriter::writeArrayHeader(context.output, count);
    list->values.forEachRange(*start, count, [&](std::string_view value) {
        writeBulkView(context.output, value);
    });
}

void cmd_lindex(CommandContext& context, const RespValue::Array& arguments) {
    std::int64_t index = 0;
    if (!parseInteger(stringArgument(arguments, 2), index)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    Object* object = nullptr;
    List* list = nullptr;
    if (!getMutableList(context, stringArgument(arguments, 1), object, list)) {
        return;
    }
    const auto normalized =
        list == nullptr ? std::nullopt : normalizeIndex(index, list->values.size());
    if (!normalized.has_value()) {
        RespWriter::writeNull(context.output);
        return;
    }
    std::string scratch;
    writeBulkView(context.output, list->values.peek(*normalized, scratch));
}

void cmd_lset(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    std::int64_t index = 0;
    if (!parseInteger(stringArgument(arguments, 2), index)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    Object* object = nullptr;
    List* list = nullptr;
    if (!getMutableList(context, key, object, list)) {
        return;
    }
    if (list == nullptr) {
        RespWriter::writeError(context.output, "ERR no such key");
        return;
    }
    const auto normalized = normalizeIndex(index, list->values.size());
    if (!normalized.has_value()) {
        RespWriter::writeError(context.output, "ERR index out of range");
        return;
    }
    // Fixed: Mutate directly in-place without deep-copying the list
    list->values.set(*normalized, stringArgument(arguments, 3));
    RespWriter::writeOk(context.output);
}

}  // namespace redis_clone