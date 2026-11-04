#include "command/command.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace redis_clone {
namespace {

const std::string& stringArgument(const RespValue::Array& arguments,
                                  std::size_t index) {
    return std::get<BulkString>(arguments[index].data).value;
}

bool parseSize(std::string_view text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

void writeScanReply(Buffer& output, std::size_t cursor,
                    const std::vector<std::string>& keys,
                    std::string_view pattern) {
    std::vector<std::string> matchingKeys;
    matchingKeys.reserve(keys.size());
    for (const std::string& key : keys) {
        if (matchesGlob(key, pattern)) {
            matchingKeys.push_back(key);
        }
    }

    RespWriter::writeArrayHeader(output, 2);
    RespWriter::writeBulk(output, std::to_string(cursor));
    RespWriter::writeArrayHeader(output, matchingKeys.size());
    for (const std::string& key : matchingKeys) {
        RespWriter::writeBulk(output, key);
    }
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
    std::vector<std::string> matchingKeys;
    for (const std::string& key : context.database.keys()) {
        if (matchesGlob(key, pattern)) {
            matchingKeys.push_back(key);
        }
    }
    RespWriter::writeArrayHeader(context.output, matchingKeys.size());
    for (const std::string& key : matchingKeys) {
        RespWriter::writeBulk(context.output, key);
    }
}

void cmd_scan(CommandContext& context, const RespValue::Array& arguments) {
    std::size_t cursor = 0;
    if (!parseSize(stringArgument(arguments, 1), cursor)) {
        RespWriter::writeError(context.output, "ERR invalid cursor");
        return;
    }

    std::size_t count = 10;
    std::string_view pattern = "*";
    for (std::size_t i = 2; i < arguments.size();) {
        const std::string& option = stringArgument(arguments, i);
        if (asciiCaseEqual(option, "MATCH") && i + 1 < arguments.size()) {
            pattern = stringArgument(arguments, i + 1);
            i += 2;
        } else if (asciiCaseEqual(option, "COUNT") &&
                   i + 1 < arguments.size()) {
            if (!parseSize(stringArgument(arguments, i + 1), count) || count == 0) {
                RespWriter::writeError(context.output, "ERR invalid COUNT value");
                return;
            }
            i += 2;
        } else {
            RespWriter::writeError(context.output, "ERR syntax error");
            return;
        }
    }

    std::vector<std::string> scannedKeys;
    const std::size_t nextCursor = context.database.scan(cursor, count, scannedKeys);
    writeScanReply(context.output, nextCursor, scannedKeys, pattern);
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

}  // namespace redis_clone
