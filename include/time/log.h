#pragma once
#include <logging/logging.h>

namespace marketlib::time {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.time");
} // namespace marketlib::time
