#include "command/command.h"
#include "store/clock.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace redis_clone;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

RespValue bulk(std::string value) {
    return RespValue{RespValue::Storage{BulkString{std::move(value)}}};
}

RespValue::Array arguments(std::initializer_list<std::string> values) {
    RespValue::Array result;
    result.reserve(values.size());
    for (const std::string& value : values) {
        result.push_back(bulk(value));
    }
    return result;
}

std::string execute(CommandContext& context, const CommandTable& table,
                    std::initializer_list<std::string> values) {
    const RespValue::Array args = arguments(values);
    const CommandSpec* command =
        table.find(std::get<BulkString>(args.front().data).value);
    expect(command != nullptr, "command should be registered");
    context.output.clear();
    command->handler(context, args);
    return std::string(context.output.readable());
}

void testHashCommands(CommandContext& context, const CommandTable& table) {
    expect(execute(context, table, {"HSET", "hash", "one", "1", "two", "2"}) ==
               ":2\r\n",
           "HSET should report newly created fields");
    const Object* hash = context.database.find("hash");
    expect(hash != nullptr &&
               context.database.usedBytes() == hash->logicalSize(4),
           "hash mutations should update logical memory accounting");
    expect(execute(context, table, {"TYPE", "hash"}) == "+hash\r\n",
           "TYPE should report hash objects");
    expect(context.database.setExpireAt("hash", nowMs() + 60'000),
           "test hash should accept an expiry");
    expect(execute(context, table, {"HGET", "hash", "two"}) == "$1\r\n2\r\n",
           "HGET should return the field value");
    expect(execute(context, table, {"HEXISTS", "hash", "one"}) == ":1\r\n",
           "HEXISTS should find existing fields");
    expect(execute(context, table, {"HLEN", "hash"}) == ":2\r\n",
           "HLEN should return the field count");
    expect(execute(context, table, {"HKEYS", "hash"}).find("*2\r\n") == 0,
           "HKEYS should return all fields");
    expect(execute(context, table, {"HVALS", "hash"}).find("*2\r\n") == 0,
           "HVALS should return all values");
    expect(execute(context, table, {"HGETALL", "hash"}).find("*4\r\n") == 0,
           "HGETALL should return field-value pairs");
    expect(execute(context, table, {"HINCRBY", "hash", "one", "4"}) ==
               ":5\r\n",
           "HINCRBY should update integer fields");
    expect(context.database.ttl("hash", true) > 0,
           "hash updates should preserve the key TTL");
    expect(execute(context, table, {"HDEL", "hash", "one", "two"}) ==
               ":2\r\n",
           "HDEL should report removed fields");
    expect(context.database.find("hash") == nullptr,
           "empty hashes should be removed from the keyspace");
}

void testListCommands(CommandContext& context, const CommandTable& table) {
    expect(execute(context, table, {"LPUSH", "left-list", "a", "b"}) ==
               ":2\r\n",
           "LPUSH should prepend values");
    expect(execute(context, table, {"LRANGE", "left-list", "0", "-1"}) ==
               "*2\r\n$1\r\nb\r\n$1\r\na\r\n",
           "LPUSH should prepend arguments in Redis order");
    expect(execute(context, table, {"LLEN", "left-list"}) == ":2\r\n",
           "LLEN should return the list length");
    expect(execute(context, table, {"LPOP", "left-list", "2"}).find("*2\r\n") ==
               0,
           "LPOP count should return an array");

    expect(execute(context, table, {"RPUSH", "list", "a", "b", "c"}) ==
               ":3\r\n",
           "RPUSH should append all values");
    expect(execute(context, table, {"LRANGE", "list", "-2", "-1"}) ==
               "*2\r\n$1\r\nb\r\n$1\r\nc\r\n",
           "LRANGE should resolve negative endpoints");
    expect(execute(context, table, {"LINDEX", "list", "-1"}) == "$1\r\nc\r\n",
           "LINDEX should resolve a negative index");
    expect(execute(context, table, {"LSET", "list", "-1", "z"}) == "+OK\r\n",
           "LSET should support negative indexes");
    expect(execute(context, table, {"LPOP", "list", "2"}) ==
               "*2\r\n$1\r\na\r\n$1\r\nb\r\n",
           "LPOP count should return values from the head");
    expect(execute(context, table, {"RPOP", "list"}) == "$1\r\nz\r\n",
           "RPOP should return the tail value");
    expect(context.database.find("list") == nullptr,
           "empty lists should be removed from the keyspace");
    expect(execute(context, table, {"LRANGE", "list", "0", "-1"}) ==
               "*0\r\n",
           "LRANGE on a missing list should return an empty array");
}

void testSetCommands(CommandContext& context, const CommandTable& table) {
    expect(execute(context, table, {"SADD", "set-a", "a", "b", "b"}) ==
               ":2\r\n",
           "SADD should count unique additions");
    expect(execute(context, table, {"SADD", "set-b", "b", "c"}) == ":2\r\n",
           "SADD should create another set");
    expect(execute(context, table, {"SISMEMBER", "set-a", "a"}) == ":1\r\n",
           "SISMEMBER should find members");
    expect(execute(context, table, {"SCARD", "set-a"}) == ":2\r\n",
           "SCARD should return set cardinality");
    expect(execute(context, table, {"SMEMBERS", "set-a"}).find("*2\r\n") == 0,
           "SMEMBERS should return all members");
    const std::string intersection =
        execute(context, table, {"SINTER", "set-a", "set-b"});
    expect(intersection.find("*1\r\n") == 0 &&
               intersection.find("$1\r\nb\r\n") != std::string::npos,
           "SINTER should return shared members");
    expect(execute(context, table, {"SUNION", "set-a", "set-b"}).find("*3\r\n") ==
               0,
           "SUNION should return unique members");
    expect(execute(context, table, {"SDIFF", "set-a", "set-b"}).find("*1\r\n") ==
               0,
           "SDIFF should return members unique to the first set");
    expect(execute(context, table, {"SREM", "set-a", "a", "b"}) == ":2\r\n",
           "SREM should count removed members");
    expect(context.database.find("set-a") == nullptr,
           "empty sets should be removed from the keyspace");
    expect(execute(context, table, {"SPOP", "set-b", "2"}).find("*2\r\n") == 0,
           "SPOP count should return and remove members");
    expect(context.database.find("set-b") == nullptr,
           "SPOP should remove an emptied set key");
    expect(execute(context, table, {"SADD", "random-set", "x", "y"}) ==
               ":2\r\n",
           "SADD should initialize random-member test data");
    const std::string random =
        execute(context, table, {"SRANDMEMBER", "random-set", "-4"});
    expect(random.find("*4\r\n") == 0,
           "negative SRANDMEMBER count should return the requested count");
    expect(execute(context, table, {"SREM", "random-set", "x", "y"}) ==
               ":2\r\n",
           "SREM should remove all remaining set members");
    expect(context.database.find("random-set") == nullptr,
           "removing the final members should delete the set key");
}

void testSortedSetCommands(CommandContext& context,
                           const CommandTable& table) {
    expect(execute(context, table,
                   {"ZADD", "sorted", "1", "one", "2", "two", "2", "alpha"}) ==
               ":3\r\n",
           "ZADD should count newly added members");
    expect(execute(context, table, {"TYPE", "sorted"}) == "+zset\r\n",
           "TYPE should report sorted sets");
    expect(execute(context, table, {"ZRANGE", "sorted", "0", "-1",
                                    "WITHSCORES"}) ==
               "*6\r\n$3\r\none\r\n$1\r\n1\r\n$5\r\nalpha\r\n$1\r\n2\r\n"
               "$3\r\ntwo\r\n$1\r\n2\r\n",
           "ZRANGE should order by score then member");
    expect(execute(context, table, {"ZRANGE", "sorted", "0", "1", "REV"}) ==
               "*2\r\n$3\r\ntwo\r\n$5\r\nalpha\r\n",
           "ZRANGE REV should return reverse rank order");
    expect(execute(context, table, {"ZRANK", "sorted", "two"}) == ":2\r\n",
           "ZRANK should return the zero-based rank");
    expect(execute(context, table, {"ZREVRANK", "sorted", "one"}) == ":2\r\n",
           "ZREVRANK should return the reverse rank");
    expect(execute(context, table, {"ZADD", "sorted", "XX", "CH", "1", "two"}) ==
               ":1\r\n",
           "ZADD XX CH should count score updates");
    expect(execute(context, table, {"ZSCORE", "sorted", "two"}) ==
               "$1\r\n1\r\n",
           "ZSCORE should return the current score");
    expect(execute(context, table, {"ZADD", "sorted", "NX", "3", "two",
                                    "4", "four"}) == ":1\r\n",
           "ZADD NX should skip existing members and add missing members");
    expect(execute(context, table, {"ZRANGEBYSCORE", "sorted", "(1", "+inf",
                                    "WITHSCORES"}) ==
               "*4\r\n$5\r\nalpha\r\n$1\r\n2\r\n$4\r\nfour\r\n$1\r\n4\r\n",
           "ZRANGEBYSCORE should parse exclusive and infinite bounds");
    expect(execute(context, table, {"ZCOUNT", "sorted", "-inf", "(2"}) ==
               ":2\r\n",
           "ZCOUNT should respect exclusive bounds");
    expect(execute(context, table, {"ZINCRBY", "sorted", "2.5", "one"}) ==
               "$3\r\n3.5\r\n",
           "ZINCRBY should update and return the member score");
    expect(execute(context, table, {"ZREMRANGEBYRANK", "sorted", "-2", "-1"}) ==
               ":2\r\n",
           "ZREMRANGEBYRANK should support negative indexes");
    expect(execute(context, table, {"ZREM", "sorted", "two", "alpha"}) ==
               ":2\r\n",
           "ZREM should count removed members");
    expect(context.database.find("sorted") == nullptr,
           "empty sorted sets should be removed from the keyspace");
    expect(execute(context, table,
                   {"ZADD", "infinities", "-inf", "negative", "+inf", "positive"}) ==
               ":2\r\n",
           "ZADD should accept infinite scores");
    expect(execute(context, table, {"ZRANGEBYSCORE", "infinities", "-inf",
                                    "+inf"}) ==
               "*2\r\n$8\r\nnegative\r\n$8\r\npositive\r\n",
           "score bounds should include infinite scores");
    expect(execute(context, table, {"DEL", "infinities"}) == ":1\r\n",
           "sorted sets should be removable with DEL");
}

void testSkipListProperties() {
    using namespace redis_clone;
    SkipList skipList(0.25);
    std::set<std::pair<double, std::string>> reference;
    std::mt19937 random(29017);
    std::uniform_int_distribution<int> memberDistribution(0, 79);
    std::uniform_int_distribution<int> scoreDistribution(-12, 12);
    std::uniform_int_distribution<int> operationDistribution(0, 1);

    for (int operation = 0; operation < 4000; ++operation) {
        const double score = scoreDistribution(random);
        const std::string member =
            "member-" + std::to_string(memberDistribution(random));
        const auto entry = std::make_pair(score, member);
        if (operationDistribution(random) == 0) {
            const bool expectedInserted = reference.insert(entry).second;
            expect(skipList.insert(score, member) == expectedInserted,
                   "skip-list insert should match std::set");
        } else {
            const bool expectedErased = reference.erase(entry) != 0;
            expect(skipList.erase(score, member) == expectedErased,
                   "skip-list erase should match std::set");
        }

        expect(skipList.size() == reference.size(),
               "skip-list size should match std::set after every operation");
        const std::vector<SkipList::Entry> entries =
            skipList.rangeByRank(0, -1);
        expect(entries.size() == reference.size(),
               "skip-list traversal size should match std::set");
        std::size_t rank = 0;
        for (const auto& expected : reference) {
            expect(entries[rank] == expected,
                   "skip-list ordering should match std::set");
            const auto actualRank =
                skipList.rank(expected.first, expected.second);
            expect(actualRank.has_value() && *actualRank == rank,
                   "skip-list rank should match std::set position");
            const auto byRank = skipList.getByRank(rank);
            expect(byRank.has_value() && *byRank == expected,
                   "skip-list getByRank should match std::set position");
            ++rank;
        }
    }
}

void testSortedSetInvariants() {
    using namespace redis_clone;
    SkipList skipList(0.5);
    expect(skipList.promotionProbability() == 0.5,
           "skip-list probability should be configurable at construction");
    expect(skipList.setPromotionProbability(0.125) &&
               skipList.promotionProbability() == 0.125,
           "skip-list probability should be modifiable");
    expect(!skipList.setPromotionProbability(1.0),
           "skip-list should reject invalid promotion probabilities");

    ZSet sortedSet(0.25);
    std::set<std::pair<double, std::string>> reference;
    std::mt19937 random(17);
    std::uniform_int_distribution<int> memberDistribution(0, 39);
    std::uniform_int_distribution<int> scoreDistribution(-5, 5);
    std::uniform_int_distribution<int> operationDistribution(0, 2);
    for (int operation = 0; operation < 1200; ++operation) {
        const std::string member =
            "member-" + std::to_string(memberDistribution(random));
        const int selectedOperation = operationDistribution(random);
        if (selectedOperation == 0) {
            const double score = scoreDistribution(random);
            const auto existing = std::find_if(
                reference.begin(), reference.end(),
                [&member](const auto& entry) { return entry.second == member; });
            const bool expectedAdded = existing == reference.end();
            if (existing != reference.end()) {
                reference.erase(existing);
            }
            reference.emplace(score, member);
            expect(sortedSet.add(score, member) == expectedAdded,
                   "sorted-set add result should match the reference set");
        } else if (selectedOperation == 1) {
            const auto existing = std::find_if(
                reference.begin(), reference.end(),
                [&member](const auto& entry) { return entry.second == member; });
            const bool expectedRemoved = existing != reference.end();
            if (expectedRemoved) {
                reference.erase(existing);
            }
            expect(sortedSet.remove(member) == expectedRemoved,
                   "sorted-set remove should match the reference set");
        } else {
            const double score = scoreDistribution(random);
            const auto existing = std::find_if(
                reference.begin(), reference.end(),
                [&member](const auto& entry) { return entry.second == member; });
            const bool expectedChanged =
                existing != reference.end() && existing->first != score;
            if (expectedChanged) {
                reference.erase(existing);
                reference.emplace(score, member);
            }
            expect(sortedSet.updateScore(member, score) == expectedChanged,
                   "sorted-set updateScore should match the reference set");
        }

        const auto entries = sortedSet.rangeByRank(0, -1);
        expect(sortedSet.indexSizeMatches() &&
                   entries.size() == sortedSet.size() &&
                   entries.size() == reference.size(),
               "sorted-set map and skip-list sizes should match after every operation");
        std::size_t rank = 0;
        for (const auto& expected : reference) {
            expect(entries[rank].first == expected.first &&
                       entries[rank].second == expected.second,
                   "skip-list ordering should match std::set");
            const auto actualRank = sortedSet.rank(expected.first, expected.second);
            expect(actualRank.has_value() && *actualRank == rank,
                   "span-based rank should match the reference position");
            const auto byRank = sortedSet.getByRank(rank);
            expect(byRank.has_value() && byRank->first == expected.first &&
                       byRank->second == expected.second,
                   "getByRank should match the reference position");
            ++rank;
        }

        const int length = static_cast<int>(reference.size());
        std::uniform_int_distribution<int> endpointDistribution(-length - 2,
                                                                length + 2);
        const std::int64_t start = endpointDistribution(random);
        const std::int64_t stop = endpointDistribution(random);
        std::int64_t normalizedStart =
            start < 0 ? std::max<std::int64_t>(0, length + start) : start;
        std::int64_t normalizedStop = stop < 0 ? length + stop : stop;
        std::vector<ZSet::Entry> expectedRange;
        if (normalizedStart < length && normalizedStop >= normalizedStart) {
            normalizedStop = std::min<std::int64_t>(normalizedStop, length - 1);
            auto entry = reference.begin();
            std::advance(entry, normalizedStart);
            for (std::int64_t index = normalizedStart;
                 index <= normalizedStop; ++index, ++entry) {
                expectedRange.push_back(*entry);
            }
        }
        expect(sortedSet.rangeByRank(start, stop) == expectedRange,
               "rangeByRank should match Redis-style inclusive indexes");
    }
}

void testWrongType(CommandContext& context, const CommandTable& table) {
    context.database.setString("string", "value");
    const std::string reply =
        execute(context, table, {"LPUSH", "string", "item"});
    expect(reply.find("-WRONGTYPE") == 0,
           "container commands on strings should return WRONGTYPE");
    const std::string sortedSetReply =
        execute(context, table, {"ZADD", "string", "1", "member"});
    expect(sortedSetReply.find("-WRONGTYPE") == 0,
           "sorted-set commands on strings should return WRONGTYPE");
}

}  // namespace

int main() {
    try {
        redis_clone::Buffer output;
        bool closeAfterReply = false;
        redis_clone::Database database;
        redis_clone::UserRegistry users;
        redis_clone::User* currentUser = nullptr;
        redis_clone::CommandContext context(output, closeAfterReply, database,
                                            users, currentUser, 1);
        const redis_clone::CommandTable table;
        testHashCommands(context, table);
        testListCommands(context, table);
        testSetCommands(context, table);
        testSortedSetCommands(context, table);
        testSkipListProperties();
        testSortedSetInvariants();
        testWrongType(context, table);
    } catch (const std::exception& error) {
        std::cerr << "Container command test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
