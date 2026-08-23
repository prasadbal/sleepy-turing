#pragma once
#include <logging/logging.h>

namespace marketlib::orderbook {
    inline ::marketlib::logging::Logger& log =
        ::marketlib::logging::Logger::get("marketlib.orderbook");
} // namespace marketlib::orderbook
