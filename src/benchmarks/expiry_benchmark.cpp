#include "store/clock.h"
#include "user/user_registry.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

struct Options {
    std::size_t keyCount = 1'000'000;
    std::int64_t ttlMilliseconds = 1'000;
    std::int64_t budgetMicroseconds = 1'000;
    std::int64_t maxRuntimeSeconds = 300;
    std::string allocator = "unspecified";
};

template <typename Integer>
bool parseInteger(std::string_view text, Integer& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (i + 1 >= argc) {
            throw std::invalid_argument("missing value for " +
                                        std::string(name));
        }
        const std::string_view value(argv[++i]);
        if (name == "--keys") {
            if (!parseInteger(value, options.keyCount) || options.keyCount == 0) {
                throw std::invalid_argument("--keys must be a positive integer");
            }
        } else if (name == "--ttl-ms") {
            if (!parseInteger(value, options.ttlMilliseconds) ||
                options.ttlMilliseconds <= 0) {
                throw std::invalid_argument(
                    "--ttl-ms must be a positive integer");
            }
        } else if (name == "--budget-us") {
            if (!parseInteger(value, options.budgetMicroseconds) ||
                options.budgetMicroseconds <= 0) {
                throw std::invalid_argument(
                    "--budget-us must be a positive integer");
            }
        } else if (name == "--max-runtime-seconds") {
            if (!parseInteger(value, options.maxRuntimeSeconds) ||
                options.maxRuntimeSeconds <= 0) {
                throw std::invalid_argument(
                    "--max-runtime-seconds must be a positive integer");
            }
        } else if (name == "--allocator") {
            if (value.empty() || value.find_first_of(",\r\n") !=
                                     std::string_view::npos) {
                throw std::invalid_argument(
                    "--allocator must be non-empty and contain no commas");
            }
            options.allocator = value;
        } else {
            throw std::invalid_argument("unknown option: " + std::string(name));
        }
    }
    return options;
}

std::uint64_t residentBytes() {
    std::ifstream status("/proc/self/status");
    if (!status) {
        throw std::runtime_error("could not open /proc/self/status for RSS");
    }

    std::string line;
    while (std::getline(status, line)) {
        constexpr std::string_view kResidentPrefix = "VmRSS:";
        if (line.compare(0, kResidentPrefix.size(), kResidentPrefix) != 0) {
            continue;
        }

        std::uint64_t kibibytes = 0;
        std::string unit;
        std::istringstream fields(line.substr(kResidentPrefix.size()));
        if (!(fields >> kibibytes >> unit) || unit != "kB" ||
            kibibytes > std::numeric_limits<std::uint64_t>::max() / 1024) {
            throw std::runtime_error("could not parse VmRSS in /proc/self/status");
        }
        return kibibytes * 1024;
    }
    throw std::runtime_error("VmRSS is missing from /proc/self/status");
}

struct Result {
    std::chrono::steady_clock::duration populationDuration{};
    std::chrono::steady_clock::duration expiryDuration{};
    std::chrono::steady_clock::duration maxCycleDuration{};
    std::uint64_t rssBeforeBytes = 0;
    std::uint64_t rssAfterBytes = 0;
    std::size_t expiredKeys = 0;
};

Result runBenchmark(const Options& options) {
    redis_clone::UserRegistry users;
    redis_clone::User& user = users.addUser("benchmark", "benchmark");
    redis_clone::Database& database = user.database;

    Result result;
    const auto populationStartedAt = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < options.keyCount; ++i) {
        database.setString("expiry-benchmark-" + std::to_string(i), "value");
    }

    std::int64_t latestDeadlineMs = 0;
    for (const std::string& key : database.keys()) {
        const std::int64_t currentTime = redis_clone::nowMs();
        if (currentTime >
            std::numeric_limits<std::int64_t>::max() -
                options.ttlMilliseconds) {
            throw std::invalid_argument("--ttl-ms is too large");
        }
        const std::int64_t keyDeadlineMs =
            currentTime + options.ttlMilliseconds;
        if (!database.setExpireAt(key, keyDeadlineMs)) {
            throw std::runtime_error("key disappeared while setting benchmark TTL");
        }
        latestDeadlineMs = std::max(latestDeadlineMs, keyDeadlineMs);
    }
    result.populationDuration =
        std::chrono::steady_clock::now() - populationStartedAt;
    result.rssBeforeBytes = residentBytes();

    const auto expiryDeadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max<std::int64_t>(
            0, latestDeadlineMs - redis_clone::nowMs()));
    std::this_thread::sleep_until(expiryDeadline);

    const auto expiryStartedAt = std::chrono::steady_clock::now();
    const auto maxRuntime =
        std::chrono::seconds(options.maxRuntimeSeconds);
    const auto cycleBudget =
        std::chrono::microseconds(options.budgetMicroseconds);
    while (result.expiredKeys < options.keyCount) {
        if (std::chrono::steady_clock::now() - expiryStartedAt > maxRuntime) {
            throw std::runtime_error(
                "expiry did not complete before --max-runtime-seconds");
        }

        const auto cycleStartedAt = std::chrono::steady_clock::now();
        result.expiredKeys += users.activeExpireCycle(cycleBudget);
        result.maxCycleDuration =
            std::max(result.maxCycleDuration,
                     std::chrono::steady_clock::now() - cycleStartedAt);
    }
    result.expiryDuration =
        std::chrono::steady_clock::now() - expiryStartedAt;
    result.rssAfterBytes = residentBytes();
    return result;
}

std::int64_t microseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::microseconds>(duration)
        .count();
}

std::int64_t milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
        .count();
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parseOptions(argc, argv);
        const Result result = runBenchmark(options);
        const bool overBudget =
            result.maxCycleDuration >
            std::chrono::microseconds(options.budgetMicroseconds);

        std::cout << options.allocator << ',' << options.keyCount << ','
                  << options.budgetMicroseconds << ','
                  << microseconds(result.maxCycleDuration) << ','
                  << milliseconds(result.populationDuration) << ','
                  << milliseconds(result.expiryDuration) << ','
                  << result.rssBeforeBytes << ',' << result.rssAfterBytes << ','
                  << result.expiredKeys << ','
                  << (overBudget ? "over_budget" : "within_budget") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "expiry-benchmark: " << error.what() << '\n';
        return 1;
    }
}
