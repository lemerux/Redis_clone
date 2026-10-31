#include "resp/resp.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using redis_clone::Buffer;
using redis_clone::BulkString;
using redis_clone::ParseStatus;
using redis_clone::RespParser;
using redis_clone::RespValue;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectCommand(RespParser& parser, Buffer& buffer, const RespValue::Array& expected) {
    const auto result = parser.parse(buffer);
    expect(result.status == ParseStatus::Command, "expected a parsed command");
    const auto* array = std::get_if<RespValue::Array>(&result.value.data);
    expect(array != nullptr && array->size() == expected.size(),
           "parsed command has unexpected array size");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto* actualString = std::get_if<BulkString>(&(*array)[i].data);
        const auto* expectedString = std::get_if<BulkString>(&expected[i].data);
        expect(actualString != nullptr && expectedString != nullptr &&
                   actualString->value == expectedString->value,
               "parsed command argument differs");
    }
}

RespValue::Array command(std::initializer_list<std::string> arguments) {
    RespValue::Array values;
    for (const std::string& argument : arguments) {
        values.emplace_back(BulkString{argument});
    }
    return values;
}

void testIncrementalParsing() {
    const std::string encoded = "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    const auto expected = command({"ECHO", "hello"});
    RespParser parser;

    for (std::size_t split = 0; split <= encoded.size(); ++split) {
        Buffer buffer;
        buffer.append(std::string_view(encoded).substr(0, split));
        if (split < encoded.size()) {
            expect(parser.parse(buffer).status == ParseStatus::NeedMoreData,
                   "partial command should need more data");
        }
        buffer.append(std::string_view(encoded).substr(split));
        expectCommand(parser, buffer, expected);
        expect(buffer.empty(), "parsed command bytes should be consumed");
    }

    Buffer bytewise;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char byte = encoded[i];
        bytewise.append(std::string_view(&byte, 1));
        const auto result = parser.parse(bytewise);
        if (i + 1 < encoded.size()) {
            expect(result.status == ParseStatus::NeedMoreData,
                   "bytewise input should remain incomplete until complete");
        } else {
            expect(result.status == ParseStatus::Command,
                   "bytewise input should produce a command");
        }
    }
}

void testPipelining() {
    Buffer buffer;
    buffer.append("*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n");
    RespParser parser;
    expectCommand(parser, buffer, command({"PING"}));
    expect(!buffer.empty(), "second pipelined command should remain buffered");
    expectCommand(parser, buffer, command({"GET", "x"}));
    expect(buffer.empty(), "all pipelined command bytes should be consumed");
}

void testRespValueTypes() {
    Buffer buffer;
    buffer.append("+OK\r\n-ERR bad\r\n:42\r\n$3\r\nfoo\r\n$-1\r\n*-1\r\n");
    RespParser parser;

    auto result = parser.parse(buffer);
    const auto* simple = std::get_if<redis_clone::SimpleString>(&result.value.data);
    expect(result.status == ParseStatus::Command && simple != nullptr &&
               simple->value == "OK",
           "simple string was not parsed");
    result = parser.parse(buffer);
    const auto* error = std::get_if<redis_clone::ErrorString>(&result.value.data);
    expect(result.status == ParseStatus::Command && error != nullptr &&
               error->value == "ERR bad",
           "error string was not parsed");
    result = parser.parse(buffer);
    expect(result.status == ParseStatus::Command &&
               std::get_if<std::int64_t>(&result.value.data) != nullptr &&
               std::get<std::int64_t>(result.value.data) == 42,
           "integer was not parsed");
    result = parser.parse(buffer);
    const auto* bulk = std::get_if<BulkString>(&result.value.data);
    expect(result.status == ParseStatus::Command && bulk != nullptr &&
               bulk->value == "foo",
           "bulk string was not parsed");
    result = parser.parse(buffer);
    expect(result.status == ParseStatus::Command &&
               std::get_if<redis_clone::NullValue>(&result.value.data) != nullptr,
           "null bulk string was not parsed");
    result = parser.parse(buffer);
    expect(result.status == ParseStatus::Command &&
               std::get_if<redis_clone::NullValue>(&result.value.data) != nullptr,
           "null array was not parsed");
    expect(buffer.empty(), "all scalar RESP values should be consumed");
}

void testInlineCommands() {
    redis_clone::RespLimits limits;
    limits.allowInlineCommands = true;
    RespParser parser(limits);
    Buffer buffer;
    buffer.append("PING hello\r\n");
    expectCommand(parser, buffer, command({"PING", "hello"}));
}

void testErrorsAndLimits() {
    RespParser parser;
    Buffer invalid;
    invalid.append("$-2\r\n");
    expect(parser.parse(invalid).status == ParseStatus::ProtocolError,
           "negative bulk length below -1 should fail");

    redis_clone::RespLimits limits;
    limits.maxBulkLength = 3;
    RespParser limitedParser(limits);
    Buffer oversized;
    oversized.append("$4\r\n");
    expect(limitedParser.parse(oversized).status == ParseStatus::ProtocolError,
           "oversized bulk string should fail before payload arrives");
}

void testWriter() {
    Buffer output;
    redis_clone::RespWriter::writeOk(output);
    redis_clone::RespWriter::writeError(output, "ERR failure");
    redis_clone::RespWriter::writeInt(output, 42);
    redis_clone::RespWriter::writeBulk(output, std::string_view("a\0b", 3));
    redis_clone::RespWriter::writeNull(output);
    redis_clone::RespWriter::writeArrayHeader(output, 2);
    const char expectedBytes[] = "+OK\r\n-ERR failure\r\n:42\r\n$3\r\na\0b\r\n$-1\r\n*2\r\n";
    const std::string expected(expectedBytes, sizeof(expectedBytes) - 1);
    expect(output.readable() == expected,
           "writer emitted unexpected RESP bytes");
}

}  // namespace

int main() {
    try {
        testIncrementalParsing();
        testPipelining();
        testRespValueTypes();
        testInlineCommands();
        testErrorsAndLimits();
        testWriter();
    } catch (const std::exception& error) {
        std::cerr << "RESP test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
