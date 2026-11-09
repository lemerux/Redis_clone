#include "command/command.h"

#include <array>
#include <cctype>
#include <limits>
#include <string>

namespace redis_clone {
namespace {

constexpr std::size_t kUnlimitedArguments =
    std::numeric_limits<std::size_t>::max();

const std::array<CommandSpec, 68> kCommands{{
    {"PING", 1, 2, cmd_ping},
    {"ECHO", 2, 2, cmd_echo},
    {"QUIT", 1, 1, cmd_quit},
    {"AUTH", 3, 3, cmd_auth},
    {"WHOAMI", 1, 1, cmd_whoami},
    {"SET", 3, kUnlimitedArguments, cmd_set},
    {"GET", 2, 2, cmd_get},
    {"INCR", 2, 2, cmd_incr},
    {"DECR", 2, 2, cmd_decr},
    {"INCRBY", 3, 3, cmd_incrby},
    {"APPEND", 3, 3, cmd_append},
    {"STRLEN", 2, 2, cmd_strlen},
    {"HSET", 4, kUnlimitedArguments, cmd_hset},
    {"HGET", 3, 3, cmd_hget},
    {"HDEL", 3, kUnlimitedArguments, cmd_hdel},
    {"HEXISTS", 3, 3, cmd_hexists},
    {"HLEN", 2, 2, cmd_hlen},
    {"HGETALL", 2, 2, cmd_hgetall},
    {"HKEYS", 2, 2, cmd_hkeys},
    {"HVALS", 2, 2, cmd_hvals},
    {"HINCRBY", 4, 4, cmd_hincrby},
    {"LPUSH", 3, kUnlimitedArguments, cmd_lpush},
    {"RPUSH", 3, kUnlimitedArguments, cmd_rpush},
    {"LPOP", 2, 3, cmd_lpop},
    {"RPOP", 2, 3, cmd_rpop},
    {"LLEN", 2, 2, cmd_llen},
    {"LRANGE", 4, 4, cmd_lrange},
    {"LINDEX", 3, 3, cmd_lindex},
    {"LSET", 4, 4, cmd_lset},
    {"SADD", 3, kUnlimitedArguments, cmd_sadd},
    {"SREM", 3, kUnlimitedArguments, cmd_srem},
    {"SISMEMBER", 3, 3, cmd_sismember},
    {"SMEMBERS", 2, 2, cmd_smembers},
    {"SCARD", 2, 2, cmd_scard},
    {"SINTER", 2, kUnlimitedArguments, cmd_sinter},
    {"SUNION", 2, kUnlimitedArguments, cmd_sunion},
    {"SDIFF", 2, kUnlimitedArguments, cmd_sdiff},
    {"SPOP", 2, 3, cmd_spop},
    {"SRANDMEMBER", 2, 3, cmd_srandmember},
    {"ZADD", 4, kUnlimitedArguments, cmd_zadd},
    {"ZREM", 3, kUnlimitedArguments, cmd_zrem},
    {"ZSCORE", 3, 3, cmd_zscore},
    {"ZRANK", 3, 3, cmd_zrank},
    {"ZREVRANK", 3, 3, cmd_zrevrank},
    {"ZCARD", 2, 2, cmd_zcard},
    {"ZRANGE", 4, kUnlimitedArguments, cmd_zrange},
    {"ZRANGEBYSCORE", 4, kUnlimitedArguments, cmd_zrangebyscore},
    {"ZINCRBY", 4, 4, cmd_zincrby},
    {"ZCOUNT", 4, 4, cmd_zcount},
    {"ZREMRANGEBYRANK", 4, 4, cmd_zremrangebyrank},
    {"MGET", 2, kUnlimitedArguments, cmd_mget},
    {"MSET", 3, kUnlimitedArguments, cmd_mset},
    {"DEL", 2, kUnlimitedArguments, cmd_del},
    {"EXISTS", 2, kUnlimitedArguments, cmd_exists},
    {"TYPE", 2, 2, cmd_type},
    {"DBSIZE", 1, 1, cmd_dbsize},
    {"FLUSHALL", 1, 1, cmd_flushall},
    {"KEYS", 2, 2, cmd_keys},
    {"RENAME", 3, 3, cmd_rename},
    {"EXPIRE", 3, 3, cmd_expire},
    {"PEXPIRE", 3, 3, cmd_pexpire},
    {"EXPIREAT", 3, 3, cmd_expireat},
    {"PEXPIREAT", 3, 3, cmd_pexpireat},
    {"TTL", 2, 2, cmd_ttl},
    {"PTTL", 2, 2, cmd_pttl},
    {"PERSIST", 2, 2, cmd_persist},
    {"INFO", 1, 2, cmd_info},
    {"CONFIG", 3, 3, cmd_config},
}};

const std::string& stringArgument(const RespValue::Array& arguments,
                                  std::size_t index) {
    return std::get<BulkString>(arguments[index].data).value;
}

bool commandNameMatches(std::string_view candidate,
                        std::string_view registered) noexcept {
    return asciiCaseEqual(candidate, registered);
}

}  // namespace

bool matchesGlob(std::string_view value, std::string_view pattern) noexcept {
    std::size_t valueIndex = 0;
    std::size_t patternIndex = 0;
    std::size_t starIndex = std::string_view::npos;
    std::size_t retryValueIndex = 0;

    while (valueIndex < value.size()) {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' ||
             pattern[patternIndex] == value[valueIndex])) {
            ++valueIndex;
            ++patternIndex;
        } else if (patternIndex < pattern.size() &&
                   pattern[patternIndex] == '*') {
            starIndex = patternIndex++;
            retryValueIndex = valueIndex;
        } else if (starIndex != std::string_view::npos) {
            patternIndex = starIndex + 1;
            valueIndex = ++retryValueIndex;
        } else {
            return false;
        }
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

bool asciiCaseEqual(std::string_view value, std::string_view expected) noexcept {
    if (value.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(value[i])) != expected[i]) {
            return false;
        }
    }
    return true;
}

void cmd_ping(CommandContext& context, const RespValue::Array& arguments) {
    if (arguments.size() == 1) {
        context.output.append("+PONG\r\n");
    } else {
        RespWriter::writeBulk(context.output, stringArgument(arguments, 1));
    }
}

void cmd_quit(CommandContext& context, const RespValue::Array&) {
    RespWriter::writeOk(context.output);
    context.closeAfterReply = true;
}

void writeWrongTypeError(Buffer& output) {
    RespWriter::writeError(
        output, "WRONGTYPE Operation against a key holding the wrong kind of value");
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
