#include "command/command.h"

#include "store/clock.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

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

bool toMilliseconds(std::int64_t value, std::int64_t multiplier,
                    std::int64_t& result) {
    if (value > std::numeric_limits<std::int64_t>::max() / multiplier ||
        value < std::numeric_limits<std::int64_t>::min() / multiplier) {
        return false;
    }
    result = value * multiplier;
    return true;
}

void expire(CommandContext& context, const RespValue::Array& arguments,
            std::int64_t multiplier, bool absolute) {
    std::int64_t value = 0;
    if (!parseInteger(stringArgument(arguments, 2), value)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }

    std::int64_t milliseconds = 0;
    if (!toMilliseconds(value, multiplier, milliseconds)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    if (!absolute) {
        const std::int64_t currentTime = nowMs();
        if ((milliseconds > 0 &&
             currentTime >
                 std::numeric_limits<std::int64_t>::max() - milliseconds) ||
            (milliseconds < 0 &&
             currentTime <
                 std::numeric_limits<std::int64_t>::min() - milliseconds)) {
            RespWriter::writeError(context.output,
                                   "ERR value is not an integer or out of range");
            return;
        }
        milliseconds += currentTime;
    }
    RespWriter::writeInt(
        context.output,
        context.database.setExpireAt(stringArgument(arguments, 1), milliseconds)
            ? 1
            : 0);
}

}  // namespace

void cmd_del(CommandContext& context, const RespValue::Array& arguments) {
    std::size_t deleted = 0;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        deleted += context.database.erase(stringArgument(arguments, i)) ? 1 : 0;
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(deleted));
}

void cmd_exists(CommandContext& context, const RespValue::Array& arguments) {
    std::size_t found = 0;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        found += context.database.find(stringArgument(arguments, i)) != nullptr
                     ? 1
                     : 0;
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(found));
}

void cmd_type(CommandContext& context, const RespValue::Array& arguments) {
    const Object* object = context.database.find(stringArgument(arguments, 1));
    if (object == nullptr) {
        context.output.append("+none\r\n");
        return;
    }
    switch (object->type) {
        case ObjType::String:
            context.output.append("+string\r\n");
            return;
        case ObjType::Hash:
            context.output.append("+hash\r\n");
            return;
        case ObjType::List:
            context.output.append("+list\r\n");
            return;
        case ObjType::Set:
            context.output.append("+set\r\n");
            return;
        case ObjType::ZSet:
            context.output.append("+zset\r\n");
            return;
    }
    context.output.append("+none\r\n");
}

void cmd_dbsize(CommandContext& context, const RespValue::Array&) {
    RespWriter::writeInt(context.output,
                         static_cast<std::int64_t>(context.database.size()));
}

void cmd_flushall(CommandContext& context, const RespValue::Array&) {
    context.database.flush();
    RespWriter::writeOk(context.output);
}

void cmd_keys(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& pattern = stringArgument(arguments, 1);
    const std::vector<std::string> keys = context.database.keys();
    std::vector<std::string> matchingKeys;
    for (const std::string& key : keys) {
        if (context.database.find(key) != nullptr &&
            matchesGlob(key, pattern)) {
            matchingKeys.push_back(key);
        }
    }
    RespWriter::writeArrayHeader(context.output, matchingKeys.size());
    for (const std::string& key : matchingKeys) {
        RespWriter::writeBulk(context.output, key);
    }
}

void cmd_rename(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& source = stringArgument(arguments, 1);
    const std::string& destination = stringArgument(arguments, 2);
    if (!context.database.rename(source, destination)) {
        RespWriter::writeError(context.output, "ERR no such key");
        return;
    }
    RespWriter::writeOk(context.output);
}

void cmd_expire(CommandContext& context, const RespValue::Array& arguments) {
    expire(context, arguments, 1000, false);
}

void cmd_pexpire(CommandContext& context, const RespValue::Array& arguments) {
    expire(context, arguments, 1, false);
}

void cmd_expireat(CommandContext& context, const RespValue::Array& arguments) {
    expire(context, arguments, 1000, true);
}

void cmd_pexpireat(CommandContext& context,
                   const RespValue::Array& arguments) {
    expire(context, arguments, 1, true);
}

void cmd_ttl(CommandContext& context, const RespValue::Array& arguments) {
    RespWriter::writeInt(
        context.output,
        context.database.ttl(stringArgument(arguments, 1), false));
}

void cmd_pttl(CommandContext& context, const RespValue::Array& arguments) {
    RespWriter::writeInt(
        context.output,
        context.database.ttl(stringArgument(arguments, 1), true));
}

void cmd_persist(CommandContext& context,
                 const RespValue::Array& arguments) {
    RespWriter::writeInt(
        context.output,
        context.database.persist(stringArgument(arguments, 1)) ? 1 : 0);
}

}  // namespace redis_clone
