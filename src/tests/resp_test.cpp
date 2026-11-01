#include "resp/resp.h"

#include <algorithm>
#include <iostream>
#include <random>
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
    const std::string encoded =
        "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n"
        "*1\r\n$4\r\nPING\r\n"
        "*2\r\n$3\r\nGET\r\n$1\r\nx\r\n";
    RespParser parser;

    const auto parseWithChunks = [&](const std::vector<std::size_t>& chunks) {
        Buffer buffer;
        Buffer parsedOutput;
        std::size_t offset = 0;
        std::size_t chunkIndex = 0;
        while (offset < encoded.size()) {
            const std::size_t chunkSize =
                std::min(chunks[chunkIndex++ % chunks.size()], encoded.size() - offset);
            buffer.append(std::string_view(encoded).substr(offset, chunkSize));
            offset += chunkSize;

            while (!buffer.empty()) {
                const auto result = parser.parse(buffer);
                if (result.status == ParseStatus::NeedMoreData) {
                    break;
                }
                expect(result.status == ParseStatus::Command,
                       "valid chunked input should not produce a protocol error");
                if (std::get_if<RespValue::Array>(&result.value.data) == nullptr) {
                    throw std::runtime_error("expected a command array");
                }
                redis_clone::RespWriter::writeValue(parsedOutput, result.value);
            }
        }
        while (!buffer.empty()) {
            const auto result = parser.parse(buffer);
            if (result.status == ParseStatus::NeedMoreData) {
                break;
            }
            expect(result.status == ParseStatus::Command,
                   "valid input should not produce a protocol error");
            redis_clone::RespWriter::writeValue(parsedOutput, result.value);
        }
        expect(buffer.empty(), "complete input should leave no buffered bytes");
        expect(parsedOutput.readable() == encoded,
               "chunked input produced different parsed commands");
    };

    parseWithChunks({1});
    parseWithChunks({encoded.size()});
    parseWithChunks({2, 7, 1, 13, 4, 3, 11});

    std::mt19937 random(0x5EED);
    std::vector<std::size_t> randomChunks;
    for (int i = 0; i < 32; ++i) {
        randomChunks.push_back(1 + random() % 17);
    }
    parseWithChunks(randomChunks);
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

    Buffer oversizedDefaultBulk;
    oversizedDefaultBulk.append("$536870913\r\n");
    expect(parser.parse(oversizedDefaultBulk).status == ParseStatus::ProtocolError,
           "bulk string above the default 512 MiB limit should fail");

    Buffer oversizedArray;
    oversizedArray.append("*1000001\r\n");
    expect(parser.parse(oversizedArray).status == ParseStatus::ProtocolError,
           "array above the default one-million element limit should fail");
    Buffer incompleteMaximumArray;
    incompleteMaximumArray.append("*1000000\r\n");
    expect(parser.parse(incompleteMaximumArray).status == ParseStatus::NeedMoreData,
           "maximum-sized array header should wait for elements without preallocating them");

    redis_clone::RespLimits limits;
    limits.maxBulkLength = 3 * 1024 * 1024;
    limits.maxCommandBytes = limits.maxBulkLength + 64;
    RespParser limitedParser(limits);
    Buffer acceptedBulkLength;
    acceptedBulkLength.append("$3145728\r\n");
    expect(limitedParser.parse(acceptedBulkLength).status == ParseStatus::NeedMoreData,
           "bulk length at the configured 3 MiB test limit should be accepted");
    Buffer oversizedBulk;
    oversizedBulk.append("$3145729\r\n");
    expect(limitedParser.parse(oversizedBulk).status == ParseStatus::ProtocolError,
           "bulk string above the configured 3 MiB test limit should fail");

    redis_clone::RespLimits smallCommand;
    smallCommand.maxCommandBytes = 8;
    RespParser smallCommandParser(smallCommand);
    Buffer partialOversizedLine;
    partialOversizedLine.append("*1\r\n+12345");
    expect(smallCommandParser.parse(partialOversizedLine).status ==
               ParseStatus::ProtocolError,
           "incomplete nested line must respect the total command byte limit");

    Buffer impossibleBulk;
    impossibleBulk.append("$3\r\n");
    expect(smallCommandParser.parse(impossibleBulk).status == ParseStatus::ProtocolError,
           "bulk header whose payload and terminator exceed the limit should fail early");
}

void testBufferSelfAppend() {
    Buffer buffer;
    buffer.append(std::string(60, 'a'));
    buffer.consume(50);
    const std::string_view readable = buffer.readable();
    buffer.append(readable);
    expect(buffer.readable() == std::string(20, 'a'),
           "appending the buffer's readable view should preserve its bytes");
}

void testRandomBytes() {
    redis_clone::RespLimits limits;
    limits.maxBulkLength = 1024;
    limits.maxArrayLength = 128;
    limits.maxCommandBytes = 1024;
    limits.maxNestingDepth = 16;
    RespParser parser(limits);
    std::mt19937 random(0xF022);

    for (int sample = 0; sample < 10000; ++sample) {
        const std::size_t size = random() % 256;
        std::string bytes(size, '\0');
        for (char& byte : bytes) {
            byte = static_cast<char>(random() & 0xFF);
        }

        Buffer buffer;
        buffer.append(bytes);
        for (int parseCount = 0; parseCount < 8 && !buffer.empty(); ++parseCount) {
            const auto result = parser.parse(buffer);
            if (result.status != ParseStatus::Command) {
                break;
            }
        }
    }
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
        testBufferSelfAppend();
        testRandomBytes();
        testWriter();
    } catch (const std::exception& error) {
        std::cerr << "RESP test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
