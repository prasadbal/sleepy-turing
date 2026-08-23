#pragma once
#include <logging/logging.h>

namespace marketlib::serialization {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.serialization");
} // namespace marketlib::serialization
