#pragma once
#include <logging/logging.h>

namespace marketlib::lockfree {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.lockfree");
} // namespace marketlib::lockfree
