#pragma once
#include <perfmeasure/tsc.h>
#include <chrono>
#include <cstdint>

namespace marketlib::time {

using Nanos        = std::int64_t;
using MktTimestamp = std::int64_t;  // nanoseconds since Unix epoch — fits in a register

inline MktTimestamp now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline MktTimestamp now_ns(const perfmeasure::TscClock& clk) noexcept {
    return static_cast<MktTimestamp>(clk.now_ns());
}

constexpr Nanos us(std::int64_t n) noexcept { return n * 1'000LL; }
constexpr Nanos ms(std::int64_t n) noexcept { return n * 1'000'000LL; }
constexpr Nanos  s(std::int64_t n) noexcept { return n * 1'000'000'000LL; }

inline MktTimestamp from_session_ns(std::uint64_t ns_since_midnight,
                                    MktTimestamp  session_midnight_utc) noexcept {
    return session_midnight_utc + static_cast<MktTimestamp>(ns_since_midnight);
}

} // namespace marketlib::time
