#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace redis_clone {

class Buffer {
public:
    void append(std::string_view data);
    std::string_view readable() const noexcept;
    void consume(std::size_t count);
    std::size_t readableSize() const noexcept;
    bool empty() const noexcept;
    void clear() noexcept;

private:
    void makeWritable(std::size_t required);

    std::vector<char> storage_;
    std::size_t readPosition_ = 0;
    std::size_t writePosition_ = 0;
};

struct SimpleString {
    std::string value;
};

struct ErrorString {
    std::string value;
};

struct BulkString {
    std::string value;
};

struct NullValue {};

struct RespValue {
    using Array = std::vector<RespValue>;
    using Storage =
        std::variant<SimpleString, ErrorString, std::int64_t, BulkString, Array, NullValue>;

    RespValue() = default;
    explicit RespValue(Storage value) : data(std::move(value)) {}

    Storage data;
};

struct RespLimits {
    std::size_t maxBulkLength = 512ULL * 1024 * 1024;
    std::size_t maxArrayLength = 1'000'000;
    std::size_t maxCommandBytes = 64 * 1024;
    std::size_t maxNestingDepth = 128;
    bool allowInlineCommands = false;
};

enum class ParseStatus {
    NeedMoreData,
    Command,
    ProtocolError,
};

struct ParseResult {
    ParseStatus status;
    RespValue value;
    std::string error;
};

class RespParser {
public:
    explicit RespParser(RespLimits limits = {});

    ParseResult parse(Buffer& buffer) const;

private:
    RespLimits limits_;
};

class RespWriter {
public:
    static void writeOk(Buffer& output);
    static void writeError(Buffer& output, std::string_view message);
    static void writeInt(Buffer& output, std::int64_t value);
    static void writeBulk(Buffer& output, std::string_view value);
    static void writeNull(Buffer& output);
    static void writeArrayHeader(Buffer& output, std::size_t length);
    static void writeValue(Buffer& output, const RespValue& value);
};

}  // namespace redis_clone
