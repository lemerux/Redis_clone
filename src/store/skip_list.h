#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace redis_clone {

class SkipList {
public:
    using Entry = std::pair<double, std::string>;
    static constexpr std::size_t kMaxLevel = 32;

    explicit SkipList(double promotionProbability = 0.25);
    SkipList(const SkipList& other);
    SkipList& operator=(const SkipList& other);
    SkipList(SkipList&&) noexcept = default;
    SkipList& operator=(SkipList&& other) noexcept;
    ~SkipList();

    bool setPromotionProbability(double probability) noexcept;
    double promotionProbability() const noexcept;

    bool insert(double score, std::string member);
    bool erase(double score, const std::string& member);
    std::optional<std::size_t> rank(double score,
                                    const std::string& member) const;
    std::optional<Entry> getByRank(std::size_t rank) const;
    std::vector<Entry> rangeByRank(std::int64_t start,
                                   std::int64_t stop) const;
    std::vector<Entry> rangeByScore(double minimum, double maximum,
                                    bool minimumExclusive = false,
                                    bool maximumExclusive = false) const;
    std::size_t size() const noexcept;

private:
    friend class ZSet;

    struct Node;

    struct Level {
        Node* forward = nullptr;
        std::size_t span = 0;
    };

    struct Node {
        Node(double scoreValue, std::string memberValue, std::size_t height);

        double score;
        std::string member;
        std::vector<Level> levels;
        Node* backward = nullptr;
    };

    static bool less(double leftScore, const std::string& leftMember,
                     double rightScore, const std::string& rightMember) noexcept;
    Node* findNode(double score, const std::string& member) const noexcept;
    std::size_t randomLevel();
    void clear() noexcept;

    std::unique_ptr<Node> head_;
    std::size_t currentLevel_ = 1;
    std::size_t size_ = 0;
    double promotionProbability_ = 0.25;
};

class ZSet {
public:
    using Entry = SkipList::Entry;

    explicit ZSet(double promotionProbability = 0.25)
        : skipList_(promotionProbability) {}
    ZSet(const ZSet& other);
    ZSet& operator=(const ZSet& other);
    ZSet(ZSet&&) noexcept = default;
    ZSet& operator=(ZSet&&) noexcept = default;

    bool add(double score, std::string member);
    bool updateScore(const std::string& member, double score);
    bool remove(const std::string& member);
    std::optional<double> score(const std::string& member) const;
    bool contains(const std::string& member) const;

    std::optional<std::size_t> rank(double score,
                                    const std::string& member) const;
    std::optional<Entry> getByRank(std::size_t rank) const;
    std::vector<Entry> rangeByRank(std::int64_t start,
                                   std::int64_t stop) const;
    std::vector<Entry> rangeByScore(double minimum, double maximum,
                                    bool minimumExclusive = false,
                                    bool maximumExclusive = false) const;
    std::size_t size() const noexcept;
    std::size_t logicalSize() const noexcept;
    std::size_t memoryUsage() const noexcept;
    bool indexSizeMatches() const noexcept;

private:
    SkipList skipList_;
    std::unordered_map<std::string_view, SkipList::Node*> members_;
};

}  // namespace redis_clone