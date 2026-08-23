#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <lockfree/spsc_queue.h>
#include <atomic>
#include <thread>

using namespace marketlib::lockfree;

TEST_CASE("SpscQueue: basic push and pop", "[spsc]") {
    SpscQueue<int, 16> q;
    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);
    REQUIRE(q.try_push(42));
    REQUIRE_FALSE(q.empty());
    int val{};
    REQUIRE(q.try_pop(val));
    REQUIRE(val == 42);
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue: fills to capacity, rejects when full", "[spsc]") {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) REQUIRE(q.try_push(i));
    REQUIRE_FALSE(q.try_push(99));
}

TEST_CASE("SpscQueue: preserves FIFO order", "[spsc]") {
    SpscQueue<int, 8> q;
    for (int i = 0; i < 8; ++i) REQUIRE(q.try_push(i));
    for (int i = 0; i < 8; ++i) {
        int val{};
        REQUIRE(q.try_pop(val));
        REQUIRE(val == i);
    }
}

TEST_CASE("SpscQueue: wrap-around across ring boundary", "[spsc]") {
    SpscQueue<int, 4> q;
    int val{};
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 4; ++i) REQUIRE(q.try_push(i));
        for (int i = 0; i < 4; ++i) { REQUIRE(q.try_pop(val)); REQUIRE(val == i); }
    }
}

TEST_CASE("SpscQueue: producer-consumer correctness under threading", "[spsc][thread]") {
    SpscQueue<int, 1024> q;
    constexpr int N = 500'000;
    std::thread producer([&] {
        for (int i = 0; i < N; ++i) while (!q.try_push(i)) {}
    });
    int expected = 0;
    while (expected < N) {
        int val{};
        if (q.try_pop(val)) { REQUIRE(val == expected); ++expected; }
    }
    producer.join();
}

TEST_CASE("SpscQueue: throughput benchmark", "[spsc][!benchmark]") {
    SpscQueue<std::uint64_t, 65536> q;
    BENCHMARK("push+pop pair") {
        std::uint64_t v = 1;
        bool pushed = q.try_push(v);
        std::uint64_t out{};
        bool popped = q.try_pop(out);
        return pushed && popped;
    };
}
