#pragma once
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <perfmeasure/tsc.h>
#include <cstdint>
#include <thread>

namespace marketlib::testing {

template<typename Pred>
void spin_until(Pred&& pred, std::uint64_t timeout_ns = 5'000'000'000ULL) {
    const auto deadline = perfmeasure::rdtsc() +
        static_cast<std::uint64_t>(timeout_ns * 3.0);
    while (!pred()) {
        if (perfmeasure::rdtsc() > deadline)
            FAIL("spin_until timed out");
        std::this_thread::yield();
    }
}

} // namespace marketlib::testing
