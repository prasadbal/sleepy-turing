#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace marketlib::lockfree {

// Single-writer, multi-reader sequence lock (Linux seqlock pattern).
//
// Use this — not a mutex — at any point where a hot-path writer (e.g. the
// single thread that owns an order book) must publish a snapshot for other
// threads/processes to read without ever blocking the writer. Readers retry
// instead of blocking; the writer never waits on a reader.
//
// T must be trivially copyable: the payload is memcpy'd in and out, never
// constructed/destructed under the lock.
//
// The whole object — not the individual members — is aligned to 128B (Intel's
// L2 spatial prefetcher fetches 64B lines in pairs). seq_ and payload_ are
// never touched independently: every writer call and every reader call
// touches both, together, so splitting them onto separate lines would only
// double the cache-line traffic per operation for no benefit. The real
// false-sharing risk is between *adjacent* SeqLock instances — e.g. two
// symbols' slots in SharedBookTable's array, owned/read by different threads
// — and padding the object as a whole is what guards against that.
template<typename T>
requires std::is_trivially_copyable_v<T>
class alignas(128) SeqLock {
    std::atomic<std::uint64_t> seq_{0};
    T                          payload_{};

public:
    SeqLock() = default;
    SeqLock(const SeqLock&)            = delete;
    SeqLock& operator=(const SeqLock&) = delete;

    // Writer side — single writer only, never call concurrently from two threads.
    void write(const T& value) noexcept {
        const auto s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);      // now odd: write in flight
        std::atomic_thread_fence(std::memory_order_release);
        payload_ = value;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);      // back to even: stable
    }

    // Reader side — lock-free, retries if it races a concurrent write.
    // Never blocks the writer; bounded only by writer frequency vs. read cost.
    [[nodiscard]] T read() const noexcept {
        T out;
        std::uint64_t s0, s1;
        do {
            s0 = seq_.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_acquire);
            out = payload_;
            std::atomic_thread_fence(std::memory_order_acquire);
            s1 = seq_.load(std::memory_order_acquire);
        } while (s0 != s1 || (s0 & 1) != 0);
        return out;
    }

    // Non-blocking single-attempt read. Returns false if it caught a write in
    // flight — caller can skip this cycle rather than spin (useful for readers
    // that sample opportunistically, e.g. a monitoring/UI thread).
    [[nodiscard]] bool try_read(T& out) const noexcept {
        const auto s0 = seq_.load(std::memory_order_acquire);
        if (s0 & 1) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        out = payload_;
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto s1 = seq_.load(std::memory_order_acquire);
        return s0 == s1;
    }
};

} // namespace marketlib::lockfree
