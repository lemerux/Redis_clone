#include "command/command.h"

#include <array>
#include <cctype>
#include <string>

namespace redis_clone {
namespace {

const std::array<CommandSpec, 3> kCommands{{
    {"PING", 1, 2, cmd_ping},
    {"ECHO", 2, 2, cmd_echo},
    {"QUIT", 1, 1, cmd_quit},
}};

const std::string& stringArgument(const RespValue::Array& arguments,
                                  std::size_t index) {
    return std::get<BulkString>(arguments[index].data).value;
}

bool commandNameMatches(std::string_view candidate,
                        std::string_view registered) noexcept {
    if (candidate.size() != registered.size()) {
        return false;
    }
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(candidate[i])) !=
            registered[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

void cmd_ping(CommandContext& context, const RespValue::Array& arguments) {
    if (arguments.size() == 1) {
        context.output.append("+PONG\r\n");
    } else {
        RespWriter::writeBulk(context.output, stringArgument(arguments, 1));
    }
}

void cmd_echo(CommandContext& context, const RespValue::Array& arguments) {
    RespWriter::writeBulk(context.output, stringArgument(arguments, 1));
}

void cmd_quit(CommandContext& context, const RespValue::Array&) {
    RespWriter::writeOk(context.output);
    context.closeAfterReply = true;
}

const CommandSpec* CommandTable::find(std::string_view name) const noexcept {
    for (const CommandSpec& command : kCommands) {
        if (commandNameMatches(name, command.name)) {
            return &command;
        }
    }
    return nullptr;
}

}  // namespace redis_clone
