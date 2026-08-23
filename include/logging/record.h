#pragma once
#include <logging/level.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace marketlib::logging {

// Drain calls format_fn(fmt_str, payload) → formatted message string.
// One instantiation per unique Args... combo, generated at the call site.
using FormatFn  = std::string(*)(const char* fmt_str, const std::byte* payload) noexcept;
using DestroyFn = void(*)(std::byte*) noexcept;

// ── Fast-path record (256 bytes, fixed, trivially copyable) ───────────────────
// Lives in a per-thread SpscQueue<LogRecord, N>. No heap allocation.
// Args must be trivially copyable — they are stored in a std::tuple in payload.
struct alignas(64) LogRecord {
    uint64_t     timestamp_tsc;  // raw TSC ticks — drain converts to wall ns
    uint32_t     thread_id;
    uint32_t     line;
    const char*  file;           // string literal pointer
    const char*  function;       // string literal pointer
    const char*  fmt_str;        // string literal pointer
    FormatFn     format_fn;      // compile-time generated per Args... combo
    uint16_t     logger_id;
    uint8_t      level;
    uint8_t      _pad[13];

    static constexpr std::size_t kPayloadSize = 192;
    alignas(8) std::byte payload[kPayloadSize];
};
static_assert(sizeof(LogRecord) == 256);
static_assert(std::is_trivially_destructible_v<LogRecord>);

// ── Debug-path record (heap, unlimited size) ──────────────────────────────────
// Lives in a global MPSC ConcurrentQueue<unique_ptr<DebugRecord>>.
// Supports any fmt-formattable type including containers.
struct DebugRecord {
    uint64_t     timestamp_tsc;
    uint32_t     thread_id;
    uint32_t     line;
    const char*  file;
    const char*  function;
    const char*  fmt_str;
    FormatFn     format_fn;
    DestroyFn    destroy_payload; // calls ~Tuple() on non-trivial args
    uint16_t     logger_id;
    uint8_t      level;
    std::unique_ptr<std::byte[]> payload; // sizeof(Tuple<Args...>) bytes
};

} // namespace marketlib::logging
