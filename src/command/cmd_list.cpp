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

bool requireList(CommandContext& context, const std::string& key,
                 const Object*& object, const List*& list) {
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
    const Object* existing = context.database.find(key);
    if (existing != nullptr && std::get_if<List>(&existing->data) == nullptr) {
        writeWrongTypeError(context.output);
        return;
    }
    List list = existing == nullptr ? List{} : std::get<List>(existing->data);
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        if (left) {
            list.values.push_front(stringArgument(arguments, i));
        } else {
            list.values.push_back(stringArgument(arguments, i));
        }
    }
    const auto length = static_cast<std::int64_t>(list.values.size());
    context.database.setObject(key, Object{std::move(list)}, true);
    RespWriter::writeInt(context.output, length);
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
    const Object* object = nullptr;
    const List* existing = nullptr;
    if (!requireList(context, key, object, existing)) {
        return;
    }
    if (existing == nullptr) {
        if (withCount) {
            RespWriter::writeArrayHeader(context.output, 0);
        } else {
            RespWriter::writeNull(context.output);
        }
        return;
    }
    const std::size_t popped = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(count), existing->values.size());
    if (withCount) {
        RespWriter::writeArrayHeader(context.output, popped);
    }
    List updated = *existing;
    for (std::size_t i = 0; i < popped; ++i) {
        const std::string value = left
                                      ? updated.values[0]
                                      : updated.values[updated.values.size() - 1];
        if (left) {
            updated.values.pop_front();
        } else {
            updated.values.pop_back();
        }
        RespWriter::writeBulk(context.output, value);
    }
    if (popped != 0) {
        if (updated.values.empty()) {
            context.database.erase(key);
        } else {
            context.database.setObject(key, Object{std::move(updated)}, true);
        }
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
    const Object* object = nullptr;
    const List* list = nullptr;
    if (!requireList(context, stringArgument(arguments, 1), object, list)) {
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
    const Object* object = nullptr;
    const List* list = nullptr;
    if (!requireList(context, stringArgument(arguments, 1), object, list)) {
        return;
    }
    if (list == nullptr || list->values.empty()) {
        RespWriter::writeArrayHeader(context.output, 0);
        return;
    }
    const std::size_t length = list->values.size();
    const auto start = normalizeIndex(startIndex, length);
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
    RespWriter::writeArrayHeader(context.output, *stop - *start + 1);
    for (std::size_t index = *start; index <= *stop; ++index) {
        RespWriter::writeBulk(context.output, list->values[index]);
    }
}

void cmd_lindex(CommandContext& context, const RespValue::Array& arguments) {
    std::int64_t index = 0;
    if (!parseInteger(stringArgument(arguments, 2), index)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    const Object* object = nullptr;
    const List* list = nullptr;
    if (!requireList(context, stringArgument(arguments, 1), object, list)) {
        return;
    }
    const auto normalized =
        list == nullptr ? std::nullopt : normalizeIndex(index, list->values.size());
    if (!normalized.has_value()) {
        RespWriter::writeNull(context.output);
        return;
    }
    RespWriter::writeBulk(context.output, list->values[*normalized]);
}

void cmd_lset(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    std::int64_t index = 0;
    if (!parseInteger(stringArgument(arguments, 2), index)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    const Object* object = nullptr;
    const List* list = nullptr;
    if (!requireList(context, key, object, list)) {
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
    List updated = *list;
    updated.values[*normalized] = stringArgument(arguments, 3);
    context.database.setObject(key, Object{std::move(updated)}, true);
    RespWriter::writeOk(context.output);
}

}  // namespace redis_clone
