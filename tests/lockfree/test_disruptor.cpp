#include <catch2/catch_test_macros.hpp>
#include <lockfree/disruptor.h>
#include <atomic>
#include <cstdint>
#include <thread>

using namespace marketlib::lockfree;

TEST_CASE("Disruptor: single producer single consumer", "[disruptor]") {
    Disruptor<int, 16> d;
    auto tok = d.subscribe();
    d.publish(42);
    const int& val = d.wait(tok);
    REQUIRE(val == 42);
    d.consumed(tok);
}

TEST_CASE("Disruptor: fan-out to two consumers sees all events", "[disruptor]") {
    // Capacity must exceed N: both consumers only start draining after every
    // event is published, so the producer would deadlock against its own
    // backpressure check if the ring filled before consumption began.
    Disruptor<int, 128> d;
    auto tok_a = d.subscribe();
    auto tok_b = d.subscribe();
    constexpr int N = 100;
    for (int i = 0; i < N; ++i) d.publish(i);
    for (int i = 0; i < N; ++i) {
        REQUIRE(d.wait(tok_a) == i); d.consumed(tok_a);
        REQUIRE(d.wait(tok_b) == i); d.consumed(tok_b);
    }
}

TEST_CASE("Disruptor: concurrent producer and consumers", "[disruptor][thread]") {
    static constexpr int N = 200'000;
    Disruptor<int, 65536> d;
    auto tok_a = d.subscribe();
    auto tok_b = d.subscribe();
    std::atomic<std::int64_t> sum_a{0}, sum_b{0};

    std::thread ca([&] {
        for (int i = 0; i < N; ++i) {
            sum_a.fetch_add(d.wait(tok_a), std::memory_order_relaxed);
            d.consumed(tok_a);
        }
    });
    std::thread cb([&] {
        for (int i = 0; i < N; ++i) {
            sum_b.fetch_add(d.wait(tok_b), std::memory_order_relaxed);
            d.consumed(tok_b);
        }
    });

    for (int i = 0; i < N; ++i) d.publish(i);
    ca.join(); cb.join();

    // N*(N-1)/2 overflows 32-bit int for N=200'000 — use 64-bit throughout.
    const std::int64_t expected = static_cast<std::int64_t>(N) * (N - 1) / 2;
    REQUIRE(sum_a.load() == expected);
    REQUIRE(sum_b.load() == expected);
}
