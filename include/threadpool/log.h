#pragma once
#include <logging/logging.h>

namespace marketlib::threadpool {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.threadpool");
} // namespace marketlib::threadpool
