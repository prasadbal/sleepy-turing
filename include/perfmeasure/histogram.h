#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace marketlib::perfmeasure {

// Power-of-2 latency histogram. Bucket[i] counts samples in [2^i ns, 2^(i+1) ns).
// Single-writer, no atomics — use one instance per thread.
class LatencyHistogram {
public:
    static constexpr std::size_t kBuckets = 64;

    void record(std::uint64_t ns) noexcept {
        ++counts_[bucket_for(ns)];
        ++total_;
        if (ns < min_) min_ = ns;
        if (ns > max_) max_ = ns;
    }

    void reset() noexcept {
        counts_.fill(0);
        total_ = 0;
        min_   = std::uint64_t(-1);
        max_   = 0;
    }

    std::uint64_t count()  const noexcept { return total_; }
    std::uint64_t min_ns() const noexcept { return min_; }
    std::uint64_t max_ns() const noexcept { return max_; }

    std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(p * static_cast<double>(total_));
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            cum += counts_[i];
            if (cum > target) return std::uint64_t(1) << i;
        }
        return std::uint64_t(1) << (kBuckets - 1);
    }

    std::uint64_t p50()  const noexcept { return percentile(0.50); }
    std::uint64_t p99()  const noexcept { return percentile(0.99); }
    std::uint64_t p999() const noexcept { return percentile(0.999); }

    const std::array<std::uint64_t, kBuckets>& buckets() const noexcept { return counts_; }

private:
    static std::size_t bucket_for(std::uint64_t ns) noexcept {
        if (ns == 0) return 0;
#if defined(__GNUC__) || defined(__clang__)
        const std::size_t bit = 63u - static_cast<std::size_t>(__builtin_clzll(ns));
#else
        std::size_t bit = 0;
        for (std::uint64_t v = ns; v > 1; v >>= 1) ++bit;
#endif
        return std::min(bit, kBuckets - 1);
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_{0};
    std::uint64_t min_{std::uint64_t(-1)};
    std::uint64_t max_{0};
};

} // namespace marketlib::perfmeasure
