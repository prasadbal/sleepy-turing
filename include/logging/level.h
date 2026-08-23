#pragma once
#include <cstdint>
#include <string_view>

namespace marketlib::logging {

enum class Level : uint8_t {
    trace = 0,
    debug,
    info,
    warn,
    error,
    crit,
    off,
    inherit = 0xFF   // walk up to parent — never stored externally
};

constexpr std::string_view level_name(Level l) noexcept {
    switch (l) {
        case Level::trace:   return "trace";
        case Level::debug:   return "debug";
        case Level::info:    return "info";
        case Level::warn:    return "warn";
        case Level::error:   return "error";
        case Level::crit:    return "critical";
        case Level::off:     return "off";
        default:             return "unknown";
    }
}

constexpr Level level_from_str(std::string_view s) noexcept {
    if (s == "trace")                    return Level::trace;
    if (s == "debug")                    return Level::debug;
    if (s == "info")                     return Level::info;
    if (s == "warn" || s == "warning")   return Level::warn;
    if (s == "error" || s == "err")      return Level::error;
    if (s == "critical" || s == "crit")  return Level::crit;
    if (s == "off")                      return Level::off;
    return Level::info;
}

} // namespace marketlib::logging
