#include <catch2/catch_test_macros.hpp>
#include <perfmeasure/tsc.h>
#include <perfmeasure/histogram.h>
#include <chrono>
#include <thread>

using namespace marketlib::perfmeasure;
using namespace std::chrono_literals;

TEST_CASE("rdtsc: advances monotonically", "[tsc]") {
    REQUIRE(rdtsc() <= rdtsc());
}

TEST_CASE("TscClock: calibration within 5% of wall clock over 50ms", "[tsc]") {
    const auto clk    = TscClock::calibrate();
    const auto t0_tsc = rdtsc();
    const auto t0_w   = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(50ms);
    const auto t1_tsc = rdtsc();
    const auto t1_w   = std::chrono::steady_clock::now();

    const double tsc_ns  = static_cast<double>(t1_tsc - t0_tsc) * clk.ns_per_tick();
    const double wall_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1_w - t0_w).count());

    REQUIRE(std::abs(tsc_ns - wall_ns) / wall_ns < 0.05);
}

TEST_CASE("LatencyHistogram: records and reports percentiles", "[histogram]") {
    LatencyHistogram h;
    for (std::uint64_t ns = 100; ns <= 10000; ns += 100) h.record(ns);
    REQUIRE(h.count()   == 100);
    REQUIRE(h.min_ns()  == 100);
    REQUIRE(h.max_ns()  == 10000);
    REQUIRE(h.p50()     >= 2048);
    REQUIRE(h.p50()     <= 8192);
    REQUIRE(h.p99()     >= 4096);
}

TEST_CASE("LatencyHistogram: reset clears all state", "[histogram]") {
    LatencyHistogram h;
    h.record(500);
    h.reset();
    REQUIRE(h.count() == 0);
    REQUIRE(h.min_ns() == std::uint64_t(-1));
}
