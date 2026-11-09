#include "command/command.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
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

bool requireSet(CommandContext& context, const std::string& key,
                const Object*& object, const Set*& set) {
    object = context.database.find(key);
    set = object == nullptr ? nullptr : std::get_if<Set>(&object->data);
    if (object != nullptr && set == nullptr) {
        writeWrongTypeError(context.output);
        return false;
    }
    return true;
}

bool collectSets(CommandContext& context, const RespValue::Array& arguments,
                 std::vector<const Set*>& sets) {
    sets.reserve(arguments.size() - 1);
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const Object* object = nullptr;
        const Set* set = nullptr;
        if (!requireSet(context, stringArgument(arguments, i), object, set)) {
            return false;
        }
        sets.push_back(set);
    }
    return true;
}

std::mt19937_64& randomEngine() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

std::vector<std::string> randomElements(const Set& set, std::size_t count,
                                        bool allowDuplicates) {
    std::vector<std::string> elements(set.values.begin(), set.values.end());
    std::vector<std::string> result;
    if (elements.empty() || count == 0) {
        return result;
    }
    if (!allowDuplicates) {
        count = std::min(count, elements.size());
        for (std::size_t i = 0; i < count; ++i) {
            std::uniform_int_distribution<std::size_t> distribution(
                i, elements.size() - 1);
            std::swap(elements[i], elements[distribution(randomEngine())]);
            result.push_back(elements[i]);
        }
        return result;
    }
    result.reserve(count);
    std::uniform_int_distribution<std::size_t> distribution(0,
                                                            elements.size() - 1);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(elements[distribution(randomEngine())]);
    }
    return result;
}

void writeStringArray(Buffer& output, const std::vector<std::string>& values) {
    RespWriter::writeArrayHeader(output, values.size());
    for (const std::string& value : values) {
        RespWriter::writeBulk(output, value);
    }
}

void setOperation(CommandContext& context, const RespValue::Array& arguments,
                  int operation) {
    std::vector<const Set*> sets;
    if (!collectSets(context, arguments, sets)) {
        return;
    }
    std::unordered_set<std::string> result;
    if (operation == 0) {
        for (const Set* set : sets) {
            if (set != nullptr) {
                result.insert(set->values.begin(), set->values.end());
            }
        }
    } else if (operation == 1) {
        if (!sets.empty() && sets.front() != nullptr) {
            result = sets.front()->values;
            for (std::size_t i = 1; i < sets.size(); ++i) {
                if (sets[i] == nullptr) {
                    result.clear();
                    break;
                }
                for (auto it = result.begin(); it != result.end();) {
                    if (sets[i]->values.count(*it) == 0) {
                        it = result.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
    } else if (!sets.empty() && sets.front() != nullptr) {
        result = sets.front()->values;
        for (std::size_t i = 1; i < sets.size(); ++i) {
            if (sets[i] == nullptr) {
                continue;
            }
            for (const std::string& value : sets[i]->values) {
                result.erase(value);
            }
        }
    }
    std::vector<std::string> values(result.begin(), result.end());
    writeStringArray(context.output, values);
}

}  // namespace

void cmd_sadd(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    const Object* existing = context.database.find(key);
    if (existing != nullptr && std::get_if<Set>(&existing->data) == nullptr) {
        writeWrongTypeError(context.output);
        return;
    }
    Set set = existing == nullptr ? Set{} : std::get<Set>(existing->data);
    std::size_t added = 0;
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        added += set.values.emplace(stringArgument(arguments, i)).second ? 1 : 0;
    }
    if (added != 0) {
        context.database.setObject(key, Object{std::move(set)}, true);
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(added));
}

void cmd_srem(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    const Object* object = nullptr;
    const Set* existing = nullptr;
    if (!requireSet(context, key, object, existing)) {
        return;
    }
    if (existing == nullptr) {
        RespWriter::writeInt(context.output, 0);
        return;
    }
    Set updated = *existing;
    std::size_t removed = 0;
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        removed += updated.values.erase(stringArgument(arguments, i));
    }
    if (removed != 0) {
        if (updated.values.empty()) {
            context.database.erase(key);
        } else {
            context.database.setObject(key, Object{std::move(updated)}, true);
        }
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(removed));
}

void cmd_sismember(CommandContext& context,
                   const RespValue::Array& arguments) {
    const Object* object = nullptr;
    const Set* set = nullptr;
    if (!requireSet(context, stringArgument(arguments, 1), object, set)) {
        return;
    }
    RespWriter::writeInt(
        context.output,
        set != nullptr && set->values.count(stringArgument(arguments, 2)) != 0
            ? 1
            : 0);
}

void cmd_smembers(CommandContext& context, const RespValue::Array& arguments) {
    const Object* object = nullptr;
    const Set* set = nullptr;
    if (!requireSet(context, stringArgument(arguments, 1), object, set)) {
        return;
    }
    std::vector<std::string> values;
    if (set != nullptr) {
        values.assign(set->values.begin(), set->values.end());
    }
    writeStringArray(context.output, values);
}

void cmd_scard(CommandContext& context, const RespValue::Array& arguments) {
    const Object* object = nullptr;
    const Set* set = nullptr;
    if (!requireSet(context, stringArgument(arguments, 1), object, set)) {
        return;
    }
    RespWriter::writeInt(
        context.output,
        set == nullptr ? 0 : static_cast<std::int64_t>(set->values.size()));
}

void cmd_sinter(CommandContext& context, const RespValue::Array& arguments) {
    setOperation(context, arguments, 1);
}

void cmd_sunion(CommandContext& context, const RespValue::Array& arguments) {
    setOperation(context, arguments, 0);
}

void cmd_sdiff(CommandContext& context, const RespValue::Array& arguments) {
    setOperation(context, arguments, 2);
}

void cmd_spop(CommandContext& context, const RespValue::Array& arguments) {
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
    const Set* set = nullptr;
    if (!requireSet(context, key, object, set)) {
        return;
    }
    if (set == nullptr) {
        if (withCount) {
            RespWriter::writeArrayHeader(context.output, 0);
        } else {
            RespWriter::writeNull(context.output);
        }
        return;
    }
    Set updated = *set;
    const auto values = randomElements(
        updated, static_cast<std::size_t>(std::min<std::uint64_t>(
                     static_cast<std::uint64_t>(count), updated.values.size())),
        false);
    for (const std::string& value : values) {
        updated.values.erase(value);
    }
    if (!values.empty()) {
        if (updated.values.empty()) {
            context.database.erase(key);
        } else {
            context.database.setObject(key, Object{std::move(updated)}, true);
        }
    }
    if (withCount) {
        writeStringArray(context.output, values);
    } else if (values.empty()) {
        RespWriter::writeNull(context.output);
    } else {
        RespWriter::writeBulk(context.output, values.front());
    }
}

void cmd_srandmember(CommandContext& context,
                     const RespValue::Array& arguments) {
    const bool withCount = arguments.size() == 3;
    std::int64_t count = 1;
    if (withCount && !parseInteger(stringArgument(arguments, 2), count)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    const Object* object = nullptr;
    const Set* set = nullptr;
    if (!requireSet(context, stringArgument(arguments, 1), object, set)) {
        return;
    }
    if (!withCount) {
        if (set == nullptr) {
            RespWriter::writeNull(context.output);
        } else {
            const auto values = randomElements(*set, 1, false);
            RespWriter::writeBulk(context.output, values.front());
        }
        return;
    }
    constexpr std::uint64_t kMaxReturnedMembers = 1'000'000;
    const std::uint64_t magnitude =
        count < 0 ? static_cast<std::uint64_t>(-(count + 1)) + 1
                  : static_cast<std::uint64_t>(count);
    if (magnitude > kMaxReturnedMembers) {
        RespWriter::writeError(context.output, "ERR count is out of range");
        return;
    }
    const bool duplicates = count < 0;
    const std::size_t requested = static_cast<std::size_t>(magnitude);
    const auto values =
        set == nullptr ? std::vector<std::string>{}
                       : randomElements(*set, requested, duplicates);
    writeStringArray(context.output, values);
}

}  // namespace redis_clone
