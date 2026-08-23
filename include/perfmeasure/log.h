#pragma once
#include <logging/logging.h>

namespace marketlib::perfmeasure {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.perfmeasure");
} // namespace marketlib::perfmeasure
