#include <logging/logger.h>
#include <lockfree/mpsc_queue.h>
#include <mutex>
#include <unordered_map>

namespace marketlib::logging {

// ── global registry ───────────────────────────────────────────────────────────
namespace {
    std::unordered_map<std::string, std::unique_ptr<Logger>> g_registry;
    std::mutex                                                g_registry_mu;
    // 128 slots: one per thread — registration happens once at thread startup.
    lockfree::MpscQueue<LogBuffer*, 128>                     g_register_queue;
    // 4096 slots: debug/trace records are off in production; bounded is fine.
    lockfree::MpscQueue<std::unique_ptr<DebugRecord>, 4096>  g_debug_queue;
}

// ── detail:: — called from header-only hot path ───────────────────────────────
namespace detail {

void register_buffer(LogBuffer* buf) noexcept {
    g_register_queue.try_push(buf);
}

void enqueue_debug(std::unique_ptr<DebugRecord> rec) noexcept {
    g_debug_queue.try_push(std::move(rec));
}

} // namespace detail

// Drain accesses these queues directly — exposed via these declarations.
lockfree::MpscQueue<LogBuffer*, 128>&                    registration_queue() noexcept {
    return g_register_queue;
}
lockfree::MpscQueue<std::unique_ptr<DebugRecord>, 4096>& debug_queue() noexcept {
    return g_debug_queue;
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
        std::lock_guard lock{g_registry_mu};
        if (auto it = g_registry.find(std::string{name}); it != g_registry.end())
            return *it->second;
    }

    // Ensure parent exists (recursive, lock-free for this step).
    Logger* parent = nullptr;
    const auto dot = name.rfind('.');
    if (dot != std::string_view::npos)
        parent = &get(name.substr(0, dot));

    std::lock_guard lock{g_registry_mu};
    // Double-check after acquiring lock.
    const std::string key{name};
    if (auto it = g_registry.find(key); it != g_registry.end())
        return *it->second;

    auto logger     = std::make_unique<Logger>(name, parent);
    auto& ref       = *logger;
    g_registry.emplace(key, std::move(logger));
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
