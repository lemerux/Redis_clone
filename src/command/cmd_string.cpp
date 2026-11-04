#include "command/command.h"

#include <charconv>
#include <cstdint>
#include <limits>
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

bool getString(Database& database, std::string_view key,
               const std::string*& value, bool& exists) {
    const Object* object = database.find(key);
    exists = object != nullptr;
    if (object == nullptr) {
        value = nullptr;
        return true;
    }
    value = std::get_if<std::string>(&object->data);
    return value != nullptr;
}

void increment(CommandContext& context, const std::string& key,
               std::int64_t amount) {
    const std::string* current = nullptr;
    bool exists = false;
    if (!getString(context.database, key, current, exists)) {
        writeWrongTypeError(context.output);
        return;
    }

    std::int64_t number = 0;
    if (exists && !parseInteger(*current, number)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    if ((amount > 0 && number > std::numeric_limits<std::int64_t>::max() - amount) ||
        (amount < 0 && number < std::numeric_limits<std::int64_t>::min() - amount)) {
        RespWriter::writeError(context.output,
                               "ERR increment or decrement would overflow");
        return;
    }
    number += amount;
    const std::string updated = std::to_string(number);
    context.database.setString(key, updated);
    RespWriter::writeInt(context.output, number);
}

}  // namespace

void cmd_set(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    const std::string& value = stringArgument(arguments, 2);
    if (arguments.size() != 3) {
        RespWriter::writeError(
            context.output,
            "ERR syntax error (SET options are not supported in this step)");
        return;
    }
    context.database.setString(key, value);
    RespWriter::writeOk(context.output);
}

void cmd_get(CommandContext& context, const RespValue::Array& arguments) {
    const Object* object = context.database.find(stringArgument(arguments, 1));
    if (object == nullptr) {
        RespWriter::writeNull(context.output);
        return;
    }
    const auto* value = std::get_if<std::string>(&object->data);
    if (value == nullptr) {
        writeWrongTypeError(context.output);
        return;
    }
    RespWriter::writeBulk(context.output, *value);
}

void cmd_incr(CommandContext& context, const RespValue::Array& arguments) {
    increment(context, stringArgument(arguments, 1), 1);
}

void cmd_decr(CommandContext& context, const RespValue::Array& arguments) {
    increment(context, stringArgument(arguments, 1), -1);
}

void cmd_incrby(CommandContext& context, const RespValue::Array& arguments) {
    std::int64_t amount = 0;
    if (!parseInteger(stringArgument(arguments, 2), amount)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    increment(context, stringArgument(arguments, 1), amount);
}

void cmd_append(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    const std::string& suffix = stringArgument(arguments, 2);
    const Object* object = context.database.find(key);
    if (object != nullptr && std::get_if<std::string>(&object->data) == nullptr) {
        writeWrongTypeError(context.output);
        return;
    }

    std::string value =
        object == nullptr ? std::string{} : std::get<std::string>(object->data);
    value.append(suffix);
    if (value.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        RespWriter::writeError(context.output, "ERR string exceeds maximum length");
        return;
    }
    const auto length = static_cast<std::int64_t>(value.size());
    context.database.setString(key, std::move(value));
    RespWriter::writeInt(context.output, length);
}

void cmd_strlen(CommandContext& context, const RespValue::Array& arguments) {
    const std::string* value = nullptr;
    bool exists = false;
    if (!getString(context.database, stringArgument(arguments, 1), value, exists)) {
        writeWrongTypeError(context.output);
        return;
    }
    const std::size_t length = exists ? value->size() : 0;
    if (length >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        RespWriter::writeError(context.output, "ERR string exceeds maximum length");
        return;
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(length));
}

void cmd_mget(CommandContext& context, const RespValue::Array& arguments) {
    RespWriter::writeArrayHeader(context.output, arguments.size() - 1);
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const Object* object = context.database.find(stringArgument(arguments, i));
        const auto* value =
            object == nullptr ? nullptr : std::get_if<std::string>(&object->data);
        if (value == nullptr) {
            RespWriter::writeNull(context.output);
        } else {
            RespWriter::writeBulk(context.output, *value);
        }
    }
}

void cmd_mset(CommandContext& context, const RespValue::Array& arguments) {
    if (arguments.size() < 3 || arguments.size() % 2 == 0) {
        RespWriter::writeError(context.output, "ERR wrong number of arguments");
        return;
    }
    for (std::size_t i = 1; i < arguments.size(); i += 2) {
        context.database.setString(stringArgument(arguments, i),
                                    stringArgument(arguments, i + 1));
    }
    RespWriter::writeOk(context.output);
}

}  // namespace redis_clone
