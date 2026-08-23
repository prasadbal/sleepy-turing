#pragma once
#include <logging/logger.h>
#include <source_location>

// LOG_TRACE / LOG_DEBUG  → debug path always (heap, global MPSC, any formattable type)
// LOG_INFO  … LOG_CRIT   → write() dispatches at compile time:
//                            trivially copyable args + fits payload → fast path (SPSC, zero alloc)
//                            otherwise                              → debug path (heap, MPSC)
//
// source_location::current() is captured here at the call site — NOT inside
// write/write_fast/write_debug — so file/line always point to the actual log call.

#define LOG_TRACE(logger, fmt, ...)                                              \
    do {                                                                         \
        if ((logger).should_log(::marketlib::logging::Level::trace))             \
            (logger).write_debug(::marketlib::logging::Level::trace,             \
                                  std::source_location::current(),               \
                                  fmt, ##__VA_ARGS__);                           \
    } while(0)

#define LOG_DEBUG(logger, fmt, ...)                                              \
    do {                                                                         \
        if ((logger).should_log(::marketlib::logging::Level::debug))             \
            (logger).write_debug(::marketlib::logging::Level::debug,             \
                                  std::source_location::current(),               \
                                  fmt, ##__VA_ARGS__);                           \
    } while(0)

#define LOG_INFO(logger, fmt, ...)                                               \
    do {                                                                         \
        if ((logger).should_log(::marketlib::logging::Level::info))              \
            (logger).write(::marketlib::logging::Level::info,                    \
                            std::source_location::current(),                     \
                            fmt, ##__VA_ARGS__);                                 \
    } while(0)

#define LOG_WARN(logger, fmt, ...)                                               \
    do {                                                                         \
        if ((logger).should_log(::marketlib::logging::Level::warn))              \
            (logger).write(::marketlib::logging::Level::warn,                    \
                            std::source_location::current(),                     \
                            fmt, ##__VA_ARGS__);                                 \
    } while(0)

#define LOG_ERROR(logger, fmt, ...)                                              \
    do {                                                                         \
        if ((logger).should_log(::marketlib::logging::Level::error))             \
            (logger).write(::marketlib::logging::Level::error,                   \
                            std::source_location::current(),                     \
                            fmt, ##__VA_ARGS__);                                 \
    } while(0)

#define LOG_CRIT(logger, fmt, ...)                                               \
    do {                                                                         \
        if ((logger).should_log(::marketlib::logging::Level::crit))              \
            (logger).write(::marketlib::logging::Level::crit,                    \
                            std::source_location::current(),                     \
                            fmt, ##__VA_ARGS__);                                 \
    } while(0)
