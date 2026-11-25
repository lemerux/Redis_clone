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

// Looks up `key` for a write. Returns false (after writing WRONGTYPE) when the
// key holds something other than a hash; otherwise `object` is the live value
// or nullptr when the key does not exist.
bool findHashForWrite(CommandContext& context, const std::string& key,
                      Object*& object) {
    object = context.database.find(key);
    if (object != nullptr && std::get_if<Hash>(&object->data) == nullptr) {
        writeWrongTypeError(context.output);
        return false;
    }
    return true;
}

bool requireHash(CommandContext& context, const std::string& key,
                 const Hash*& hash) {
    const Object* object = context.database.find(key);
    hash = object == nullptr ? nullptr : std::get_if<Hash>(&object->data);
    if (object != nullptr && hash == nullptr) {
        writeWrongTypeError(context.output);
        return false;
    }
    return true;
}

void writeHashValues(Buffer& output, const Hash* hash, bool fieldsOnly,
                     bool valuesOnly) {
    const std::size_t size = hash == nullptr ? 0 : hash->values().size();
    RespWriter::writeArrayHeader(output, size * (fieldsOnly || valuesOnly ? 1 : 2));
    if (hash == nullptr) {
        return;
    }
    for (const auto& [field, value] : hash->values()) {
        if (!valuesOnly) {
            RespWriter::writeBulk(output, field);
        }
        if (!fieldsOnly) {
            RespWriter::writeBulk(output, value);
        }
    }
}

}  // namespace

void cmd_hset(CommandContext& context, const RespValue::Array& arguments) {
    if (arguments.size() < 4 || arguments.size() % 2 != 0) {
        RespWriter::writeError(context.output, "ERR wrong number of arguments");
        return;
    }
    const std::string& key = stringArgument(arguments, 1);
    Object* object = nullptr;
    if (!findHashForWrite(context, key, object)) {
        return;
    }
    if (object == nullptr) {
        object = &context.database.create(key, Object{Hash{}});
    }
    std::size_t added = 0;
    context.database.modify(key, *object, [&](Object& value) {
        Hash& hash = std::get<Hash>(value.data);
        for (std::size_t i = 2; i < arguments.size(); i += 2) {
            if (hash.set(stringArgument(arguments, i),
                         stringArgument(arguments, i + 1))) {
                ++added;
            }
        }
    });
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(added));
}

void cmd_hget(CommandContext& context, const RespValue::Array& arguments) {
    const Hash* hash = nullptr;
    if (!requireHash(context, stringArgument(arguments, 1), hash)) {
        return;
    }
    if (hash == nullptr) {
        RespWriter::writeNull(context.output);
        return;
    }
    const auto found = hash->values().find(stringArgument(arguments, 2));
    if (found == hash->values().end()) {
        RespWriter::writeNull(context.output);
    } else {
        RespWriter::writeBulk(context.output, found->second);
    }
}

void cmd_hdel(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    Object* object = nullptr;
    if (!findHashForWrite(context, key, object)) {
        return;
    }
    std::size_t removed = 0;
    if (object != nullptr) {
        context.database.modify(key, *object, [&](Object& value) {
            Hash& hash = std::get<Hash>(value.data);
            for (std::size_t i = 2; i < arguments.size(); ++i) {
                if (hash.erase(stringArgument(arguments, i))) {
                    ++removed;
                }
            }
        });
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(removed));
}

void cmd_hexists(CommandContext& context, const RespValue::Array& arguments) {
    const Hash* hash = nullptr;
    if (!requireHash(context, stringArgument(arguments, 1), hash)) {
        return;
    }
    RespWriter::writeInt(
        context.output,
        hash != nullptr && hash->values().count(stringArgument(arguments, 2)) != 0
            ? 1
            : 0);
}

void cmd_hlen(CommandContext& context, const RespValue::Array& arguments) {
    const Hash* hash = nullptr;
    if (!requireHash(context, stringArgument(arguments, 1), hash)) {
        return;
    }
    RespWriter::writeInt(context.output,
                         hash == nullptr
                             ? 0
                             : static_cast<std::int64_t>(hash->values().size()));
}

void cmd_hgetall(CommandContext& context, const RespValue::Array& arguments) {
    const Hash* hash = nullptr;
    if (!requireHash(context, stringArgument(arguments, 1), hash)) {
        return;
    }
    writeHashValues(context.output, hash, false, false);
}

void cmd_hkeys(CommandContext& context, const RespValue::Array& arguments) {
    const Hash* hash = nullptr;
    if (!requireHash(context, stringArgument(arguments, 1), hash)) {
        return;
    }
    writeHashValues(context.output, hash, true, false);
}

void cmd_hvals(CommandContext& context, const RespValue::Array& arguments) {
    const Hash* hash = nullptr;
    if (!requireHash(context, stringArgument(arguments, 1), hash)) {
        return;
    }
    writeHashValues(context.output, hash, false, true);
}

void cmd_hincrby(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    const std::string& field = stringArgument(arguments, 2);
    std::int64_t amount = 0;
    if (!parseInteger(stringArgument(arguments, 3), amount)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    Object* object = nullptr;
    if (!findHashForWrite(context, key, object)) {
        return;
    }
    std::int64_t current = 0;
    if (object != nullptr) {
        const Hash::Map& values = std::get<Hash>(object->data).values();
        const auto found = values.find(field);
        if (found != values.end() && !parseInteger(found->second, current)) {
            RespWriter::writeError(context.output,
                                   "ERR hash value is not an integer");
            return;
        }
    }
    if ((amount > 0 &&
         current > std::numeric_limits<std::int64_t>::max() - amount) ||
        (amount < 0 &&
         current < std::numeric_limits<std::int64_t>::min() - amount)) {
        RespWriter::writeError(context.output,
                               "ERR increment or decrement would overflow");
        return;
    }
    current += amount;
    if (object == nullptr) {
        object = &context.database.create(key, Object{Hash{}});
    }
    context.database.modify(key, *object, [&](Object& value) {
        std::get<Hash>(value.data).set(field, std::to_string(current));
    });
    RespWriter::writeInt(context.output, current);
}

}  // namespace redis_clone
