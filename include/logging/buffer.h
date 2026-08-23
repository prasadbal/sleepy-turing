#pragma once
#include <logging/record.h>
#include <lockfree/spsc_queue.h>

namespace marketlib::logging {

// Per-thread ring buffer of fast-path log records.
// Capacity: 1024 × 256 bytes = 256 KB per thread.
class LogBuffer {
public:
    static constexpr std::size_t kCapacity = 1024;

    lockfree::SpscQueue<LogRecord, kCapacity> queue;

    // Returns this thread's buffer, registering it with the drain on first call.
    static LogBuffer& current() noexcept;
};

namespace detail {
// Called once per thread by LogBuffer::current() on first access.
void register_buffer(LogBuffer*) noexcept;

// Called by Logger::write_debug() — pushes to the global MPSC debug queue.
void enqueue_debug(std::unique_ptr<DebugRecord>) noexcept;
} // namespace detail

} // namespace marketlib::logging
