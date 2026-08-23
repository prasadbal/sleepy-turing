#pragma once
#include <logging/buffer.h>
#include <perfmeasure/tsc.h>
#include <atomic>
#include <memory>
#include <new>
#include <source_location>
#include <string_view>
#include <tuple>

// fmt is included here only for fmt::format_string (compile-time format checking)
// and fmt::format / fmt::runtime on the drain side.  The include is isolated to
// this header so that consuming modules compiled with -fno-exceptions do not pick
// up fmt's internal throw statements from other fmt headers.
#include <spdlog/fmt/fmt.h>

namespace marketlib::logging {

// Compact sequential thread ID — cheaper than hashing std::thread::id.
inline uint32_t this_thread_id() noexcept {
    static std::atomic<uint32_t> s_next{1};
    thread_local uint32_t s_id = s_next.fetch_add(1, std::memory_order_relaxed);
    return s_id;
}

// ── Logger ────────────────────────────────────────────────────────────────────
// Named node in a dot-separated hierarchy ("marketlib", "marketlib.networking").
// Level inherits upward — if this node's level is Level::inherit, walk to parent.
// Instances live forever in the global registry (Logger::get returns Logger&).
class Logger {
public:
    // Look up or create a logger by name. Thread-safe. Returns a stable reference.
    [[nodiscard]] static Logger& get(std::string_view name) noexcept;

    explicit Logger(std::string_view name, Logger* parent = nullptr) noexcept;

    void set_level(Level l) noexcept {
        level_.store(static_cast<uint8_t>(l), std::memory_order_relaxed);
    }
    [[nodiscard]] Level           level() const noexcept {
        return static_cast<Level>(level_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] bool       should_log(Level l) const noexcept {
        return static_cast<uint8_t>(l) >=
               static_cast<uint8_t>(effective_level());
    }
    [[nodiscard]] uint16_t        id()   const noexcept { return id_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // ── Unified entry point (used by LOG_INFO … LOG_CRIT macros) ─────────────
    // Dispatches to write_fast if all args are trivially copyable and fit in the
    // fixed payload, otherwise falls back to write_debug transparently.
    template<typename... Args>
    void write(Level lvl, std::source_location loc,
               fmt::format_string<Args...> fmt_str, Args&&... args) noexcept {
        using Tuple = std::tuple<std::decay_t<Args>...>;
        if constexpr ((std::is_trivially_copyable_v<std::decay_t<Args>> && ...) &&
                       sizeof(Tuple) <= LogRecord::kPayloadSize)
            write_fast(lvl, loc, fmt_str, std::forward<Args>(args)...);
        else
            write_debug(lvl, loc, fmt_str, std::forward<Args>(args)...);
    }

    // ── Fast path: trivially copyable args, per-thread SPSC queue ─────────────
    // Compile-time check: sizeof(tuple<Args...>) must fit in LogRecord::kPayloadSize.
    // format_fn is marked noexcept: a fmt::format_error would std::terminate,
    // which is acceptable — bad format strings are a programmer error.
    template<typename... Args>
    requires (std::is_trivially_copyable_v<std::decay_t<Args>> && ...)
    void write_fast(Level lvl, std::source_location loc,
                    fmt::format_string<Args...> fmt_str, Args&&... args) noexcept {
        using Tuple = std::tuple<std::decay_t<Args>...>;
        static_assert(sizeof(Tuple) <= LogRecord::kPayloadSize,
            "args exceed 192-byte fast-log payload — use LOG_DEBUG for large/container args");

        LogRecord rec{};
        rec.timestamp_tsc = perfmeasure::rdtsc();
        rec.thread_id     = this_thread_id();
        rec.line          = loc.line();
        rec.file          = loc.file_name();
        rec.function      = loc.function_name();
        rec.fmt_str       = fmt_str.get().data();
        rec.logger_id     = id_;
        rec.level         = static_cast<uint8_t>(lvl);

        // Placement-new tuple directly into fixed payload — zero heap alloc.
        new (rec.payload) Tuple{std::forward<Args>(args)...};

        // Captureless lambda → function pointer.  Tuple type baked in at compile
        // time; one instantiation per unique Args... combo across the whole binary.
        // noexcept: format errors terminate (acceptable — bad fmt str = programmer error).
        rec.format_fn = [](const char* fs, const std::byte* p) noexcept -> std::string {
            const auto& t = *std::launder(reinterpret_cast<const Tuple*>(p));
            return std::apply([fs](const auto&... a) noexcept -> std::string {
                return fmt::format(fmt::runtime(fs), a...);
            }, t);
        };

        LogBuffer::current().queue.try_push(rec);
    }

    // ── Debug path: any formattable type, global MPSC queue, heap allocated ───
    // Supports std::vector, std::string, custom types with fmt::formatter.
    // noexcept: OOM or format errors terminate — debug logging is best-effort.
    template<typename... Args>
    void write_debug(Level lvl, std::source_location loc,
                     fmt::format_string<Args...> fmt_str, Args&&... args) noexcept {
        using Tuple = std::tuple<std::decay_t<Args>...>;

        auto rec           = std::make_unique<DebugRecord>();
        rec->timestamp_tsc = perfmeasure::rdtsc();
        rec->thread_id     = this_thread_id();
        rec->line          = loc.line();
        rec->file          = loc.file_name();
        rec->function      = loc.function_name();
        rec->fmt_str       = fmt_str.get().data();
        rec->logger_id     = id_;
        rec->level         = static_cast<uint8_t>(lvl);

        rec->payload = std::make_unique<std::byte[]>(sizeof(Tuple));
        new (rec->payload.get()) Tuple{std::forward<Args>(args)...};

        rec->format_fn = [](const char* fs, const std::byte* p) noexcept -> std::string {
            const auto& t = *std::launder(reinterpret_cast<const Tuple*>(p));
            return std::apply([fs](const auto&... a) noexcept -> std::string {
                return fmt::format(fmt::runtime(fs), a...);
            }, t);
        };

        // Destructor for non-trivial tuple members (std::string, std::vector, etc.)
        if constexpr (!std::is_trivially_destructible_v<Tuple>) {
            rec->destroy_payload = [](std::byte* p) noexcept {
                std::launder(reinterpret_cast<Tuple*>(p))->~Tuple();
            };
        }

        detail::enqueue_debug(std::move(rec));
    }

private:
    std::string           name_;
    uint16_t              id_;
    std::atomic<uint8_t>  level_{static_cast<uint8_t>(Level::inherit)};
    Logger*               parent_{nullptr};

    [[nodiscard]] Level effective_level() const noexcept;

    static uint16_t alloc_id() noexcept {
        static std::atomic<uint16_t> s_next{0};
        return s_next.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace marketlib::logging
