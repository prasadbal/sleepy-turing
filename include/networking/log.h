#pragma once
#include <logging/logging.h>

namespace marketlib::networking {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.networking");
} // namespace marketlib::networking
