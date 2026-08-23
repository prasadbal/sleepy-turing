#include <logging/drain.h>
#include <logging/logger.h>
#include <logging/buffer.h>
#include <perfmeasure/tsc.h>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <lockfree/mpsc_queue.h>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

namespace marketlib::logging {

// Queues are defined in logger.cpp — accessed here via these declarations.
lockfree::MpscQueue<LogBuffer*, 128>&                    registration_queue() noexcept;
lockfree::MpscQueue<std::unique_ptr<DebugRecord>, 4096>& debug_queue()        noexcept;

// ── drain state ───────────────────────────────────────────────────────────────
namespace {
    std::atomic<bool>        g_running{false};
    std::thread              g_thread;
    double                   g_ns_per_tick{1.0};
    uint64_t                 g_tsc_epoch{0};
    std::chrono::nanoseconds g_wall_epoch{0};

    // Recalibrate every 30 seconds so NTP slew and accumulated error don't
    // cause log timestamps to drift far from real wall time.
    constexpr uint64_t kRecalibIntervalNs = 30ULL * 1'000'000'000ULL;
    uint64_t           g_next_recalib_tsc{0};
}

// ── helpers ───────────────────────────────────────────────────────────────────
static void recalibrate() noexcept {
    // Snapshot TSC and wall clock as close together as possible.
    const uint64_t tsc  = perfmeasure::rdtsc();
    const auto     wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    g_tsc_epoch  = tsc;
    g_wall_epoch = wall;
    // Schedule next recalibration using current ns_per_tick estimate.
    g_next_recalib_tsc = tsc +
        static_cast<uint64_t>(kRecalibIntervalNs / g_ns_per_tick);
}

static uint64_t tsc_to_ns(uint64_t tsc) noexcept {
    const int64_t delta = static_cast<int64_t>(tsc - g_tsc_epoch);
    return static_cast<uint64_t>(
        g_wall_epoch.count() + static_cast<int64_t>(delta * g_ns_per_tick));
}

static spdlog::level::level_enum to_spdlog(uint8_t lvl) noexcept {
    // Level enum values match spdlog's ordering (trace=0 … critical=5, off=6)
    return static_cast<spdlog::level::level_enum>(
        std::min(lvl, static_cast<uint8_t>(spdlog::level::off)));
}

static void emit(uint8_t level, uint64_t ts_ns,
                 const char* file, uint32_t line, const char* fn,
                 std::string_view msg) noexcept {
    auto* logger = spdlog::default_logger_raw();
    if (!logger) return;
    spdlog::source_loc loc{file, static_cast<int>(line), fn};
    logger->log(loc, to_spdlog(level), msg);
}

static void format_and_emit(FormatFn format_fn, const char* fmt_str,
                             const std::byte* payload,
                             uint8_t level, uint64_t tsc,
                             const char* file, uint32_t line,
                             const char* fn) noexcept {
    // format_fn is noexcept — fmt::format_error terminates (programmer error).
    const auto msg = format_fn(fmt_str, payload);
    emit(level, tsc_to_ns(tsc), file, line, fn, msg);
}

// ── drain loop ────────────────────────────────────────────────────────────────
static void drain_loop() noexcept {
    std::vector<LogBuffer*> buffers;
    buffers.reserve(64);

    while (g_running.load(std::memory_order_relaxed)) {
        bool did_work = false;

        // Periodic TSC↔wall recalibration — keeps timestamps accurate despite
        // NTP slew.  Drain is single-threaded so no locking needed.
        if (perfmeasure::rdtsc() >= g_next_recalib_tsc) [[unlikely]]
            recalibrate();

        // Pick up newly registered thread buffers (lock-free).
        {
            LogBuffer* buf{};
            while (registration_queue().try_pop(buf))
                buffers.push_back(buf);
        }

        // Drain fast-path (per-thread SPSC) queues.
        for (auto* buf : buffers) {
            LogRecord rec;
            while (buf->queue.try_pop(rec)) {
                format_and_emit(rec.format_fn, rec.fmt_str, rec.payload,
                                rec.level, rec.timestamp_tsc,
                                rec.file, rec.line, rec.function);
                did_work = true;
            }
        }

        // Drain debug-path (global MPSC) queue.
        {
            std::unique_ptr<DebugRecord> rec;
            while (debug_queue().try_pop(rec)) {
                format_and_emit(rec->format_fn, rec->fmt_str, rec->payload.get(),
                                rec->level, rec->timestamp_tsc,
                                rec->file, rec->line, rec->function);
                if (rec->destroy_payload)
                    rec->destroy_payload(rec->payload.get());
                did_work = true;
            }
        }

        if (!did_work)
            std::this_thread::yield();
    }

    // Final drain after stop signal — flush everything remaining.
    LogBuffer* buf{};
    while (registration_queue().try_pop(buf))
        buffers.push_back(buf);

    for (auto* b : buffers) {
        LogRecord rec;
        while (b->queue.try_pop(rec))
            format_and_emit(rec.format_fn, rec.fmt_str, rec.payload,
                            rec.level, rec.timestamp_tsc,
                            rec.file, rec.line, rec.function);
    }
    {
        std::unique_ptr<DebugRecord> rec;
        while (debug_queue().try_pop(rec)) {
            format_and_emit(rec->format_fn, rec->fmt_str, rec->payload.get(),
                            rec->level, rec->timestamp_tsc,
                            rec->file, rec->line, rec->function);
            if (rec->destroy_payload)
                rec->destroy_payload(rec->payload.get());
        }
    }

    if (auto l = spdlog::default_logger()) l->flush();
}

// ── public API ────────────────────────────────────────────────────────────────
void Drain::start(const Config& cfg) noexcept {
    // Calibrate TSC frequency, then snapshot the epoch pair.
    const auto clk = perfmeasure::TscClock::calibrate();
    g_ns_per_tick  = clk.ns_per_tick();
    recalibrate();   // sets g_tsc_epoch, g_wall_epoch, g_next_recalib_tsc

    // Apply per-module log levels from config.
    Logger::get("marketlib").set_level(cfg.root_level);
    for (const auto& [name, lvl] : cfg.module_levels)
        Logger::get(name).set_level(lvl);

    // Set up spdlog sinks.
    spdlog::init_thread_pool(8192, 1);
    const std::filesystem::path log_path =
        cfg.log_dir.empty() ? cfg.log_file : cfg.log_dir / cfg.log_file;
    if (!log_path.parent_path().empty())
        std::filesystem::create_directories(log_path.parent_path());

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_path.string(), cfg.max_size, cfg.max_files));
    if (cfg.console)
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    auto logger = std::make_shared<spdlog::async_logger>(
        "marketlib", sinks.begin(), sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(spdlog::level::trace); // filtering done by our Logger hierarchy
    logger->set_pattern(cfg.pattern);
    spdlog::set_default_logger(logger);

    g_running = true;
    g_thread  = std::thread(drain_loop);
}

void Drain::stop() noexcept {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
    spdlog::shutdown();
}

void Drain::flush() noexcept {
    if (auto l = spdlog::default_logger()) l->flush();
}

// ── config ────────────────────────────────────────────────────────────────────
Config from_config(const config::Config& root) noexcept {
    Config cfg;
    const auto sec = root.section("logging");
    if (sec.empty()) return cfg;

    if (auto v = sec.get_string("dir"))       cfg.log_dir    = *v;
    if (auto v = sec.get_string("file"))      cfg.log_file   = *v;
    if (auto v = sec.get_int   ("max_size"))  cfg.max_size   = static_cast<std::size_t>(*v);
    if (auto v = sec.get_int   ("max_files")) cfg.max_files  = static_cast<std::size_t>(*v);
    if (auto v = sec.get_bool  ("console"))   cfg.console    = *v;
    if (auto v = sec.get_string("level"))     cfg.root_level = level_from_str(*v);
    if (auto v = sec.get_string("fmt"))       cfg.pattern    = *v;

    // [logging.levels] — per-module overrides
    const auto levels = sec.section("levels");
    // iterate via get on known keys is not possible without toml — levels section
    // is intentionally not iterable through the Config wrapper;
    // applications set per-module levels programmatically after from_config().
    (void)levels;

    return cfg;
}

} // namespace marketlib::logging
