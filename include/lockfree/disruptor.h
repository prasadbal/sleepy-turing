#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64)
#  include <immintrin.h>
#  define SLEEPY_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#  define SLEEPY_CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#  define SLEEPY_CPU_PAUSE() ((void)0)
#endif

namespace marketlib::lockfree {

// Single-producer multi-consumer (fan-out) ring buffer — the Disruptor pattern.
//
// All consumers see every event (broadcast). The producer stalls only when the
// slowest consumer is a full buffer behind.
template<typename T, std::size_t N, std::size_t MaxConsumers = 8>
requires (N >= 2 && (N & (N - 1)) == 0)
class Disruptor {
public:
    static constexpr std::size_t  kCapacity     = N;
    static constexpr std::size_t  kMaxConsumers = MaxConsumers;
    static constexpr std::int64_t kInitial      = -1;

    struct ConsumerToken {
        std::size_t  id{};
        std::int64_t next{0};
    };

private:
    struct alignas(128) Cursor { std::atomic<std::int64_t> value{kInitial}; };
    struct alignas(64)  Slot   { T data; };

    static constexpr std::int64_t kMask = static_cast<std::int64_t>(N) - 1;

    Cursor                           producer_;
    std::array<Cursor, MaxConsumers> consumers_;
    std::size_t                      num_consumers_{0};
    Slot                             slots_[N];

    std::int64_t min_consumer_seq() const noexcept {
        std::int64_t min = std::numeric_limits<std::int64_t>::max();
        for (std::size_t i = 0; i < num_consumers_; ++i)
            min = std::min(min, consumers_[i].value.load(std::memory_order_acquire));
        return min;
    }

    // Kept private: publish(T) is the only externally safe entry point. A
    // caller with a bare next_slot()/commit() pair could call commit()
    // without ever having written the slot (publishing garbage to
    // consumers), or claim a slot and never commit it (silently stalling the
    // ring for every producer past capacity) — the exact "forgot to pair
    // next()/publish()" footgun the real LMAX Disruptor's public API is
    // known for. If in-place mutation (skipping the extra move in
    // publish(T)) is ever needed for a large T, expose it as an RAII guard
    // that commits in its destructor, not as these two raw calls.
    [[nodiscard]] T& next_slot() noexcept {
        const std::int64_t next = producer_.value.load(std::memory_order_relaxed) + 1;
        while (next - min_consumer_seq() > static_cast<std::int64_t>(N))
            SLEEPY_CPU_PAUSE();
        return slots_[next & kMask].data;
    }

    void commit() noexcept {
        const std::int64_t next = producer_.value.load(std::memory_order_relaxed) + 1;
        producer_.value.store(next, std::memory_order_release);
    }

public:
    Disruptor() = default;
    Disruptor(const Disruptor&)            = delete;
    Disruptor& operator=(const Disruptor&) = delete;

    [[nodiscard]] ConsumerToken subscribe() noexcept {
        assert(num_consumers_ < MaxConsumers);
        return ConsumerToken{ .id = num_consumers_++, .next = 0 };
    }

    void publish(T event) noexcept {
        next_slot() = std::move(event);
        commit();
    }

    const T& wait(ConsumerToken& tok) const noexcept {
        while (producer_.value.load(std::memory_order_acquire) < tok.next)
            SLEEPY_CPU_PAUSE();
        return slots_[tok.next & kMask].data;
    }

    const T* try_consume(ConsumerToken& tok) const noexcept {
        if (producer_.value.load(std::memory_order_acquire) < tok.next)
            return nullptr;
        return &slots_[tok.next & kMask].data;
    }

    void consumed(ConsumerToken& tok) noexcept {
        consumers_[tok.id].value.store(tok.next, std::memory_order_release);
        ++tok.next;
    }

    std::int64_t published_seq() const noexcept {
        return producer_.value.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return N; }
};

} // namespace marketlib::lockfree
