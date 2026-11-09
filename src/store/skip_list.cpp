#include "store/skip_list.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace redis_clone {

namespace {

// One engine per thread, shared by every SkipList used on that thread.
// Seeded once on first use; no per-list engine state (~2.5 KB each) and
// no random_device call per construction.
std::mt19937_64& threadRandomEngine() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

}  // namespace

SkipList::Node::Node(double scoreValue, std::string memberValue,
                     std::size_t height)
    : score(scoreValue),
      member(std::move(memberValue)),
      levels(height) {}

SkipList::SkipList(double promotionProbability)
    : head_(std::make_unique<Node>(0.0, std::string{}, kMaxLevel)) {
    if (!setPromotionProbability(promotionProbability)) {
        throw std::invalid_argument("skip-list probability must be between 0 and 1");
    }
}

SkipList::SkipList(const SkipList& other)
    : SkipList(other.promotionProbability_) {
    for (Node* node = other.head_->levels[0].forward; node != nullptr;
         node = node->levels[0].forward) {
        insert(node->score, node->member);
    }
}

SkipList& SkipList::operator=(const SkipList& other) {
    if (this != &other) {
        SkipList copy(other);
        std::swap(head_, copy.head_);
        std::swap(currentLevel_, copy.currentLevel_);
        std::swap(size_, copy.size_);
        std::swap(promotionProbability_, copy.promotionProbability_);
    }
    return *this;
}

SkipList& SkipList::operator=(SkipList&& other) noexcept {
    if (this != &other) {
        clear();
        head_ = std::move(other.head_);
        currentLevel_ = other.currentLevel_;
        size_ = other.size_;
        promotionProbability_ = other.promotionProbability_;
        other.currentLevel_ = 1;
        other.size_ = 0;
    }
    return *this;
}

SkipList::~SkipList() {
    clear();
}

bool SkipList::setPromotionProbability(double probability) noexcept {
    if (!std::isfinite(probability) || probability <= 0.0 ||
        probability >= 1.0) {
        return false;
    }
    promotionProbability_ = probability;
    return true;
}

double SkipList::promotionProbability() const noexcept {
    return promotionProbability_;
}

bool SkipList::less(double leftScore, const std::string& leftMember,
                    double rightScore, const std::string& rightMember) noexcept {
    return leftScore < rightScore ||
           (leftScore == rightScore && leftMember < rightMember);
}

SkipList::Node* SkipList::findNode(double score,
                                   const std::string& member) const noexcept {
    if (std::isnan(score)) {
        return nullptr;
    }
    Node* node = head_.get();
    for (std::size_t level = currentLevel_; level-- > 0;) {
        while (node->levels[level].forward != nullptr &&
               less(node->levels[level].forward->score,
                    node->levels[level].forward->member, score, member)) {
            node = node->levels[level].forward;
        }
    }
    node = node->levels[0].forward;
    return node != nullptr && node->score == score && node->member == member
               ? node
               : nullptr;
}

std::size_t SkipList::randomLevel() {
    std::size_t level = 1;
    std::mt19937_64& engine = threadRandomEngine();
    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    while (level < kMaxLevel && distribution(engine) < promotionProbability_) {
        ++level;
    }
    return level;
}

bool SkipList::insert(double score, std::string member) {
    if (std::isnan(score)) {
        return false;
    }

    std::vector<Node*> update(kMaxLevel);
    std::vector<std::size_t> rankAtLevel(kMaxLevel, 0);
    Node* node = head_.get();
    for (std::size_t level = currentLevel_; level-- > 0;) {
        rankAtLevel[level] =
            level == currentLevel_ - 1 ? 0 : rankAtLevel[level + 1];
        while (node->levels[level].forward != nullptr &&
               less(node->levels[level].forward->score,
                    node->levels[level].forward->member, score, member)) {
            rankAtLevel[level] += node->levels[level].span;
            node = node->levels[level].forward;
        }
        update[level] = node;
    }

    Node* next = update[0]->levels[0].forward;
    if (next != nullptr && next->score == score && next->member == member) {
        return false;
    }

    const std::size_t newLevel = randomLevel();
    if (newLevel > currentLevel_) {
        for (std::size_t level = currentLevel_; level < newLevel; ++level) {
            rankAtLevel[level] = 0;
            update[level] = head_.get();
            update[level]->levels[level].span = size_;
        }
        currentLevel_ = newLevel;
    }

    auto inserted = std::make_unique<Node>(score, std::move(member), newLevel);
    for (std::size_t level = 0; level < newLevel; ++level) {
        inserted->levels[level].forward = update[level]->levels[level].forward;
        inserted->levels[level].span =
            update[level]->levels[level].span -
            (rankAtLevel[0] - rankAtLevel[level]);
        update[level]->levels[level].forward = inserted.get();
        update[level]->levels[level].span =
            (rankAtLevel[0] - rankAtLevel[level]) + 1;
    }
    for (std::size_t level = newLevel; level < currentLevel_; ++level) {
        ++update[level]->levels[level].span;
    }

    inserted->backward = update[0] == head_.get() ? nullptr : update[0];
    if (inserted->levels[0].forward != nullptr) {
        inserted->levels[0].forward->backward = inserted.get();
    }
    inserted.release();
    ++size_;
    return true;
}

bool SkipList::erase(double score, const std::string& member) {
    std::vector<Node*> update(currentLevel_);
    Node* node = head_.get();
    for (std::size_t level = currentLevel_; level-- > 0;) {
        while (node->levels[level].forward != nullptr &&
               less(node->levels[level].forward->score,
                    node->levels[level].forward->member, score, member)) {
            node = node->levels[level].forward;
        }
        update[level] = node;
    }

    Node* target = update[0]->levels[0].forward;
    if (target == nullptr || target->score != score || target->member != member) {
        return false;
    }
    for (std::size_t level = 0; level < currentLevel_; ++level) {
        if (update[level]->levels[level].forward == target) {
            update[level]->levels[level].span +=
                target->levels[level].span - 1;
            update[level]->levels[level].forward =
                target->levels[level].forward;
        } else {
            --update[level]->levels[level].span;
        }
    }
    if (target->levels[0].forward != nullptr) {
        target->levels[0].forward->backward = target->backward;
    }
    delete target;
    while (currentLevel_ > 1 &&
           head_->levels[currentLevel_ - 1].forward == nullptr) {
        --currentLevel_;
    }
    --size_;
    return true;
}

std::optional<std::size_t> SkipList::rank(
    double score, const std::string& member) const {
    if (std::isnan(score)) {
        return std::nullopt;
    }
    std::size_t traversed = 0;
    Node* node = head_.get();
    for (std::size_t level = currentLevel_; level-- > 0;) {
        while (node->levels[level].forward != nullptr &&
               !less(score, member, node->levels[level].forward->score,
                     node->levels[level].forward->member)) {
            traversed += node->levels[level].span;
            node = node->levels[level].forward;
        }
    }
    if (node != head_.get() && node->score == score &&
        node->member == member) {
        return traversed - 1;
    }
    return std::nullopt;
}

std::optional<SkipList::Entry> SkipList::getByRank(std::size_t rankValue) const {
    if (rankValue >= size_) {
        return std::nullopt;
    }
    std::size_t traversed = 0;
    Node* node = head_.get();
    for (std::size_t level = currentLevel_; level-- > 0;) {
        while (node->levels[level].forward != nullptr &&
               traversed + node->levels[level].span <= rankValue) {
            traversed += node->levels[level].span;
            node = node->levels[level].forward;
        }
    }
    node = node->levels[0].forward;
    if (node == nullptr) {
        return std::nullopt;
    }
    return Entry{node->score, node->member};
}

std::vector<SkipList::Entry> SkipList::rangeByRank(std::int64_t start,
                                                   std::int64_t stop) const {
    std::vector<Entry> result;
    if (size_ == 0) {
        return result;
    }
    const std::int64_t length =
        size_ > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(size_);
    if (start < 0) {
        start = std::max<std::int64_t>(0, length + start);
    }
    if (stop < 0) {
        stop = length + stop;
    }
    if (start >= length || stop < start) {
        return result;
    }
    stop = std::min(stop, length - 1);
    result.reserve(static_cast<std::size_t>(stop - start + 1));
    const std::size_t startRank = static_cast<std::size_t>(start);
    std::size_t traversed = 0;
    Node* node = head_.get();
    for (std::size_t level = currentLevel_; level-- > 0;) {
        while (node->levels[level].forward != nullptr &&
               traversed + node->levels[level].span <= startRank) {
            traversed += node->levels[level].span;
            node = node->levels[level].forward;
        }
    }
    node = node->levels[0].forward;
    for (std::int64_t index = start; node != nullptr && index <= stop;
         ++index, node = node->levels[0].forward) {
        result.emplace_back(node->score, node->member);
    }
    return result;
}

std::vector<SkipList::Entry> SkipList::rangeByScore(
    double minimum, double maximum, bool minimumExclusive,
    bool maximumExclusive) const {
    std::vector<Entry> result;
    if (std::isnan(minimum) || std::isnan(maximum) || minimum > maximum ||
        (minimum == maximum && (minimumExclusive || maximumExclusive))) {
        return result;
    }

    Node* node = head_.get();
    for (std::size_t level = currentLevel_; level-- > 0;) {
        while (node->levels[level].forward != nullptr &&
               (node->levels[level].forward->score < minimum ||
                (minimumExclusive &&
                 node->levels[level].forward->score == minimum))) {
            node = node->levels[level].forward;
        }
    }
    node = node->levels[0].forward;
    while (node != nullptr &&
           (node->score < maximum ||
            (!maximumExclusive && node->score == maximum))) {
        result.emplace_back(node->score, node->member);
        node = node->levels[0].forward;
    }
    return result;
}

std::size_t SkipList::size() const noexcept {
    return size_;
}

void SkipList::clear() noexcept {
    if (head_ == nullptr) {
        return;
    }
    Node* node = head_->levels[0].forward;
    while (node != nullptr) {
        Node* next = node->levels[0].forward;
        delete node;
        node = next;
    }
    for (Level& level : head_->levels) {
        level = Level{};
    }
    currentLevel_ = 1;
    size_ = 0;
}

ZSet::ZSet(const ZSet& other) : skipList_(other.skipList_) {
    members_.reserve(other.members_.size());
    for (SkipList::Node* node = skipList_.head_->levels[0].forward;
         node != nullptr; node = node->levels[0].forward) {
        members_.emplace(std::string_view(node->member), node);
    }
}

ZSet& ZSet::operator=(const ZSet& other) {
    if (this != &other) {
        ZSet copy(other);
        *this = std::move(copy);
    }
    return *this;
}

bool ZSet::add(double scoreValue, std::string member) {
    if (std::isnan(scoreValue)) {
        return false;
    }
    const auto existing = members_.find(member);
    if (existing != members_.end()) {
        updateScore(member, scoreValue);
        return false;
    }
    if (!skipList_.insert(scoreValue, member)) {
        return false;
    }
    SkipList::Node* node = skipList_.findNode(scoreValue, member);
    try {
        members_.emplace(std::string_view(node->member), node);
    } catch (...) {
        skipList_.erase(scoreValue, member);
        throw;
    }
    return true;
}

bool ZSet::updateScore(const std::string& member, double scoreValue) {
    if (std::isnan(scoreValue)) {
        return false;
    }
    auto existing = members_.find(member);
    if (existing == members_.end() ||
        existing->second->score == scoreValue) {
        return false;
    }
    if (!skipList_.insert(scoreValue, member)) {
        return false;
    }
    SkipList::Node* oldNode = existing->second;
    SkipList::Node* newNode = skipList_.findNode(scoreValue, member);
    auto indexEntry = members_.extract(existing);
    indexEntry.key() = std::string_view(newNode->member);
    indexEntry.mapped() = newNode;
    if (!skipList_.erase(oldNode->score, oldNode->member)) {
        indexEntry.key() = std::string_view(oldNode->member);
        indexEntry.mapped() = oldNode;
        members_.insert(std::move(indexEntry));
        skipList_.erase(scoreValue, member);
        return false;
    }
    members_.insert(std::move(indexEntry));
    return true;
}

bool ZSet::remove(const std::string& member) {
    const auto existing = members_.find(member);
    if (existing == members_.end()) {
        return false;
    }
    SkipList::Node* node = existing->second;
    if (!skipList_.erase(node->score, node->member)) {
        return false;
    }
    members_.erase(existing);
    return true;
}

std::optional<double> ZSet::score(const std::string& member) const {
    const auto found = members_.find(member);
    if (found == members_.end()) {
        return std::nullopt;
    }
    return found->second->score;
}

bool ZSet::contains(const std::string& member) const {
    return members_.count(member) != 0;
}

std::optional<std::size_t> ZSet::rank(double scoreValue,
                                      const std::string& member) const {
    return skipList_.rank(scoreValue, member);
}

std::optional<ZSet::Entry> ZSet::getByRank(std::size_t rankValue) const {
    return skipList_.getByRank(rankValue);
}

std::vector<ZSet::Entry> ZSet::rangeByRank(std::int64_t start,
                                           std::int64_t stop) const {
    return skipList_.rangeByRank(start, stop);
}

std::vector<ZSet::Entry> ZSet::rangeByScore(double minimum, double maximum,
                                            bool minimumExclusive,
                                            bool maximumExclusive) const {
    return skipList_.rangeByScore(minimum, maximum, minimumExclusive,
                                  maximumExclusive);
}

std::size_t ZSet::size() const noexcept {
    return members_.size();
}

std::size_t ZSet::logicalSize() const noexcept {
    std::size_t size = 0;
    if (!members_.empty()) {
        size += (members_.bucket_count() + 1) * sizeof(void*);
    }
    size += members_.size() * 40;
    for (SkipList::Node* node = skipList_.head_->levels[0].forward;
         node != nullptr; node = node->levels[0].forward) {
        size += sizeof(SkipList::Node) + node->member.capacity() +
                node->levels.capacity() * sizeof(SkipList::Level);
    }
    return size;
}

std::size_t ZSet::memoryUsage() const noexcept {
    return logicalSize();
}

bool ZSet::indexSizeMatches() const noexcept {
    if (members_.size() != skipList_.size()) {
        return false;
    }
    for (SkipList::Node* node = skipList_.head_->levels[0].forward;
         node != nullptr; node = node->levels[0].forward) {
        const auto found = members_.find(std::string_view(node->member));
        if (found == members_.end() || found->second != node) {
            return false;
        }
    }
    return true;
}

}  // namespace redis_clone