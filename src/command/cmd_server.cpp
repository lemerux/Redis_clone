#include "command/command.h"

#include <string>
#include <string_view>

namespace redis_clone {
namespace {

const std::string& stringArgument(const RespValue::Array& arguments,
                                  std::size_t index) {
    return std::get<BulkString>(arguments[index].data).value;
}

}  // namespace

void cmd_echo(CommandContext& context, const RespValue::Array& arguments) {
    RespWriter::writeBulk(context.output, stringArgument(arguments, 1));
}

void cmd_info(CommandContext& context, const RespValue::Array&) {
    std::string info =
        "# Server\r\nredis_version:0.1.0\r\n"
        "# Keyspace\r\nkeys:" +
        std::to_string(context.database.size()) + "\r\n"
        "# Memory\r\nused_bytes:" +
        std::to_string(context.database.usedBytes()) + "\r\n";
    RespWriter::writeBulk(context.output, info);
}

void cmd_config(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& subcommand = stringArgument(arguments, 1);
    const std::string& pattern = stringArgument(arguments, 2);
    if (!asciiCaseEqual(subcommand, "GET")) {
        RespWriter::writeError(context.output, "ERR unsupported CONFIG subcommand");
        return;
    }

    constexpr std::string_view kNames[] = {"databases", "maxmemory"};
    constexpr std::string_view kValues[] = {"1", "0"};
    std::size_t matches = 0;
    for (const std::string_view name : kNames) {
        matches += matchesGlob(name, pattern) ? 1 : 0;
    }
    RespWriter::writeArrayHeader(context.output, matches * 2);
    for (std::size_t i = 0; i < 2; ++i) {
        if (matchesGlob(kNames[i], pattern)) {
            RespWriter::writeBulk(context.output, kNames[i]);
            RespWriter::writeBulk(context.output, kValues[i]);
        }
    }
}

}  // namespace redis_clone
