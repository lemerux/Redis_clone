#pragma once

#include "resp/resp.h"

#include <cstddef>
#include <string_view>

namespace redis_clone {

struct CommandContext {
    Buffer& output;
    bool& closeAfterReply;
};

using CommandHandler = void (*)(CommandContext&, const RespValue::Array&);

struct CommandSpec {
    std::string_view name;
    std::size_t minimumArguments;
    std::size_t maximumArguments;
    CommandHandler handler;
};

void cmd_ping(CommandContext& context, const RespValue::Array& arguments);
void cmd_echo(CommandContext& context, const RespValue::Array& arguments);
void cmd_quit(CommandContext& context, const RespValue::Array& arguments);

class CommandTable {
public:
    const CommandSpec* find(std::string_view name) const noexcept;
};

}  // namespace redis_clone
