#pragma once
#include <logging/logging.h>

namespace marketlib::memory {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.memory");
} // namespace marketlib::memory
