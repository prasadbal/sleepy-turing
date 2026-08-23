#pragma once
#include <hashmap/hashmap.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace marketlib::orderbook {

// Design note — local vs. shared memory, and why a seqlock either way:
//
// The book below is NOT thread-safe and deliberately has no internal lock.
// It is meant to be owned exclusively by one ThreadPool worker (selected via
// submit_by_key(symbol_id, ...) so the same symbol always lands on the same
// thread). Single-writer, in-process, thread-local: mutation is a plain
// sequence of array/hashmap writes, no atomics needed at all on this path.
// That's the fast path and it stays fast because nothing else ever touches it.
//
// The moment anything OTHER than the owning thread needs to see the book —
// a strategy thread reading top-of-book, a risk process, a monitoring
// dashboard — you have a single-writer/multi-reader problem, and that's
// exactly what SeqLock<T> (include/lockfree/seqlock.h) is for. Two cases:
//
//   Same process (e.g. a strategy thread colocated with the book owner):
//     wrap Snapshot in a plain lockfree::SeqLock<Snapshot> member, or push
//     snapshots down a queue if the reader can tolerate one hop of latency.
//
//   Cross-process (risk engine, market-data replay recorder, GUI):
//     the seqlock has to live in memory those processes can map, i.e. shared
//     memory — see shared_book_table.h, which is exactly "array of
//     SeqLock<Snapshot> in an shm segment, one slot per symbol."
//
// Either way: keep the canonical book thread-local and unlocked; only the
// *published snapshot* — a small, trivially-copyable, top-of-book struct —
// crosses a synchronization boundary, and a seqlock is the right tool there
// because the writer (the book-owning thread, already the hottest thing in
// the system) must never block on a slow reader.

using OrderId = std::uint64_t;
using Price   = std::int64_t;  // integer ticks — never float compares on the book
using Qty     = std::uint64_t;

enum class Side : std::uint8_t { Bid, Ask };

struct PriceLevel {
    Price       price{0};
    Qty         qty{0};
    std::uint32_t order_count{0};
};

// Fixed-depth top-of-book snapshot — trivially copyable, safe to memcpy across
// a seqlock or into shared memory. Depth capped deliberately: this is what
// gets published, not the full book.
template<std::size_t Depth = 5>
struct Snapshot {
    std::uint32_t symbol_id{0};
    std::uint64_t last_sequence{0};
    std::uint32_t bid_count{0};
    std::uint32_t ask_count{0};
    std::array<PriceLevel, Depth> bids{};
    std::array<PriceLevel, Depth> asks{};
};

// Array-based limit order book for a single symbol. MaxLevels is small on
// purpose (HFT books rarely need full depth resident) — insert/remove is
// O(MaxLevels) via shift, which beats a tree/skiplist at this size and,
// critically, never allocates.
template<std::size_t MaxLevels = 32, std::size_t MaxOrders = 8192>
class OrderBook {
public:
    explicit OrderBook(std::uint32_t symbol_id) noexcept : symbol_id_(symbol_id) {}

    void on_add(OrderId id, Side side, Price price, Qty qty) noexcept {
        auto& levels     = side == Side::Bid ? bids_ : asks_;
        auto& count      = side == Side::Bid ? num_bids_ : num_asks_;
        const auto level_idx = find_or_insert_level(levels, count, side, price);
        levels[level_idx].qty += qty;
        ++levels[level_idx].order_count;
        orders_[id] = OrderLoc{.side = side, .price = price, .qty = qty};
        ++sequence_;
    }

    void on_cancel(OrderId id) noexcept {
        auto it = orders_.find(id);
        if (it == orders_.end()) [[unlikely]] return;
        remove_qty(it.value());
        orders_.erase(it);
        ++sequence_;
    }

    void on_modify(OrderId id, Qty new_qty) noexcept {
        auto it = orders_.find(id);
        if (it == orders_.end()) [[unlikely]] return;
        auto& loc   = it.value();
        auto* level = find_level(loc.side, loc.price);
        if (!level) [[unlikely]] return;
        level->qty = level->qty - loc.qty + new_qty;
        loc.qty    = new_qty;
        ++sequence_;
    }

    void on_execute(OrderId id, Qty exec_qty) noexcept {
        auto it = orders_.find(id);
        if (it == orders_.end()) [[unlikely]] return;
        auto& loc   = it.value();
        auto* level = find_level(loc.side, loc.price);
        if (!level) [[unlikely]] return;
        level->qty -= exec_qty;
        loc.qty    -= exec_qty;
        if (loc.qty == 0) {
            --level->order_count;
            const bool level_empty = level->order_count == 0;
            orders_.erase(it);
            if (level_empty) erase_level(loc.side, loc.price);
        }
        ++sequence_;
    }

    [[nodiscard]] const PriceLevel* best_bid() const noexcept {
        return num_bids_ ? &bids_[0] : nullptr;
    }
    [[nodiscard]] const PriceLevel* best_ask() const noexcept {
        return num_asks_ ? &asks_[0] : nullptr;
    }
    [[nodiscard]] std::span<const PriceLevel> bids() const noexcept { return {bids_.data(), num_bids_}; }
    [[nodiscard]] std::span<const PriceLevel> asks() const noexcept { return {asks_.data(), num_asks_}; }
    [[nodiscard]] std::uint32_t symbol_id() const noexcept { return symbol_id_; }
    [[nodiscard]] std::uint64_t sequence()  const noexcept { return sequence_; }

    // Trivially-copyable projection — this, not the book itself, is what
    // should cross a SeqLock/shared-memory boundary. See file-level note.
    template<std::size_t Depth = 5>
    [[nodiscard]] Snapshot<Depth> snapshot() const noexcept {
        Snapshot<Depth> s{};
        s.symbol_id     = symbol_id_;
        s.last_sequence = sequence_;
        s.bid_count     = static_cast<std::uint32_t>(std::min(num_bids_, Depth));
        s.ask_count     = static_cast<std::uint32_t>(std::min(num_asks_, Depth));
        for (std::size_t i = 0; i < s.bid_count; ++i) s.bids[i] = bids_[i];
        for (std::size_t i = 0; i < s.ask_count; ++i) s.asks[i] = asks_[i];
        return s;
    }

private:
    // Orders reference their level by price, not by array index: index-based
    // refs go stale the instant a shallower level is erased and the array
    // shifts. Re-finding by price is a bounded linear scan (MaxLevels is
    // small, ~32) and keeps every other order's reference valid for free.
    struct OrderLoc { Side side; Price price; Qty qty; };

    std::uint32_t symbol_id_;
    std::uint64_t sequence_{0};

    std::array<PriceLevel, MaxLevels> bids_{};
    std::size_t                       num_bids_{0};
    std::array<PriceLevel, MaxLevels> asks_{};
    std::size_t                       num_asks_{0};

    marketlib::HashMap<OrderId, OrderLoc> orders_;

    [[nodiscard]] PriceLevel* find_level(Side side, Price price) noexcept {
        auto& levels = side == Side::Bid ? bids_ : asks_;
        auto& count  = side == Side::Bid ? num_bids_ : num_asks_;
        for (std::size_t i = 0; i < count; ++i)
            if (levels[i].price == price) return &levels[i];
        return nullptr;
    }

    // Bids sorted descending, asks ascending — index 0 is always best.
    [[nodiscard]] std::size_t find_or_insert_level(
        std::array<PriceLevel, MaxLevels>& levels, std::size_t& count,
        Side side, Price price) noexcept
    {
        std::size_t i = 0;
        for (; i < count; ++i) {
            if (levels[i].price == price) return i;
            const bool past = side == Side::Bid ? levels[i].price < price
                                                  : levels[i].price > price;
            if (past) break;
        }
        if (count < MaxLevels) {
            for (std::size_t j = count; j > i; --j) levels[j] = levels[j - 1];
            levels[i] = PriceLevel{.price = price, .qty = 0, .order_count = 0};
            ++count;
        }
        return i; // if at capacity and no room, caller aggregates into last visible level
    }

    void erase_level(Side side, Price price) noexcept {
        auto& levels = side == Side::Bid ? bids_ : asks_;
        auto& count  = side == Side::Bid ? num_bids_ : num_asks_;
        for (std::size_t i = 0; i < count; ++i) {
            if (levels[i].price != price) continue;
            for (std::size_t j = i; j + 1 < count; ++j) levels[j] = levels[j + 1];
            --count;
            return;
        }
    }

    void remove_qty(const OrderLoc& loc) noexcept {
        auto* level = find_level(loc.side, loc.price);
        if (!level) [[unlikely]] return;
        level->qty -= loc.qty;
        if (--level->order_count == 0) erase_level(loc.side, loc.price);
    }
};

} // namespace marketlib::orderbook
