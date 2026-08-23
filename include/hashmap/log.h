#pragma once
#include <logging/logging.h>

namespace marketlib::hashmap {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.hashmap");
} // namespace marketlib::hashmap
