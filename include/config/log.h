#pragma once
#include <logging/logging.h>

namespace marketlib::config {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.config");
} // namespace marketlib::config
