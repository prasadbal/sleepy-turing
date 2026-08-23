#pragma once
#include <logging/logging.h>

namespace marketlib::testing {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.testing");
} // namespace marketlib::testing
