#include "store/skip_list.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct Options {
    std::size_t members = 1'000'000;
    std::size_t rankQueries = 1'000'000;
};

std::size_t parsePositiveSize(std::string_view text, const char* option) {
    std::size_t value = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || value == 0) {
        throw std::invalid_argument(std::string(option) +
                                    " must be a positive integer");
    }
    return value;
}

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (i + 1 >= argc) {
            throw std::invalid_argument("missing value for " +
                                        std::string(option));
        }
        const std::string_view value(argv[++i]);
        if (option == "--members") {
            options.members = parsePositiveSize(value, "--members");
        } else if (option == "--rank-queries") {
            options.rankQueries = parsePositiveSize(value, "--rank-queries");
        } else {
            throw std::invalid_argument("unknown option: " +
                                        std::string(option));
        }
    }
    return options;
}

using Clock = std::chrono::steady_clock;
using Sample = std::chrono::nanoseconds;

std::int64_t percentile(const std::vector<Sample>& sortedSamples,
                        std::size_t numerator, std::size_t denominator) {
    const std::size_t index =
        ((sortedSamples.size() - 1) * numerator) / denominator;
    return sortedSamples[index].count();
}

void report(const char* operation, std::vector<Sample>& samples) {
    std::int64_t total = 0;
    for (const Sample sample : samples) {
        total += sample.count();
    }
    std::sort(samples.begin(), samples.end());
    std::cout << operation << ',' << samples.size() << ','
              << static_cast<double>(total) / samples.size() << ','
              << percentile(samples, 50, 100) << ','
              << percentile(samples, 95, 100) << ','
              << percentile(samples, 99, 100) << '\n';
}

void run(const Options& options) {
    redis_clone::ZSet sortedSet;
    std::vector<std::string> members;
    members.reserve(options.members);
    for (std::size_t i = 0; i < options.members; ++i) {
        members.emplace_back("member-" + std::to_string(i));
    }

    std::vector<Sample> addSamples;
    addSamples.reserve(options.members);
    for (std::size_t i = 0; i < options.members; ++i) {
        const auto startedAt = Clock::now();
        const bool added = sortedSet.add(static_cast<double>(i), members[i]);
        const auto endedAt = Clock::now();
        if (!added) {
            throw std::runtime_error("ZADD failed to add a unique member");
        }
        addSamples.push_back(
            std::chrono::duration_cast<Sample>(endedAt - startedAt));
    }

    std::mt19937_64 random(0x5E75E7ULL);
    std::uniform_int_distribution<std::size_t> memberDistribution(
        0, options.members - 1);
    std::vector<Sample> rankSamples;
    rankSamples.reserve(options.rankQueries);
    for (std::size_t i = 0; i < options.rankQueries; ++i) {
        const std::size_t expectedRank = memberDistribution(random);
        const std::string& member = members[expectedRank];
        const auto startedAt = Clock::now();
        const auto score = sortedSet.score(member);
        const auto rank = score.has_value()
                              ? sortedSet.rank(*score, member)
                              : std::nullopt;
        const auto endedAt = Clock::now();
        if (!rank.has_value() || *rank != expectedRank) {
            throw std::runtime_error("ZRANK returned an unexpected rank");
        }
        rankSamples.push_back(
            std::chrono::duration_cast<Sample>(endedAt - startedAt));
    }

    std::cout << "operation,count,avg_ns,p50_ns,p95_ns,p99_ns\n";
    report("ZADD", addSamples);
    report("ZRANK", rankSamples);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        run(parseOptions(argc, argv));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "zset-benchmark: " << error.what() << '\n';
        return 1;
    }
}
