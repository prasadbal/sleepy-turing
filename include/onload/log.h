#pragma once
#include <logging/logging.h>

namespace marketlib::onload {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.onload");
} // namespace marketlib::onload
