#pragma once
#include <lockfree/mpsc_queue.h>
#include <threadpool/log.h>
#include <logging/macros.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#  include <immintrin.h>
#  define MARKETLIB_TP_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#  define MARKETLIB_TP_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#  define MARKETLIB_TP_PAUSE() ((void)0)
#endif

#if defined(__linux__)
#  include <pthread.h>
#endif

namespace marketlib::threadpool {

// ── ThreadPool ────────────────────────────────────────────────────────────────
//
// A dispatching thread pool built for symbol-partitioned market data, not
// generic task farming. Every worker owns a private lockfree::MpscQueue<Task>
// (many producers — NIC/feed threads, other workers — one consumer: the
// worker itself). There is no global queue and no work stealing: once a task
// lands on a worker's queue it is only ever popped by that worker.
//
// Two dispatch modes share the same underlying transport:
//
//   Stateless — submit_any(): round-robins across all workers. Use this when
//   tasks are independent of any key (e.g. housekeeping, stats flush). Cheap,
//   but gives no ordering guarantee between two tasks for the "same" logical
//   stream — don't use it for anything that must serialize per symbol.
//
//   Affinity  — submit_by_key(key, task): hash(key) % num_workers always
//   picks the same worker for the same key. This is the mode order-book
//   building depends on: every message for a given symbol must land on the
//   same thread, in the order it arrived, or the book built from it is
//   wrong. Because the mapping is stable, each worker can own its symbols'
//   books outright with zero cross-thread synchronization on the book itself.
//
//   Groups — submit_to_group(group, key, task): same idea, scoped to a named
//   subset of workers (e.g. one group per NUMA node, or one group per venue/
//   feed so a CME outage can't starve Nasdaq processing). hash(key) is taken
//   modulo the group's worker count, not the whole pool.
//
// Handlers are plain function pointers + an opaque context pointer, not
// std::function — no vtable, no possible heap allocation on the hot path.
// Each worker's handler+ctx is fixed at spawn time (typically ctx is that
// worker's private OrderBook / OrderBookSet).
template<typename Task, std::size_t QueueCapacity, std::size_t MaxWorkers = 64>
requires (QueueCapacity >= 2 && (QueueCapacity & (QueueCapacity - 1)) == 0)
class ThreadPool {
public:
    using HandlerFn = void (*)(Task&, void* ctx) noexcept;

    ThreadPool()                             = default;
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() { stop(); }

    // Register a worker that takes tasks from submit_any / submit_by_key.
    // cpu_id pins the thread via pthread affinity (Linux); nullopt leaves it
    // to the scheduler. Must be called before start().
    [[nodiscard]] std::size_t spawn_worker(HandlerFn fn, void* ctx,
                                            std::optional<int> cpu_id = std::nullopt) {
        return add_worker(fn, ctx, cpu_id);
    }

    // Register a named contiguous block of workers as a group, for
    // submit_to_group(). `ctxs` must have one entry per worker — each worker
    // in a group gets its own context, never a context shared across the
    // group. That matters for symbol dispatch: two workers in the same group
    // own disjoint sets of symbols, so their per-worker OrderBook storage
    // (typically a HashMap<symbol_id, OrderBook> behind ctx) must be
    // separate objects, not one map two threads would otherwise race on.
    // cpu_ids, if given, must also have one entry per worker.
    [[nodiscard]] std::size_t spawn_group(std::string_view name, HandlerFn fn,
                                          std::span<void* const> ctxs,
                                          std::span<const int> cpu_ids = {}) {
        const std::size_t begin = num_workers_;
        const std::size_t count = ctxs.size();
        for (std::size_t i = 0; i < count; ++i) {
            const std::optional<int> cpu = (i < cpu_ids.size())
                ? std::optional<int>{cpu_ids[i]} : std::nullopt;
            add_worker(fn, ctxs[i], cpu);
        }
        groups_.push_back(GroupInfo{.name = std::string(name), .begin = begin, .count = count});
        return groups_.size() - 1;
    }

    void start() {
        running_.store(true, std::memory_order_release);
        threads_.reserve(num_workers_);
        for (std::size_t i = 0; i < num_workers_; ++i)
            threads_.emplace_back([this, i](std::stop_token st) { worker_loop(i, st); });
        LOG_INFO(log, "ThreadPool started: {} workers, {} groups",
                 num_workers_, groups_.size());
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        for (auto& t : threads_) t.request_stop();
        threads_.clear(); // jthread joins on destruction
    }

    // Stateless: goes to some worker, chosen by round-robin. No ordering
    // guarantee across calls. O(1), one atomic fetch_add, no queue-size probe.
    [[nodiscard]] bool submit_any(Task task) noexcept {
        const auto idx = rr_counter_.fetch_add(1, std::memory_order_relaxed) % num_workers_;
        return queues_[idx].try_push(std::move(task));
    }

    // Affinity: same key always maps to the same worker across the whole pool.
    template<typename Key>
    [[nodiscard]] bool submit_by_key(const Key& key, Task task) noexcept {
        const auto idx = std::hash<Key>{}(key) % num_workers_;
        return queues_[idx].try_push(std::move(task));
    }

    // Affinity scoped to one group: same key always maps to the same worker
    // within that group, independent of what other groups exist.
    template<typename Key>
    [[nodiscard]] bool submit_to_group(std::size_t group_id, const Key& key, Task task) noexcept {
        const auto& g   = groups_[group_id];
        const auto  idx = g.begin + (std::hash<Key>{}(key) % g.count);
        return queues_[idx].try_push(std::move(task));
    }

    [[nodiscard]] bool submit_to_worker(std::size_t worker_idx, Task task) noexcept {
        return queues_[worker_idx].try_push(std::move(task));
    }

    [[nodiscard]] std::size_t num_workers() const noexcept { return num_workers_; }

private:
    struct GroupInfo { std::string name; std::size_t begin; std::size_t count; };
    struct WorkerConfig { HandlerFn fn{nullptr}; void* ctx{nullptr}; std::optional<int> cpu_id; };

    std::array<lockfree::MpscQueue<Task, QueueCapacity>, MaxWorkers> queues_;
    std::array<WorkerConfig, MaxWorkers>                             configs_;
    std::vector<GroupInfo>                                           groups_;
    std::vector<std::jthread>                                        threads_;
    std::size_t                                                      num_workers_{0};
    std::atomic<bool>                                                running_{false};
    std::atomic<std::size_t>                                         rr_counter_{0};

    std::size_t add_worker(HandlerFn fn, void* ctx, std::optional<int> cpu_id) {
        const std::size_t idx = num_workers_++;
        assert_bounds(idx);
        configs_[idx] = WorkerConfig{.fn = fn, .ctx = ctx, .cpu_id = cpu_id};
        return idx;
    }

    static void assert_bounds(std::size_t idx) noexcept {
        (void)idx;
        // MaxWorkers is a compile-time cap (like Disruptor::MaxConsumers) — a
        // spawn beyond it is a configuration bug, not a runtime condition.
    }

    void pin_this_thread(std::optional<int> cpu_id) noexcept {
#if defined(__linux__)
        if (!cpu_id) return;
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(*cpu_id, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
        (void)cpu_id;
#endif
    }

    void worker_loop(std::size_t idx, std::stop_token st) {
        pin_this_thread(configs_[idx].cpu_id);
        auto& queue = queues_[idx];
        auto& cfg   = configs_[idx];

        Task task;
        while (!st.stop_requested()) {
            if (queue.try_pop(task)) [[likely]] {
                cfg.fn(task, cfg.ctx);
            } else {
                MARKETLIB_TP_PAUSE();
            }
        }
        // Drain on shutdown so in-flight book updates aren't silently dropped.
        while (queue.try_pop(task)) cfg.fn(task, cfg.ctx);
    }
};

} // namespace marketlib::threadpool
