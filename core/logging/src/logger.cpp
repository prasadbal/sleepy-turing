#include <logging/logger.h>
#include <lockfree/mpsc_queue.h>
#include <mutex>
#include <unordered_map>

namespace marketlib::logging {

// ── global registry ───────────────────────────────────────────────────────────
// Function-local statics ("Meyer's singleton"), not namespace-scope globals:
// every <module>/log.h declares an inline `Logger& log = Logger::get(...)`
// that runs as dynamic initialization in whatever TU includes it. Dynamic
// init order across translation units is unspecified, so a namespace-scope
// global here could easily still be default-constructed (e.g. an
// unordered_map with zero buckets) when some other TU's log.h initializer
// calls Logger::get() first. A function-local static is guaranteed
// (thread-safely, since C++11) to construct on first call regardless of
// init order — sidesteps the race entirely.
namespace {
    std::unordered_map<std::string, std::unique_ptr<Logger>>& registry() noexcept {
        static std::unordered_map<std::string, std::unique_ptr<Logger>> instance;
        return instance;
    }
    std::mutex& registry_mutex() noexcept {
        static std::mutex instance;
        return instance;
    }
    // 128 slots: one per thread — registration happens once at thread startup.
    lockfree::MpscQueue<LogBuffer*, 128>& register_queue() noexcept {
        static lockfree::MpscQueue<LogBuffer*, 128> instance;
        return instance;
    }
    // 4096 slots: debug/trace records are off in production; bounded is fine.
    lockfree::MpscQueue<std::unique_ptr<DebugRecord>, 4096>& debug_record_queue() noexcept {
        static lockfree::MpscQueue<std::unique_ptr<DebugRecord>, 4096> instance;
        return instance;
    }
}

// ── detail:: — called from header-only hot path ───────────────────────────────
namespace detail {

void register_buffer(LogBuffer* buf) noexcept {
    register_queue().try_push(buf);
}

void enqueue_debug(std::unique_ptr<DebugRecord> rec) noexcept {
    debug_record_queue().try_push(std::move(rec));
}

} // namespace detail

// Drain accesses these queues directly — exposed via these declarations.
lockfree::MpscQueue<LogBuffer*, 128>&                    registration_queue() noexcept {
    return register_queue();
}
lockfree::MpscQueue<std::unique_ptr<DebugRecord>, 4096>& debug_queue() noexcept {
    return debug_record_queue();
}

// ── LogBuffer::current ────────────────────────────────────────────────────────
LogBuffer& LogBuffer::current() noexcept {
    thread_local LogBuffer  t_buf;
    thread_local bool       t_registered =
        (detail::register_buffer(&t_buf), true);
    (void)t_registered;
    return t_buf;
}

// ── Logger::get ───────────────────────────────────────────────────────────────
Logger& Logger::get(std::string_view name) noexcept {
    {
        std::lock_guard lock{registry_mutex()};
        if (auto it = registry().find(std::string{name}); it != registry().end())
            return *it->second;
    }

    // Ensure parent exists (recursive, lock-free for this step).
    Logger* parent = nullptr;
    const auto dot = name.rfind('.');
    if (dot != std::string_view::npos)
        parent = &get(name.substr(0, dot));

    std::lock_guard lock{registry_mutex()};
    // Double-check after acquiring lock.
    const std::string key{name};
    if (auto it = registry().find(key); it != registry().end())
        return *it->second;

    auto logger     = std::make_unique<Logger>(name, parent);
    auto& ref       = *logger;
    registry().emplace(key, std::move(logger));
    return ref;
}

// ── Logger ────────────────────────────────────────────────────────────────────
Logger::Logger(std::string_view name, Logger* parent) noexcept
    : name_{name}
    , id_{alloc_id()}
    , parent_{parent}
{
    // Root logger "marketlib" defaults to info; all others inherit.
    if (parent_ == nullptr)
        level_.store(static_cast<uint8_t>(Level::info), std::memory_order_relaxed);
}

Level Logger::effective_level() const noexcept {
    const Logger* cur = this;
    while (cur) {
        const auto raw = cur->level_.load(std::memory_order_relaxed);
        if (raw != static_cast<uint8_t>(Level::inherit))
            return static_cast<Level>(raw);
        cur = cur->parent_;
    }
    return Level::info;
}

} // namespace marketlib::logging
