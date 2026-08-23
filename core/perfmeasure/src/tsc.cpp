#include <perfmeasure/tsc.h>
#include <chrono>
#include <thread>

namespace marketlib::perfmeasure {

TscClock TscClock::calibrate() noexcept {
    using namespace std::chrono;
    using Clock = steady_clock;

    (void)rdtsc();  // warm up

    const auto t0_wall = Clock::now();
    const auto t0_tsc  = rdtsc();

    std::this_thread::sleep_for(milliseconds(10));

    const auto t1_tsc  = rdtsc();
    const auto t1_wall = Clock::now();

    const double elapsed_ns    = static_cast<double>(
        duration_cast<nanoseconds>(t1_wall - t0_wall).count());
    const double elapsed_ticks = static_cast<double>(t1_tsc - t0_tsc);

    TscClock clk;
    clk.ns_per_tick_ = elapsed_ns / elapsed_ticks;
    clk.base_tsc_    = t0_tsc;
    clk.base_ns_     = static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count());
    return clk;
}

} // namespace marketlib::perfmeasure
