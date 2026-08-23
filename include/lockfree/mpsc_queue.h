#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

namespace marketlib::lockfree {

// Multiple-producer single-consumer lock-free ring buffer.
//
// Uses the Dmitry Vyukov MPMC design restricted to one consumer. Each slot
// carries its own sequence number so producers can claim independently via CAS.
// The consumer path is contention-free.
template<typename T, std::size_t N>
requires (N >= 2 && (N & (N - 1)) == 0)
class MpscQueue {
    struct alignas(64) Slot {
        std::atomic<std::size_t> seq;
        alignas(T) std::byte     storage[sizeof(T)];
    };

    struct alignas(128) Sequence {
        std::atomic<std::size_t> value{0};
    };

    static constexpr std::size_t kMask = N - 1;

    Slot     slots_[N];
    Sequence producer_;
    Sequence consumer_;

public:
    MpscQueue() noexcept {
        for (std::size_t i = 0; i < N; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }
    ~MpscQueue() { T tmp; while (try_pop(tmp)) {} }

    MpscQueue(const MpscQueue&)            = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    template<typename... Args>
    [[nodiscard]] bool try_push(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        std::size_t head = producer_.value.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot      = slots_[head & kMask];
            const auto seq  = slot.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq)
                            - static_cast<std::ptrdiff_t>(head);
            if (diff == 0) {
                if (producer_.value.compare_exchange_weak(
                        head, head + 1, std::memory_order_relaxed)) [[likely]]
                    break;
            } else if (diff < 0) {
                return false;
            } else {
                head = producer_.value.load(std::memory_order_relaxed);
            }
        }
        Slot& slot = slots_[head & kMask];
        std::construct_at(reinterpret_cast<T*>(slot.storage),
                          std::forward<Args>(args)...);
        slot.seq.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out)
        noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        const auto tail = consumer_.value.load(std::memory_order_relaxed);
        Slot& slot       = slots_[tail & kMask];
        const auto seq   = slot.seq.load(std::memory_order_acquire);
        const auto diff  = static_cast<std::ptrdiff_t>(seq)
                         - static_cast<std::ptrdiff_t>(tail + 1);
        if (diff != 0) [[unlikely]] return false;
        auto* ptr = reinterpret_cast<T*>(slot.storage);
        out = std::move(*ptr);
        std::destroy_at(ptr);
        slot.seq.store(tail + N, std::memory_order_release);
        consumer_.value.store(tail + 1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        const auto tail = consumer_.value.load(std::memory_order_relaxed);
        const auto seq  = slots_[tail & kMask].seq.load(std::memory_order_acquire);
        return static_cast<std::ptrdiff_t>(seq)
             - static_cast<std::ptrdiff_t>(tail + 1) != 0;
    }

    static constexpr std::size_t capacity() noexcept { return N; }
};

} // namespace marketlib::lockfree
