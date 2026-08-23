#pragma once
#include <logging/config.h>

namespace marketlib::logging {

struct Drain {
    // Calibrate TSC, set up spdlog sinks, start drain thread.
    // Must be called once before any logging. Not thread-safe.
    static void start(const Config& cfg = {}) noexcept;

    // Drain all queues, flush sinks, join drain thread.
    static void stop() noexcept;

    // Flush sinks without stopping the drain thread.
    static void flush() noexcept;
};

} // namespace marketlib::logging
