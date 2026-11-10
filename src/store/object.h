#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "store/skip_list.h"

namespace redis_clone {

enum class ObjType {
    String,
    Hash,
    List,
    Set,
    ZSet,
};

// ListValues: a byte-arena ring buffer for list elements.
//
// Every element is one record stored in a single contiguous, wrapping buffer:
//
//     [lead varint: len][len payload bytes][trail varint: len]
//
// * lead  is plain LEB128 (low group first, 0x80 = "more bytes follow"). It is
//   read forwards from the start of a record.
// * trail is written high group first; every byte except the first one carries
//   0x80 meaning "more bytes before me". It is read backwards from the end of a
//   record, which is what makes pop_back() / reverse walks possible without
//   per-element pointers (same idea as the Redis listpack backlen).
//
// Cost per element is payload + 2*varintLen(len): 18 bytes for a 16-byte
// member, no per-element allocation, no 32-byte std::string slot.
//
// Capacity grows by 1.5x (rounded to 16 bytes), shrinks when less than a
// quarter of the buffer is used, and the buffer is freed when the list empties.
// Random access (operator[], at, set) is a linear walk from the nearer end.
class ListValues {
public:
    ListValues() = default;

    ListValues(const ListValues& other)
        : buf_(other.cap_ ? new char[other.cap_] : nullptr),
          cap_(other.cap_),
          head_(0),
          bytes_(other.bytes_),
          count_(other.count_) {
        if (bytes_ != 0) {
            other.copyOut(other.head_, bytes_, buf_.get());
        }
    }

    ListValues(ListValues&& other) noexcept { swap(other); }

    ListValues& operator=(const ListValues& other) {
        if (this != &other) {
            ListValues tmp(other);
            swap(tmp);
        }
        return *this;
    }

    ListValues& operator=(ListValues&& other) noexcept {
        if (this != &other) {
            ListValues tmp(std::move(other));
            swap(tmp);
        }
        return *this;
    }

    void swap(ListValues& o) noexcept {
        buf_.swap(o.buf_);
        std::swap(cap_, o.cap_);
        std::swap(head_, o.head_);
        std::swap(bytes_, o.bytes_);
        std::swap(count_, o.count_);
    }

    void push_back(std::string_view value) { push(value, /*front=*/false); }
    void push_front(std::string_view value) { push(value, /*front=*/true); }

    void pop_front() {
        if (count_ == 0) {
            return;
        }
        std::uint32_t len;
        const std::uint32_t l = readLead(head_, len);
        const std::uint64_t rec = 2ull * l + len;
        head_ = wrap(std::uint64_t{head_} + rec);
        bytes_ -= static_cast<std::uint32_t>(rec);
        --count_;
        maybeShrink();
    }

    void pop_back() {
        if (count_ == 0) {
            return;
        }
        std::uint32_t len;
        const std::uint32_t t = readTrail(tailEnd(), len);
        bytes_ -= static_cast<std::uint32_t>(2ull * t + len);
        --count_;
        maybeShrink();
    }

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }

    // Zero-allocation read. Returns a view into the ring when the payload is
    // contiguous, otherwise copies it into `scratch` and returns a view of that.
    // The view is invalidated by any mutation.
    std::string_view peek(std::size_t index, std::string& scratch) const {
        if (index >= count_) {
            throw std::out_of_range("ListValues::peek");
        }
        const std::uint32_t pos = find(index);
        std::uint32_t len;
        const std::uint32_t l = readLead(pos, len);
        const std::uint32_t p = wrap(std::uint64_t{pos} + l);
        if (std::uint64_t{p} + len <= cap_) {
            return {buf_.get() + p, len};
        }
        scratch.resize(len);
        copyOut(p, len, scratch.data());
        return scratch;
    }

    std::string at(std::size_t index) const {
        std::string scratch;
        const std::string_view v = peek(index, scratch);
        if (v.data() == scratch.data()) {
            return scratch;
        }
        return std::string(v);
    }

    std::string front() const { return at(0); }
    std::string back() const { return at(count_ - 1); }

    // LSET. Same-length values are overwritten in place; otherwise the buffer
    // is rebuilt once.
    void set(std::size_t index, std::string_view value) {
        if (index >= count_) {
            throw std::out_of_range("ListValues::set");
        }
        checkValueSize(value.size());
        const std::uint32_t pos = find(index);
        std::uint32_t oldLen;
        const std::uint32_t ol = readLead(pos, oldLen);
        if (value.size() == oldLen) {
            copyOut_in_place(wrap(std::uint64_t{pos} + ol), value);
            return;
        }
        const std::uint32_t n = static_cast<std::uint32_t>(value.size());
        const std::uint64_t oldRec = 2ull * ol + oldLen;
        const std::uint64_t newRec = 2ull * varintLen(n) + n;
        const std::uint64_t total = std::uint64_t{bytes_} - oldRec + newRec;
        if (total > kMaxCap) {
            throw std::length_error("ListValues: list too large");
        }
        const std::uint32_t newCap = roundUp16(std::min<std::uint64_t>(
            kMaxCap, std::max<std::uint64_t>(kMinCap, total + total / 4)));
        const std::uint32_t off = pos >= head_ ? pos - head_ : pos + cap_ - head_;

        ListValues out;
        out.buf_.reset(new char[newCap]);
        out.cap_ = newCap;
        copyOut(head_, off, out.buf_.get());  // records before `index`
        out.bytes_ = off;
        out.count_ = static_cast<std::uint32_t>(index);
        out.push(value, false);               // the replacement record
        const std::uint32_t suffix = static_cast<std::uint32_t>(bytes_ - off - oldRec);
        if (suffix != 0) {                    // records after `index`
            copyOut(wrap(std::uint64_t{pos} + oldRec), suffix,
                    out.buf_.get() + out.bytes_);
            out.bytes_ += suffix;
        }
        out.count_ += count_ - static_cast<std::uint32_t>(index) - 1;
        *this = std::move(out);
    }

    // Visit elements [start, start + count) front to back: one find() for the
    // first record, then a sequential walk. f(std::string_view). No allocation
    // except when a record wraps around the end of the buffer.
    template <class F>
    void forEachRange(std::size_t start, std::size_t count, F&& f) const {
        if (start >= count_) {
            return;
        }
        count = std::min<std::size_t>(count, count_ - start);
        std::string scratch;
        std::uint32_t pos = find(start);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t len;
            const std::uint32_t l = readLead(pos, len);
            const std::uint32_t p = wrap(std::uint64_t{pos} + l);
            if (std::uint64_t{p} + len <= cap_) {
                f(std::string_view(buf_.get() + p, len));
            } else {
                scratch.resize(len);
                copyOut(p, len, scratch.data());
                f(std::string_view(scratch));
            }
            pos = wrap(std::uint64_t{pos} + 2ull * l + len);
        }
    }

    // Visit every element front to back.
    template <class F>
    void forEach(F&& f) const {
        forEachRange(0, count_, std::forward<F>(f));
    }

    void clear() noexcept {
        buf_.reset();
        cap_ = head_ = bytes_ = count_ = 0;
    }

    // Reallocate to the smallest buffer that holds the current contents.
    void shrinkToFit() {
        if (count_ == 0) {
            clear();
        } else if (roundUp16(bytes_) < cap_) {
            reallocate(std::max(kMinCap, roundUp16(bytes_)));
        }
    }

    // Exact: the object itself plus the one heap block (before allocator
    // rounding / chunk header, which the allocator column in the CSV covers).
    std::size_t memoryUsage() const noexcept { return sizeof(*this) + cap_; }
    std::size_t usedBytes() const noexcept { return bytes_; }
    std::size_t capacityBytes() const noexcept { return cap_; }

private:
    static constexpr std::uint32_t kMinCap = 64;
    static constexpr std::uint64_t kMaxCap = 0x80000000ull;  // 2 GiB

    static std::uint32_t roundUp16(std::uint64_t n) noexcept {
        return static_cast<std::uint32_t>((n + 15) & ~std::uint64_t{15});
    }

    static std::uint32_t varintLen(std::uint32_t n) noexcept {
        std::uint32_t l = 1;
        while (n >= 0x80) {
            n >>= 7;
            ++l;
        }
        return l;
    }

    static void checkValueSize(std::size_t n) {
        if (n > kMaxCap / 2) {
            throw std::length_error("ListValues: value too large");
        }
    }

    // p must be < 2 * cap_.
    std::uint32_t wrap(std::uint64_t p) const noexcept {
        return static_cast<std::uint32_t>(p >= cap_ ? p - cap_ : p);
    }

    std::uint32_t tailEnd() const noexcept {
        return wrap(std::uint64_t{head_} + bytes_);  // one past the last record
    }

    void copyOut(std::uint32_t pos, std::uint32_t n, char* dst) const noexcept {
        if (n == 0) {
            return;
        }
        const std::uint32_t first = std::min(n, cap_ - pos);
        std::memcpy(dst, buf_.get() + pos, first);
        if (n > first) {
            std::memcpy(dst + first, buf_.get(), n - first);
        }
    }

    void copyIn(std::uint32_t pos, const char* src, std::uint32_t n) noexcept {
        if (n == 0) {
            return;
        }
        const std::uint32_t first = std::min(n, cap_ - pos);
        std::memcpy(buf_.get() + pos, src, first);
        if (n > first) {
            std::memcpy(buf_.get(), src + first, n - first);
        }
    }

    void copyOut_in_place(std::uint32_t pos, std::string_view v) noexcept {
        copyIn(pos, v.data(), static_cast<std::uint32_t>(v.size()));
    }

    std::uint32_t readLead(std::uint32_t pos, std::uint32_t& len) const noexcept {
        std::uint32_t v = 0, shift = 0, n = 0;
        for (;;) {
            const std::uint8_t b = static_cast<std::uint8_t>(buf_[pos]);
            pos = wrap(std::uint64_t{pos} + 1);
            ++n;
            v |= static_cast<std::uint32_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) {
                break;
            }
            shift += 7;
        }
        len = v;
        return n;
    }

    std::uint32_t writeLead(std::uint32_t pos, std::uint32_t len) noexcept {
        std::uint32_t n = 0;
        do {
            std::uint8_t b = len & 0x7f;
            len >>= 7;
            if (len) {
                b |= 0x80;
            }
            buf_[pos] = static_cast<char>(b);
            pos = wrap(std::uint64_t{pos} + 1);
            ++n;
        } while (len);
        return n;
    }

    void writeTrail(std::uint32_t pos, std::uint32_t len) noexcept {
        std::uint8_t groups[5];
        int k = 0;
        do {
            groups[k++] = len & 0x7f;
            len >>= 7;
        } while (len);
        for (int i = k - 1; i >= 0; --i) {  // highest group first, no flag on it
            std::uint8_t b = groups[i];
            if (i != k - 1) {
                b |= 0x80;
            }
            buf_[pos] = static_cast<char>(b);
            pos = wrap(std::uint64_t{pos} + 1);
        }
    }

    // endPos is one past the record's last byte. Returns the trail byte count.
    std::uint32_t readTrail(std::uint32_t endPos, std::uint32_t& len) const noexcept {
        std::uint32_t pos = endPos, v = 0, shift = 0, n = 0;
        for (;;) {
            pos = pos == 0 ? cap_ - 1 : pos - 1;
            const std::uint8_t b = static_cast<std::uint8_t>(buf_[pos]);
            ++n;
            v |= static_cast<std::uint32_t>(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) {
                break;
            }
        }
        len = v;
        return n;
    }

    // Start offset of record `index`, walking from the nearer end.
    std::uint32_t find(std::size_t index) const noexcept {
        if (index <= count_ / 2) {
            std::uint32_t pos = head_;
            for (std::size_t k = 0; k < index; ++k) {
                std::uint32_t len;
                const std::uint32_t l = readLead(pos, len);
                pos = wrap(std::uint64_t{pos} + 2ull * l + len);
            }
            return pos;
        }
        std::uint32_t end = tailEnd();
        for (std::size_t k = count_ - index; k > 0; --k) {
            std::uint32_t len;
            const std::uint32_t t = readTrail(end, len);
            const std::uint64_t rec = 2ull * t + len;
            end = end >= rec ? static_cast<std::uint32_t>(end - rec)
                             : static_cast<std::uint32_t>(end + std::uint64_t{cap_} - rec);
        }
        return end;
    }

    void push(std::string_view value, bool front) {
        checkValueSize(value.size());
        const std::uint32_t n = static_cast<std::uint32_t>(value.size());
        const std::uint32_t vl = varintLen(n);
        const std::uint64_t rec = 2ull * vl + n;
        ensureRoom(rec);

        std::uint32_t start;
        if (front) {
            start = head_ >= rec ? static_cast<std::uint32_t>(head_ - rec)
                                 : static_cast<std::uint32_t>(head_ + std::uint64_t{cap_} - rec);
            head_ = start;
        } else {
            start = tailEnd();
        }
        std::uint32_t pos = wrap(std::uint64_t{start} + writeLead(start, n));
        copyIn(pos, value.data(), n);
        pos = wrap(std::uint64_t{pos} + n);
        writeTrail(pos, n);
        bytes_ += static_cast<std::uint32_t>(rec);
        ++count_;
    }

    void ensureRoom(std::uint64_t need) {
        if (std::uint64_t{cap_} - bytes_ >= need) {
            return;
        }
        std::uint64_t target = std::max<std::uint64_t>(
            {std::uint64_t{cap_} + cap_ / 2, std::uint64_t{bytes_} + need, kMinCap});
        if (std::uint64_t{bytes_} + need > kMaxCap) {
            throw std::length_error("ListValues: list too large");
        }
        target = std::min<std::uint64_t>(roundUp16(target), kMaxCap);
        reallocate(static_cast<std::uint32_t>(target));
    }

    void maybeShrink() {
        if (count_ == 0) {
            clear();
            return;
        }
        if (cap_ > kMinCap && bytes_ < cap_ / 4) {
            const std::uint32_t newCap = std::max(kMinCap, roundUp16(std::uint64_t{bytes_} * 2));
            if (newCap < cap_) {
                reallocate(newCap);
            }
        }
    }

    // Move the live bytes, unwrapped, into a fresh buffer of newCap bytes.
    void reallocate(std::uint32_t newCap) {
        std::unique_ptr<char[]> nb(new char[newCap]);
        if (bytes_ != 0) {
            copyOut(head_, bytes_, nb.get());
        }
        buf_ = std::move(nb);
        cap_ = newCap;
        head_ = 0;
    }

    std::unique_ptr<char[]> buf_;
    std::uint32_t cap_ = 0;    // buffer size in bytes
    std::uint32_t head_ = 0;   // offset of the first record
    std::uint32_t bytes_ = 0;  // bytes occupied by records
    std::uint32_t count_ = 0;  // number of elements
};

// Drop-in shape of the original List wrapper.
struct List {
    ListValues values;
    std::size_t memoryUsage() const noexcept { return values.memoryUsage(); }
};

struct Hash {
    std::unordered_map<std::string, std::string> values;

    std::size_t memoryUsage() const noexcept {
        std::size_t bytes = 0;
        if (!values.empty()) {
            bytes += (values.bucket_count() + 1) * sizeof(void*);
        }
        bytes += values.size() * 80;
        for (const auto& [field, value] : values) {
            bytes += field.capacity() + value.capacity();
        }
        return bytes;
    }
};

struct Set {
    std::unordered_set<std::string> values;

    std::size_t memoryUsage() const noexcept {
        std::size_t bytes = 0;
        if (!values.empty()) {
            bytes += (values.bucket_count() + 1) * sizeof(void*);
        }
        bytes += values.size() * 48;
        for (const auto& value : values) {
            bytes += value.capacity();
        }
        return bytes;
    }
};

struct Object {
    using Data = std::variant<std::string, Hash, List, Set, std::unique_ptr<ZSet>>;

    Object() : type(ObjType::String), data(std::string{}) {}
    explicit Object(std::string value)
        : type(ObjType::String), data(std::move(value)) {}
    explicit Object(Hash value) : type(ObjType::Hash), data(std::move(value)) {}
    explicit Object(List value) : type(ObjType::List), data(std::move(value)) {}
    explicit Object(Set value) : type(ObjType::Set), data(std::move(value)) {}
    explicit Object(ZSet value)
        : type(ObjType::ZSet),
          data(std::make_unique<ZSet>(std::move(value))) {}
    explicit Object(std::unique_ptr<ZSet> value)
        : type(ObjType::ZSet), data(std::move(value)) {}

    Object(const Object& other)
        : type(other.type), access_meta(other.access_meta) {
        std::visit(
            [this](const auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ZSet>>) {
                    data = val ? std::make_unique<ZSet>(*val) : nullptr;
                } else {
                    data = val;
                }
            },
            other.data);
    }

    Object& operator=(const Object& other) {
        if (this != &other) {
            type = other.type;
            access_meta = other.access_meta;
            std::visit(
                [this](const auto& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ZSet>>) {
                        data = val ? std::make_unique<ZSet>(*val) : nullptr;
                    } else {
                        data = val;
                    }
                },
                other.data);
        }
        return *this;
    }

    Object(Object&&) noexcept = default;
    Object& operator=(Object&&) noexcept = default;
    ~Object() = default;

    std::size_t logicalSize(std::size_t keySize) const noexcept {
        std::size_t size = keySize + sizeof(Object);
        if (const auto* string = std::get_if<std::string>(&data)) {
            return size + string->capacity();
        }
        if (const auto* hash = std::get_if<Hash>(&data)) {
            return size + hash->memoryUsage();
        }
        if (const auto* list = std::get_if<List>(&data)) {
            return size + list->memoryUsage();
        }
        if (const auto* set = std::get_if<Set>(&data)) {
            return size + set->memoryUsage();
        }
        if (const auto* sortedSet = std::get_if<std::unique_ptr<ZSet>>(&data)) {
            if (*sortedSet) {
                return size + sizeof(ZSet) + (*sortedSet)->memoryUsage();
            }
        }
        return size;
    }

    ObjType type;
    Data data;
    std::uint32_t access_meta = 0;
};

}  // namespace redis_clone