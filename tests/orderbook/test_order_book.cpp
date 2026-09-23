#include <catch2/catch_test_macros.hpp>
#include <orderbook/order_book.h>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

using namespace marketlib::orderbook;

// Counts global allocations while an AllocScope is alive. This is what
// actually checks the book's "no allocation after construction" claim:
// noexcept says nothing about allocation, and clang-tidy does not see it.
namespace {
std::atomic<std::size_t> g_allocs{0};
std::atomic<bool>        g_counting{false};

struct AllocScope {
    AllocScope()  { g_allocs.store(0); g_counting.store(true); }
    ~AllocScope() { g_counting.store(false); }
    [[nodiscard]] std::size_t count() const { return g_allocs.load(); }
};
} // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    std::abort();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

TEST_CASE("OrderBook: levels aggregate and sort best-first", "[orderbook]") {
    OrderBook<8, 64> book{7};
    REQUIRE(book.on_add(1, Side::Bid, 100, 10));
    REQUIRE(book.on_add(2, Side::Bid, 101, 5));
    REQUIRE(book.on_add(3, Side::Bid, 100, 7));
    REQUIRE(book.on_add(4, Side::Ask, 103, 2));
    REQUIRE(book.on_add(5, Side::Ask, 102, 4));

    REQUIRE(book.symbol_id() == 7);
    REQUIRE(book.sequence() == 5);

    REQUIRE(book.bids().size() == 2);
    REQUIRE(book.best_bid()->price == 101);
    REQUIRE(book.best_bid()->qty == 5);
    REQUIRE(book.bids()[1].price == 100);
    REQUIRE(book.bids()[1].qty == 17);
    REQUIRE(book.bids()[1].order_count == 2);

    REQUIRE(book.asks().size() == 2);
    REQUIRE(book.best_ask()->price == 102);
}

TEST_CASE("OrderBook: cancel removes quantity and empties levels", "[orderbook]") {
    OrderBook<8, 64> book{1};
    REQUIRE(book.on_add(1, Side::Bid, 100, 10));
    REQUIRE(book.on_add(2, Side::Bid, 100, 7));

    book.on_cancel(2);
    REQUIRE(book.bids().size() == 1);
    REQUIRE(book.best_bid()->qty == 10);
    REQUIRE(book.best_bid()->order_count == 1);

    book.on_cancel(1);
    REQUIRE(book.best_bid() == nullptr);

    const auto seq = book.sequence();
    book.on_cancel(999); // unknown id: ignored, not counted
    REQUIRE(book.sequence() == seq);
}

TEST_CASE("OrderBook: modify and execute", "[orderbook]") {
    OrderBook<8, 64> book{1};
    REQUIRE(book.on_add(1, Side::Bid, 100, 10));
    REQUIRE(book.on_add(2, Side::Ask, 105, 6));

    book.on_modify(1, 4);
    REQUIRE(book.best_bid()->qty == 4);

    book.on_execute(2, 2); // partial fill
    REQUIRE(book.best_ask()->qty == 4);
    REQUIRE(book.best_ask()->order_count == 1);

    book.on_execute(2, 4); // fills the rest: order and level go
    REQUIRE(book.best_ask() == nullptr);
}

TEST_CASE("OrderBook: snapshot is a top-of-book projection", "[orderbook]") {
    OrderBook<8, 64> book{3};
    REQUIRE(book.on_add(1, Side::Bid, 100, 1));
    REQUIRE(book.on_add(2, Side::Bid, 99, 2));
    REQUIRE(book.on_add(3, Side::Ask, 101, 3));

    const auto snap = book.snapshot<1>();
    REQUIRE(snap.symbol_id == 3);
    REQUIRE(snap.last_sequence == 3);
    REQUIRE(snap.bid_count == 1); // clamped to Depth
    REQUIRE(snap.bids[0].price == 100);
    REQUIRE(snap.ask_count == 1);
    REQUIRE(snap.asks[0].price == 101);
}

TEST_CASE("OrderBook: on_add refuses orders past MaxOrders and changes nothing", "[orderbook]") {
    OrderBook<4, 8> book{1};
    for (OrderId id = 1; id <= 8; ++id) REQUIRE(book.on_add(id, Side::Bid, 100, 2));

    const auto seq = book.sequence();
    REQUIRE_FALSE(book.on_add(9, Side::Bid, 100, 2)); // same level
    REQUIRE_FALSE(book.on_add(10, Side::Bid, 99, 2)); // would create a level
    REQUIRE(book.sequence() == seq);
    REQUIRE(book.bids().size() == 1);
    REQUIRE(book.best_bid()->qty == 16);
    REQUIRE(book.best_bid()->order_count == 8);

    book.on_cancel(1); // frees a slot
    REQUIRE(book.on_add(9, Side::Bid, 100, 2));
}

TEST_CASE("AllocScope counts allocations (control for the test below)", "[orderbook]") {
    std::size_t n = 0;
    {
        AllocScope scope;
        void* p = ::operator new(16); // direct call: the compiler may not elide it
        ::operator delete(p);
        n = scope.count();
    }
    REQUIRE(n >= 1);
}

TEST_CASE("OrderBook: no allocation after construction", "[orderbook]") {
    constexpr std::size_t kOrders = 256;
    OrderBook<8, kOrders> book{1}; // reserves the order table here, outside the scope

    bool all_added = true, overflow_rejected = false;
    std::size_t allocs = 0;
    {
        AllocScope scope;
        for (OrderId id = 1; id <= kOrders; ++id) {
            const auto side  = (id & 1) ? Side::Bid : Side::Ask;
            const Price base = (id & 1) ? 100 : 200;
            all_added &= book.on_add(id, side, base + static_cast<Price>(id % 4), 10);
        }
        overflow_rejected = !book.on_add(kOrders + 1, Side::Bid, 100, 10);
        for (OrderId id = 1; id <= kOrders; ++id) book.on_modify(id, 20);
        for (OrderId id = 1; id <= kOrders; id += 2) book.on_execute(id, 20);
        for (OrderId id = 2; id <= kOrders; id += 2) book.on_cancel(id);
        allocs = scope.count();
    }
    REQUIRE(all_added);
    REQUIRE(overflow_rejected);
    REQUIRE(book.best_bid() == nullptr);
    REQUIRE(book.best_ask() == nullptr);
    REQUIRE(allocs == 0);
}
