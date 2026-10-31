#include "resp/resp.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace redis_clone {
namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::size_t kMaxWriterNestingDepth = 128;

enum class InternalStatus {
    Complete,
    NeedMoreData,
    Error,
};

struct InternalResult {
    InternalStatus status;
    RespValue value{};
    std::string error;
};

bool exceedsCommandLimit(std::size_t consumed, const RespLimits& limits) {
    return consumed > limits.maxCommandBytes;
}

InternalStatus parseLine(std::string_view input, std::size_t& position,
                         std::string_view& line, const RespLimits& limits,
                         std::string& error) {
    const std::size_t start = position;
    for (std::size_t i = start; i < input.size(); ++i) {
        if (input[i] == '\n') {
            if (i == start || input[i - 1] != '\r') {
                error = "line is not terminated by CRLF";
                return InternalStatus::Error;
            }
            line = input.substr(start, i - start - 1);
            position = i + 1;
            if (exceedsCommandLimit(position, limits)) {
                error = "command exceeds configured byte limit";
                return InternalStatus::Error;
            }
            return InternalStatus::Complete;
        }
        if (input[i] == '\r' && i + 1 < input.size() && input[i + 1] != '\n') {
            error = "carriage return is not followed by line feed";
            return InternalStatus::Error;
        }
        if (i - start >= limits.maxCommandBytes) {
            error = "command exceeds configured byte limit";
            return InternalStatus::Error;
        }
    }

    if (input.size() - start > limits.maxCommandBytes) {
        error = "command exceeds configured byte limit";
        return InternalStatus::Error;
    }
    return InternalStatus::NeedMoreData;
}

bool parseInteger(std::string_view text, std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

InternalResult parseValue(std::string_view input, std::size_t& position,
                          std::size_t depth, const RespLimits& limits);

InternalResult parseInlineCommand(std::string_view input, std::size_t& position,
                                  const RespLimits& limits) {
    std::string_view line;
    std::string error;
    const InternalStatus lineStatus = parseLine(input, position, line, limits, error);
    if (lineStatus != InternalStatus::Complete) {
        return {lineStatus, {}, std::move(error)};
    }

    RespValue::Array arguments;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() &&
               (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        const std::size_t start = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            if (static_cast<unsigned char>(line[index]) < 0x20) {
                return {InternalStatus::Error, {}, "invalid control byte in inline command"};
            }
            ++index;
        }
        if (index > start) {
            arguments.emplace_back(BulkString{std::string(line.substr(start, index - start))});
            if (arguments.size() > limits.maxArrayLength) {
                return {InternalStatus::Error, {}, "inline command has too many arguments"};
            }
        }
    }
    if (arguments.empty()) {
        return {InternalStatus::Error, {}, "empty inline command"};
    }
    return {InternalStatus::Complete, RespValue{std::move(arguments)}, {}};
}

InternalResult parseValue(std::string_view input, std::size_t& position,
                          std::size_t depth, const RespLimits& limits) {
    if (depth > limits.maxNestingDepth) {
        return {InternalStatus::Error, {}, "maximum nesting depth exceeded"};
    }
    if (position >= input.size()) {
        return {InternalStatus::NeedMoreData, {}, {}};
    }
    if (exceedsCommandLimit(position, limits)) {
        return {InternalStatus::Error, {}, "command exceeds configured byte limit"};
    }

    const char prefix = input[position++];
    if (prefix == '+' || prefix == '-') {
        std::string_view line;
        std::string error;
        const InternalStatus status = parseLine(input, position, line, limits, error);
        if (status != InternalStatus::Complete) {
            return {status, {}, std::move(error)};
        }
        if (prefix == '+') {
            return {InternalStatus::Complete, RespValue{SimpleString{std::string(line)}}, {}};
        }
        return {InternalStatus::Complete, RespValue{ErrorString{std::string(line)}}, {}};
    }

    if (prefix == ':') {
        std::string_view line;
        std::string error;
        const InternalStatus status = parseLine(input, position, line, limits, error);
        if (status != InternalStatus::Complete) {
            return {status, {}, std::move(error)};
        }
        std::int64_t number = 0;
        if (!parseInteger(line, number)) {
            return {InternalStatus::Error, {}, "invalid RESP integer"};
        }
        return {InternalStatus::Complete, RespValue{number}, {}};
    }

    if (prefix == '$' || prefix == '*') {
        std::string_view line;
        std::string error;
        const InternalStatus lineStatus = parseLine(input, position, line, limits, error);
        if (lineStatus != InternalStatus::Complete) {
            return {lineStatus, {}, std::move(error)};
        }

        std::int64_t length = 0;
        if (!parseInteger(line, length)) {
            return {InternalStatus::Error, {}, "invalid RESP length"};
        }
        if (length == -1) {
            return {InternalStatus::Complete, RespValue{NullValue{}}, {}};
        }
        if (length < 0) {
            return {InternalStatus::Error, {}, "RESP length must not be less than -1"};
        }

        const auto unsignedLength = static_cast<std::uint64_t>(length);
        if (prefix == '$') {
            if (unsignedLength > limits.maxBulkLength) {
                return {InternalStatus::Error, {}, "bulk string exceeds configured limit"};
            }
            const std::size_t bulkLength = static_cast<std::size_t>(unsignedLength);
            if (bulkLength > limits.maxCommandBytes ||
                position > limits.maxCommandBytes - bulkLength) {
                return {InternalStatus::Error, {}, "command exceeds configured byte limit"};
            }
            if (input.size() - position < bulkLength + kCrlf.size()) {
                return {InternalStatus::NeedMoreData, {}, {}};
            }
            const std::size_t trailingPosition = position + bulkLength;
            if (input.substr(trailingPosition, kCrlf.size()) != kCrlf) {
                return {InternalStatus::Error, {}, "bulk string is not terminated by CRLF"};
            }
            std::string value(input.substr(position, bulkLength));
            position = trailingPosition + kCrlf.size();
            if (exceedsCommandLimit(position, limits)) {
                return {InternalStatus::Error, {}, "command exceeds configured byte limit"};
            }
            return {InternalStatus::Complete, RespValue{BulkString{std::move(value)}}, {}};
        }

        if (unsignedLength > limits.maxArrayLength) {
            return {InternalStatus::Error, {}, "array exceeds configured limit"};
        }
        RespValue::Array values;
        values.reserve(std::min(static_cast<std::size_t>(unsignedLength),
                                (input.size() - position) / 3));
        for (std::uint64_t i = 0; i < unsignedLength; ++i) {
            InternalResult child = parseValue(input, position, depth + 1, limits);
            if (child.status != InternalStatus::Complete) {
                return child;
            }
            values.push_back(std::move(child.value));
        }
        return {InternalStatus::Complete, RespValue{std::move(values)}, {}};
    }

    if (limits.allowInlineCommands) {
        --position;
        return parseInlineCommand(input, position, limits);
    }
    return {InternalStatus::Error, {}, "unsupported RESP type prefix"};
}

void appendCheckedLine(Buffer& output, char prefix, std::string_view value) {
    if (value.find('\r') != std::string_view::npos ||
        value.find('\n') != std::string_view::npos) {
        throw std::invalid_argument("RESP line values must not contain CR or LF");
    }
    std::string encoded;
    encoded.reserve(value.size() + 3);
    encoded.push_back(prefix);
    encoded.append(value);
    encoded.append(kCrlf);
    output.append(encoded);
}

void writeRespValue(Buffer& output, const RespValue& value, std::size_t depth) {
    if (depth > kMaxWriterNestingDepth) {
        throw std::invalid_argument("RESP value exceeds maximum nesting depth");
    }
    if (const auto* simple = std::get_if<SimpleString>(&value.data)) {
        appendCheckedLine(output, '+', simple->value);
    } else if (const auto* error = std::get_if<ErrorString>(&value.data)) {
        appendCheckedLine(output, '-', error->value);
    } else if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
        RespWriter::writeInt(output, *integer);
    } else if (const auto* bulk = std::get_if<BulkString>(&value.data)) {
        RespWriter::writeBulk(output, bulk->value);
    } else if (const auto* array = std::get_if<RespValue::Array>(&value.data)) {
        RespWriter::writeArrayHeader(output, array->size());
        for (const RespValue& item : *array) {
            writeRespValue(output, item, depth + 1);
        }
    } else {
        RespWriter::writeNull(output);
    }
}

}  // namespace

void Buffer::append(std::string_view data) {
    if (data.empty()) {
        return;
    }
    makeWritable(data.size());
    std::copy(data.begin(), data.end(), storage_.begin() +
                                             static_cast<std::ptrdiff_t>(writePosition_));
    writePosition_ += data.size();
}

std::string_view Buffer::readable() const noexcept {
    if (storage_.empty()) {
        return {};
    }
    return {storage_.data() + readPosition_, readableSize()};
}

void Buffer::consume(std::size_t count) {
    if (count > readableSize()) {
        throw std::out_of_range("cannot consume beyond readable buffer data");
    }
    readPosition_ += count;
    if (readPosition_ == writePosition_) {
        clear();
    }
}

std::size_t Buffer::readableSize() const noexcept {
    return writePosition_ - readPosition_;
}

bool Buffer::empty() const noexcept {
    return readableSize() == 0;
}

void Buffer::clear() noexcept {
    readPosition_ = 0;
    writePosition_ = 0;
}

void Buffer::makeWritable(std::size_t required) {
    if (required > std::numeric_limits<std::size_t>::max() - writePosition_) {
        throw std::length_error("buffer size overflow");
    }
    if (storage_.size() - writePosition_ >= required) {
        return;
    }

    const std::size_t readable = readableSize();
    if (storage_.size() - readable >= required) {
        std::move(storage_.begin() + static_cast<std::ptrdiff_t>(readPosition_),
                  storage_.begin() + static_cast<std::ptrdiff_t>(writePosition_),
                  storage_.begin());
        readPosition_ = 0;
        writePosition_ = readable;
        return;
    }

    if (required > std::numeric_limits<std::size_t>::max() - readable) {
        throw std::length_error("buffer size overflow");
    }
    const std::size_t needed = readable + required;
    std::size_t capacity = std::max<std::size_t>(storage_.size(), 64);
    while (capacity < needed) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    std::vector<char> expanded(capacity);
    std::copy(storage_.begin() + static_cast<std::ptrdiff_t>(readPosition_),
              storage_.begin() + static_cast<std::ptrdiff_t>(writePosition_),
              expanded.begin());
    storage_.swap(expanded);
    readPosition_ = 0;
    writePosition_ = readable;
}

RespParser::RespParser(RespLimits limits) : limits_(limits) {
    if (limits_.maxCommandBytes == 0 || limits_.maxNestingDepth == 0) {
        throw std::invalid_argument("RESP command and nesting limits must be positive");
    }
}

ParseResult RespParser::parse(Buffer& buffer) const {
    std::size_t position = 0;
    const std::string_view input = buffer.readable();
    InternalResult result = parseValue(input, position, 0, limits_);
    if (result.status == InternalStatus::NeedMoreData) {
        return {ParseStatus::NeedMoreData, {}, {}};
    }
    if (result.status == InternalStatus::Error) {
        return {ParseStatus::ProtocolError, {}, std::move(result.error)};
    }
    buffer.consume(position);
    return {ParseStatus::Command, std::move(result.value), {}};
}

void RespWriter::writeOk(Buffer& output) {
    output.append("+OK\r\n");
}

void RespWriter::writeError(Buffer& output, std::string_view message) {
    appendCheckedLine(output, '-', message);
}

void RespWriter::writeInt(Buffer& output, std::int64_t value) {
    output.append(":");
    output.append(std::to_string(value));
    output.append(kCrlf);
}

void RespWriter::writeBulk(Buffer& output, std::string_view value) {
    output.append("$");
    output.append(std::to_string(value.size()));
    output.append(kCrlf);
    output.append(value);
    output.append(kCrlf);
}

void RespWriter::writeNull(Buffer& output) {
    output.append("$-1\r\n");
}

void RespWriter::writeArrayHeader(Buffer& output, std::size_t length) {
    output.append("*");
    output.append(std::to_string(length));
    output.append(kCrlf);
}

void RespWriter::writeValue(Buffer& output, const RespValue& value) {
    writeRespValue(output, value, 0);
}

}  // namespace redis_clone
