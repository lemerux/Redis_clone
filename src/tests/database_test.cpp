#include "store/database.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testLogicalAccounting() {
    redis_clone::Database database;
    expect(database.usedBytes() == 0, "new database must have no used bytes");

    database.setString("alpha", "one");
    const std::size_t firstSize =
        sizeof(redis_clone::Object) + std::string("alpha").size() +
        std::string("one").capacity();
    expect(database.usedBytes() == firstSize,
           "insert must count key, value, and object overhead");

    database.setString("alpha", "longer");
    expect(database.usedBytes() ==
               sizeof(redis_clone::Object) + std::string("alpha").size() +
                   std::string("longer").capacity(),
           "replacement must account for resized values");

    database.setString("beta", "");
    const std::size_t beforeErase = database.usedBytes();
    expect(database.erase("alpha"), "erase should remove existing keys");
    expect(database.usedBytes() ==
               beforeErase - sizeof(redis_clone::Object) -
                   std::string("alpha").size() - std::string("longer").capacity(),
           "erase must subtract logical key and value sizes");

    database.flush();
    expect(database.size() == 0 && database.usedBytes() == 0,
           "flush must clear entries and reset logical usage");
}

void testSamplingAcrossGrowth() {
    redis_clone::Database database;
    for (int i = 0; i < 1007; ++i) {
        database.setString("key-" + std::to_string(i), "value");
    }

    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto sample = database.sampleKey(i);
        expect(sample.has_value() && database.find(*sample) != nullptr,
               "random sampling must return a live key");
    }

}

void testRemovalSink() {
    redis_clone::Database database;
    std::string removed;
    database.setRemovalSink([&removed](std::string_view key) {
        removed.assign(key);
    });
    database.setString("expired", "value");
    expect(database.removeExpired("expired"), "expired key should be removed");
    expect(removed == "expired", "expiry removal must notify the removal sink");
    database.setString("evicted", "value");
    expect(database.removeForEviction("evicted"),
           "evicted key should be removed");
    expect(removed == "evicted",
           "eviction removal must notify the removal sink");
    expect(!database.erase("missing"), "erasing a missing key should return false");
}

}  // namespace

int main() {
    try {
        testLogicalAccounting();
        testSamplingAcrossGrowth();
        testRemovalSink();
    } catch (const std::exception& error) {
        std::cerr << "Database test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
