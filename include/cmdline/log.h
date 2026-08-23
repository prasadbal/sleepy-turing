#pragma once
#include <logging/logging.h>

namespace marketlib::cmdline {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.cmdline");
} // namespace marketlib::cmdline
