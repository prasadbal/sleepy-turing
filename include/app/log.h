#pragma once
#include <logging/logging.h>

namespace marketlib::app {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.app");
} // namespace marketlib::app
