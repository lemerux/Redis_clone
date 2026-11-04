#pragma once

#include "resp/resp.h"
#include "store/database.h"

#include <cstddef>
#include <string_view>

namespace redis_clone {

struct CommandContext {
    Buffer& output;
    bool& closeAfterReply;
    Database& database;
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
void cmd_set(CommandContext& context, const RespValue::Array& arguments);
void cmd_get(CommandContext& context, const RespValue::Array& arguments);
void cmd_incr(CommandContext& context, const RespValue::Array& arguments);
void cmd_decr(CommandContext& context, const RespValue::Array& arguments);
void cmd_incrby(CommandContext& context, const RespValue::Array& arguments);
void cmd_append(CommandContext& context, const RespValue::Array& arguments);
void cmd_strlen(CommandContext& context, const RespValue::Array& arguments);
void cmd_mget(CommandContext& context, const RespValue::Array& arguments);
void cmd_mset(CommandContext& context, const RespValue::Array& arguments);
void cmd_del(CommandContext& context, const RespValue::Array& arguments);
void cmd_exists(CommandContext& context, const RespValue::Array& arguments);
void cmd_type(CommandContext& context, const RespValue::Array& arguments);
void cmd_dbsize(CommandContext& context, const RespValue::Array& arguments);
void cmd_flushall(CommandContext& context, const RespValue::Array& arguments);
void cmd_keys(CommandContext& context, const RespValue::Array& arguments);
void cmd_scan(CommandContext& context, const RespValue::Array& arguments);
void cmd_rename(CommandContext& context, const RespValue::Array& arguments);
void cmd_info(CommandContext& context, const RespValue::Array& arguments);
void cmd_config(CommandContext& context, const RespValue::Array& arguments);

void writeWrongTypeError(Buffer& output);
bool matchesGlob(std::string_view value, std::string_view pattern) noexcept;
bool asciiCaseEqual(std::string_view value, std::string_view expected) noexcept;

class CommandTable {
public:
    const CommandSpec* find(std::string_view name) const noexcept;
};

}  // namespace redis_clone
