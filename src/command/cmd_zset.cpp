#include "command/command.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

bool parseScore(std::string_view text, double& value) {
    if (text.empty()) {
        return false;
    }
    std::string terminated(text);
    char* end = nullptr;
    errno = 0;
    value = std::strtod(terminated.c_str(), &end);
    return end == terminated.c_str() + terminated.size() && errno != ERANGE &&
           !std::isnan(value);
}

std::string scoreString(double score) {
    if (std::isinf(score)) {
        return score < 0 ? "-inf" : "inf";
    }
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), score,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("failed to format sorted-set score");
    }
    return std::string(buffer, result.ptr);
}

bool parseScoreBound(std::string_view text, double& score, bool& exclusive) {
    exclusive = !text.empty() && text.front() == '(';
    if (exclusive) {
        text.remove_prefix(1);
    }
    return parseScore(text, score);
}

bool requireZSet(CommandContext& context, const std::string& key,
                 const Object*& object, const ZSet*& zset) {
    object = context.database.find(key);
    if (object == nullptr) {
        zset = nullptr;
        return true;
    }
    const auto* holder = std::get_if<std::unique_ptr<ZSet>>(&object->data);
    if (holder == nullptr || *holder == nullptr) {
        writeWrongTypeError(context.output);
        return false;
    }
    zset = holder->get();
    return true;
}

void writeEntries(Buffer& output, const std::vector<ZSet::Entry>& entries,
                  bool withScores) {
    RespWriter::writeArrayHeader(output,
                                 entries.size() * (withScores ? 2 : 1));
    for (const auto& [score, member] : entries) {
        RespWriter::writeBulk(output, member);
        if (withScores) {
            RespWriter::writeBulk(output, scoreString(score));
        }
    }
}

std::vector<ZSet::Entry> selectRankRange(const ZSet& zset,
                                         std::int64_t start,
                                         std::int64_t stop, bool reverse) {
    if (!reverse) {
        return zset.rangeByRank(start, stop);
    }
    const std::size_t size = zset.size();
    if (size == 0 ||
        size > static_cast<std::size_t>(
                   std::numeric_limits<std::int64_t>::max())) {
        return {};
    }
    const std::int64_t length = static_cast<std::int64_t>(size);
    if (start < 0) {
        start = std::max<std::int64_t>(0, length + start);
    }
    if (stop < 0) {
        stop = length + stop;
    }
    if (start >= length || stop < start) {
        return {};
    }
    stop = std::min(stop, length - 1);
    std::vector<ZSet::Entry> entries =
        zset.rangeByRank(length - 1 - stop, length - 1 - start);
    std::reverse(entries.begin(), entries.end());
    return entries;
}

}  // namespace
bool getMutableZSet(CommandContext& context, const std::string& key,
                    Object*& object, ZSet*& zset) {
    object = context.database.find(key);
    if (object == nullptr) {
        zset = nullptr;
        return true;
    }
    auto* holder = std::get_if<std::unique_ptr<ZSet>>(&object->data);
    if (holder == nullptr || *holder == nullptr) {
        writeWrongTypeError(context.output);
        return false;
    }
    zset = holder->get();
    return true;
}
void cmd_zadd(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    bool nx = false;
    bool xx = false;
    bool gt = false;
    bool lt = false;
    bool changedOnly = false;
    std::size_t pairStart = 2;
    while (pairStart < arguments.size()) {
        const std::string& option = stringArgument(arguments, pairStart);
        if (asciiCaseEqual(option, "NX")) {
            nx = true;
        } else if (asciiCaseEqual(option, "XX")) {
            xx = true;
        } else if (asciiCaseEqual(option, "GT")) {
            gt = true;
        } else if (asciiCaseEqual(option, "LT")) {
            lt = true;
        } else if (asciiCaseEqual(option, "CH")) {
            changedOnly = true;
        } else {
            break;
        }
        ++pairStart;
    }
    if (pairStart >= arguments.size() || (arguments.size() - pairStart) % 2 != 0 ||
        (nx && xx) || (gt && lt) || (nx && (gt || lt))) {
        RespWriter::writeError(context.output, "ERR syntax error");
        return;
    }

    std::vector<ZSet::Entry> updates;
    updates.reserve((arguments.size() - pairStart) / 2);
    for (std::size_t i = pairStart; i < arguments.size(); i += 2) {
        double score = 0.0;
        if (!parseScore(stringArgument(arguments, i), score)) {
            RespWriter::writeError(context.output,
                                   "ERR value is not a valid float");
            return;
        }
        updates.emplace_back(score, stringArgument(arguments, i + 1));
    }

    Object* object = nullptr;
    ZSet* zset = nullptr;
    if (!getMutableZSet(context, key, object, zset)) {
        return;
    }

    if (object == nullptr) {
        object = &context.database.findOrCreate(key);
        object->type = ObjType::ZSet;
        object->data = std::make_unique<ZSet>();
        zset = std::get<std::unique_ptr<ZSet>>(object->data).get();
    }

    std::size_t added = 0;
    std::size_t changed = 0;
    for (const auto& [newScore, member] : updates) {
        const std::optional<double> oldScore = zset->score(member);
        if (oldScore.has_value()) {
            if (nx || (gt && newScore <= *oldScore) ||
                (lt && newScore >= *oldScore)) {
                continue;
            }
            if (*oldScore != newScore && zset->updateScore(member, newScore)) {
                ++changed;
            }
        } else if (!xx && zset->add(newScore, member)) {
            ++added;
            ++changed;
        }
    }
    RespWriter::writeInt(context.output,
                         static_cast<std::int64_t>(changedOnly ? changed : added));
}

void cmd_zrem(CommandContext& context, const RespValue::Array& arguments) {
    const std::string& key = stringArgument(arguments, 1);
    const Object* object = nullptr;
    const ZSet* existing = nullptr;
    if (!requireZSet(context, key, object, existing)) {
        return;
    }
    if (existing == nullptr) {
        RespWriter::writeInt(context.output, 0);
        return;
    }
    ZSet zset = *existing;
    std::size_t removed = 0;
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        removed += zset.remove(stringArgument(arguments, i)) ? 1 : 0;
    }
    if (removed != 0) {
        if (zset.size() == 0) {
            context.database.erase(key);
        } else {
            context.database.setObject(key, Object{std::move(zset)}, true);
        }
    }
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(removed));
}

void cmd_zscore(CommandContext& context, const RespValue::Array& arguments) {
    const Object* object = nullptr;
    const ZSet* zset = nullptr;
    if (!requireZSet(context, stringArgument(arguments, 1), object, zset)) {
        return;
    }
    if (zset == nullptr) {
        RespWriter::writeNull(context.output);
        return;
    }
    const auto score = zset->score(stringArgument(arguments, 2));
    if (!score.has_value()) {
        RespWriter::writeNull(context.output);
        return;
    }
    RespWriter::writeBulk(context.output, scoreString(*score));
}

void cmd_zrank(CommandContext& context, const RespValue::Array& arguments) {
    const Object* object = nullptr;
    const ZSet* zset = nullptr;
    if (!requireZSet(context, stringArgument(arguments, 1), object, zset)) {
        return;
    }
    const auto score = zset == nullptr
                           ? std::nullopt
                           : zset->score(stringArgument(arguments, 2));
    const auto rank = score.has_value()
                          ? zset->rank(*score, stringArgument(arguments, 2))
                          : std::nullopt;
    if (!rank.has_value()) {
        RespWriter::writeNull(context.output);
    } else {
        RespWriter::writeInt(context.output, static_cast<std::int64_t>(*rank));
    }
}

void cmd_zrevrank(CommandContext& context,
                  const RespValue::Array& arguments) {
    const Object* object = nullptr;
    const ZSet* zset = nullptr;
    if (!requireZSet(context, stringArgument(arguments, 1), object, zset)) {
        return;
    }
    const auto score = zset == nullptr
                           ? std::nullopt
                           : zset->score(stringArgument(arguments, 2));
    const auto rank = score.has_value()
                          ? zset->rank(*score, stringArgument(arguments, 2))
                          : std::nullopt;
    if (!rank.has_value()) {
        RespWriter::writeNull(context.output);
    } else {
        RespWriter::writeInt(
            context.output,
            static_cast<std::int64_t>(zset->size() - 1 - *rank));
    }
}

void cmd_zcard(CommandContext& context, const RespValue::Array& arguments) {
    const Object* object = nullptr;
    const ZSet* zset = nullptr;
    if (!requireZSet(context, stringArgument(arguments, 1), object, zset)) {
        return;
    }
    RespWriter::writeInt(
        context.output,
        zset == nullptr ? 0 : static_cast<std::int64_t>(zset->size()));
}

void cmd_zrange(CommandContext& context, const RespValue::Array& arguments) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!parseInteger(stringArgument(arguments, 2), start) ||
        !parseInteger(stringArgument(arguments, 3), stop)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    bool withScores = false;
    bool reverse = false;
    for (std::size_t i = 4; i < arguments.size(); ++i) {
        const std::string& option = stringArgument(arguments, i);
        if (asciiCaseEqual(option, "WITHSCORES")) {
            withScores = true;
        } else if (asciiCaseEqual(option, "REV")) {
            reverse = true;
        } else {
            RespWriter::writeError(context.output, "ERR syntax error");
            return;
        }
    }
    const Object* object = nullptr;
    const ZSet* zset = nullptr;
    if (!requireZSet(context, stringArgument(arguments, 1), object, zset)) {
        return;
    }
    writeEntries(context.output,
                 zset == nullptr ? std::vector<ZSet::Entry>{}
                                 : selectRankRange(*zset, start, stop, reverse),
                 withScores);
}

void cmd_zrangebyscore(CommandContext& context,
                       const RespValue::Array& arguments) {
    double minimum = 0.0;
    double maximum = 0.0;
    bool minimumExclusive = false;
    bool maximumExclusive = false;
    if (!parseScoreBound(stringArgument(arguments, 2), minimum,
                         minimumExclusive) ||
        !parseScoreBound(stringArgument(arguments, 3), maximum,
                         maximumExclusive)) {
        RespWriter::writeError(context.output,
                               "ERR min or max is not a float");
        return;
    }
    bool withScores = false;
    bool hasLimit = false;
    std::int64_t offset = 0;
    std::int64_t count = -1;
    for (std::size_t i = 4; i < arguments.size(); ++i) {
        const std::string& option = stringArgument(arguments, i);
        if (asciiCaseEqual(option, "WITHSCORES")) {
            withScores = true;
        } else if (asciiCaseEqual(option, "LIMIT")) {
            if (hasLimit || i + 2 >= arguments.size() ||
                !parseInteger(stringArgument(arguments, i + 1), offset) ||
                !parseInteger(stringArgument(arguments, i + 2), count) ||
                offset < 0 || count < -1) {
                RespWriter::writeError(context.output,
                                       "ERR syntax error");
                return;
            }
            hasLimit = true;
            i += 2;
        } else {
            RespWriter::writeError(context.output, "ERR syntax error");
            return;
        }
    }

    const Object* object = nullptr;
    const ZSet* zset = nullptr;
    if (!requireZSet(context, stringArgument(arguments, 1), object, zset)) {
        return;
    }
    std::vector<ZSet::Entry> entries =
        zset == nullptr
            ? std::vector<ZSet::Entry>{}
            : zset->rangeByScore(minimum, maximum, minimumExclusive,
                                 maximumExclusive);
    if (hasLimit) {
        const std::size_t begin =
            std::min<std::size_t>(static_cast<std::size_t>(offset),
                                  entries.size());
        const std::size_t end =
            count < 0
                ? entries.size()
                : begin + std::min<std::size_t>(
                              static_cast<std::size_t>(count),
                              entries.size() - begin);
        entries = std::vector<ZSet::Entry>(entries.begin() + begin,
                                           entries.begin() + end);
    }
    writeEntries(context.output, entries, withScores);
}

void cmd_zincrby(CommandContext& context, const RespValue::Array& arguments) {
    double increment = 0.0;
    if (!parseScore(stringArgument(arguments, 2), increment)) {
        RespWriter::writeError(context.output,
                               "ERR value is not a valid float");
        return;
    }
    const std::string& key = stringArgument(arguments, 1);
    const Object* existing = context.database.find(key);
    const auto* holder =
        existing == nullptr
            ? nullptr
            : std::get_if<std::unique_ptr<ZSet>>(&existing->data);
    if (existing != nullptr && holder == nullptr) {
        writeWrongTypeError(context.output);
        return;
    }
    ZSet zset = (holder == nullptr || *holder == nullptr) ? ZSet{} : **holder;
    const std::string& member = stringArgument(arguments, 3);
    const double newScore = zset.score(member).value_or(0.0) + increment;
    if (std::isnan(newScore)) {
        RespWriter::writeError(context.output,
                               "ERR resulting score is not a number (NaN)");
        return;
    }
    zset.add(newScore, member);
    context.database.setObject(key, Object{std::move(zset)}, true);
    RespWriter::writeBulk(context.output, scoreString(newScore));
}

void cmd_zcount(CommandContext& context, const RespValue::Array& arguments) {
    double minimum = 0.0;
    double maximum = 0.0;
    bool minimumExclusive = false;
    bool maximumExclusive = false;
    if (!parseScoreBound(stringArgument(arguments, 2), minimum,
                         minimumExclusive) ||
        !parseScoreBound(stringArgument(arguments, 3), maximum,
                         maximumExclusive)) {
        RespWriter::writeError(context.output,
                               "ERR min or max is not a float");
        return;
    }
    const Object* object = nullptr;
    const ZSet* zset = nullptr;
    if (!requireZSet(context, stringArgument(arguments, 1), object, zset)) {
        return;
    }
    const std::size_t count =
        zset == nullptr
            ? 0
            : zset->rangeByScore(minimum, maximum, minimumExclusive,
                                 maximumExclusive)
                  .size();
    RespWriter::writeInt(context.output, static_cast<std::int64_t>(count));
}

void cmd_zremrangebyrank(CommandContext& context,
                         const RespValue::Array& arguments) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!parseInteger(stringArgument(arguments, 2), start) ||
        !parseInteger(stringArgument(arguments, 3), stop)) {
        RespWriter::writeError(context.output,
                               "ERR value is not an integer or out of range");
        return;
    }
    const std::string& key = stringArgument(arguments, 1);
    const Object* object = nullptr;
    const ZSet* existing = nullptr;
    if (!requireZSet(context, key, object, existing)) {
        return;
    }
    if (existing == nullptr) {
        RespWriter::writeInt(context.output, 0);
        return;
    }
    ZSet zset = *existing;
    const std::vector<ZSet::Entry> entries = zset.rangeByRank(start, stop);
    for (const auto& [score, member] : entries) {
        (void)score;
        zset.remove(member);
    }
    if (!entries.empty()) {
        if (zset.size() == 0) {
            context.database.erase(key);
        } else {
            context.database.setObject(key, Object{std::move(zset)}, true);
        }
    }
    RespWriter::writeInt(context.output,
                         static_cast<std::int64_t>(entries.size()));
}

}  // namespace redis_clone
