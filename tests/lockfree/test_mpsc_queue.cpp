#include <catch2/catch_test_macros.hpp>
#include <lockfree/mpsc_queue.h>
#include <atomic>
#include <thread>
#include <vector>

using namespace marketlib::lockfree;

TEST_CASE("MpscQueue: single producer basic ops", "[mpsc]") {
    MpscQueue<int, 16> q;
    REQUIRE(q.empty());
    REQUIRE(q.try_push(1));
    REQUIRE_FALSE(q.empty());
    int val{};
    REQUIRE(q.try_pop(val));
    REQUIRE(val == 1);
    REQUIRE(q.empty());
}

TEST_CASE("MpscQueue: rejects when full", "[mpsc]") {
    MpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) REQUIRE(q.try_push(i));
    REQUIRE_FALSE(q.try_push(99));
}

TEST_CASE("MpscQueue: multi-producer single consumer", "[mpsc][thread]") {
    static constexpr int kProducers   = 4;
    static constexpr int kPerProducer = 100'000;

    MpscQueue<int, 65536> q;
    std::atomic<int> total_consumed{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p)
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i)
                while (!q.try_push(i)) {}
        });

    const int expected = kProducers * kPerProducer;
    int consumed = 0;
    while (consumed < expected) {
        int val{};
        if (q.try_pop(val)) ++consumed;
    }
    total_consumed.store(consumed);
    for (auto& t : producers) t.join();
    REQUIRE(total_consumed.load() == expected);
}
