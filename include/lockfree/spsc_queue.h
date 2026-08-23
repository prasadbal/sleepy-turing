#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

namespace marketlib::lockfree {

// Single-producer single-consumer lock-free ring buffer.
//
// Sequences are padded to 128 bytes — not 64 — because Intel's L2 spatial
// prefetcher fetches cache lines in pairs; 64-byte padding still causes false
// sharing between producer and consumer sequences on the same 128-byte sector.
//
// Capacity N must be a power of 2. Index wrapping is a bitmask, never modulo.
template<typename T, std::size_t N>
requires (N >= 2 && (N & (N - 1)) == 0)
class SpscQueue {
    struct alignas(128) Sequence {
        std::atomic<std::size_t> value{0};
    };

    struct alignas(64) Slot {
        alignas(T) std::byte storage[sizeof(T)];
    };

    static constexpr std::size_t kMask = N - 1;

    Sequence producer_;
    Sequence consumer_;
    Slot     slots_[N];

public:
    SpscQueue() = default;
    ~SpscQueue() { T tmp; while (try_pop(tmp)) {} }

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    template<typename... Args>
    [[nodiscard]] bool try_push(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        const auto head = producer_.value.load(std::memory_order_relaxed);
        const auto tail = consumer_.value.load(std::memory_order_acquire);
        if (head - tail >= N) [[unlikely]] return false;
        std::construct_at(reinterpret_cast<T*>(slots_[head & kMask].storage),
                          std::forward<Args>(args)...);
        producer_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out)
        noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        const auto tail = consumer_.value.load(std::memory_order_relaxed);
        const auto head = producer_.value.load(std::memory_order_acquire);
        if (head == tail) [[unlikely]] return false;
        auto* ptr = reinterpret_cast<T*>(slots_[tail & kMask].storage);
        out = std::move(*ptr);
        std::destroy_at(ptr);
        consumer_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool        empty()    const noexcept {
        return producer_.value.load(std::memory_order_acquire) ==
               consumer_.value.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t size()     const noexcept {
        return producer_.value.load(std::memory_order_acquire) -
               consumer_.value.load(std::memory_order_acquire);
    }
    static constexpr std::size_t capacity() noexcept { return N; }
};

} // namespace marketlib::lockfree
