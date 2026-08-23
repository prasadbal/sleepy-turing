#pragma once
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#endif

namespace marketlib::perfmeasure {

inline std::uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif
}

inline std::uint64_t rdtscp(std::uint32_t& aux) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtscp(&aux);
#else
    aux = 0; return rdtsc();
#endif
}

inline std::uint64_t rdtscp() noexcept {
    std::uint32_t aux; return rdtscp(aux);
}

class TscClock {
public:
    static TscClock calibrate() noexcept;

    std::uint64_t now_ns() const noexcept { return tsc_to_ns(rdtsc()); }

    std::uint64_t tsc_to_ns(std::uint64_t tsc) const noexcept {
        return base_ns_ + static_cast<std::uint64_t>(
            static_cast<double>(tsc - base_tsc_) * ns_per_tick_);
    }

    double ns_per_tick()  const noexcept { return ns_per_tick_; }
    double ticks_per_ns() const noexcept { return 1.0 / ns_per_tick_; }

private:
    double        ns_per_tick_{1.0};
    std::uint64_t base_tsc_{0};
    std::uint64_t base_ns_{0};
};

} // namespace marketlib::perfmeasure
