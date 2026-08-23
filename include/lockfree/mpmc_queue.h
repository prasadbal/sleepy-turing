#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

namespace marketlib::lockfree {

// Multiple-producer multiple-consumer bounded ring buffer (Dmitry Vyukov's
// bounded MPMC queue).
//
// This is MpscQueue generalized to the consumer side: MpscQueue lets many
// producers race a CAS to claim a slot, but hands the single consumer a
// plain atomic counter because only one consumer thread ever touches it.
// Here consumers can race each other too, so the tail counter needs the same
// CAS-claim treatment the head counter already gets — every slot carries its
// own sequence number, and BOTH ends claim slots by winning a CAS against
// that slot's expected sequence, not by simply advancing a shared counter.
//
// Slot state machine (index i, generation g means "the (i + g*N)-th push"):
//   seq == i           -> empty, ready for a producer to claim (push g=0)
//   seq == i + 1       -> full, ready for a consumer to claim
//   seq == i + N       -> empty again, ready for push generation g=1
// A claimant CAS-advances its own head/tail counter only after confirming
// the slot's sequence matches what its position expects; losing the race
// (seq changed under it) just means retrying with a fresh position, never
// blocking.
template<typename T, std::size_t N>
requires (N >= 2 && (N & (N - 1)) == 0)
class MpmcQueue {
    struct alignas(64) Slot {
        std::atomic<std::size_t> seq;
        alignas(T) std::byte     storage[sizeof(T)];
    };

    struct alignas(128) Sequence { std::atomic<std::size_t> value{0}; };

    static constexpr std::size_t kMask = N - 1;

    Slot     slots_[N];
    Sequence head_; // next slot a producer will try to claim
    Sequence tail_; // next slot a consumer will try to claim

public:
    MpmcQueue() noexcept {
        for (std::size_t i = 0; i < N; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }
    ~MpmcQueue() { T tmp; while (try_pop(tmp)) {} }

    MpmcQueue(const MpmcQueue&)            = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    template<typename... Args>
    [[nodiscard]] bool try_push(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        Slot* slot;
        std::size_t pos = head_.value.load(std::memory_order_relaxed);
        for (;;) {
            slot            = &slots_[pos & kMask];
            const auto seq  = slot->seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq)
                            - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (head_.value.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) [[likely]]
                    break;
                // lost the race to another producer — pos was refreshed by CAS, retry
            } else if (diff < 0) {
                return false; // full: this slot is still awaiting a consumer
            } else {
                pos = head_.value.load(std::memory_order_relaxed);
            }
        }
        std::construct_at(reinterpret_cast<T*>(slot->storage),
                          std::forward<Args>(args)...);
        // Publish: seq == pos + 1 marks the slot full and hands it to consumers.
        slot->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out)
        noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        Slot* slot;
        std::size_t pos = tail_.value.load(std::memory_order_relaxed);
        for (;;) {
            slot            = &slots_[pos & kMask];
            const auto seq  = slot->seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq)
                            - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (tail_.value.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) [[likely]]
                    break;
            } else if (diff < 0) {
                return false; // empty: this slot hasn't been published yet
            } else {
                pos = tail_.value.load(std::memory_order_relaxed);
            }
        }
        auto* ptr = reinterpret_cast<T*>(slot->storage);
        out = std::move(*ptr);
        std::destroy_at(ptr);
        // Publish: seq == pos + N marks the slot empty for push generation +1.
        slot->seq.store(pos + N, std::memory_order_release);
        return true;
    }

    // Racy under concurrent access from either side — a snapshot, not a
    // guarantee. Fine for metrics/logging, not for deciding push/pop safety
    // (try_push/try_pop already do that correctly via the CAS-claim above).
    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        const auto h = head_.value.load(std::memory_order_acquire);
        const auto t = tail_.value.load(std::memory_order_acquire);
        return h > t ? h - t : 0;
    }
    static constexpr std::size_t capacity() noexcept { return N; }
};

} // namespace marketlib::lockfree
