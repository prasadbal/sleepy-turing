#pragma once
#include <logging/level.h>
#include <config/config.h>
#include <filesystem>
#include <map>
#include <string>

namespace marketlib::logging {

struct Config {
    std::filesystem::path          log_dir   {};
    std::filesystem::path          log_file  {"marketlib.log"};
    std::size_t                    max_size  {64 * 1024 * 1024};
    std::size_t                    max_files {5};
    Level                          root_level{Level::info};
    std::map<std::string, Level>   module_levels;  // "marketlib.networking" → debug
    bool                           console   {true};
    std::string                    pattern   {"%Y-%m-%dT%T.%e [%^%-5l%$] [%t] %v"};
    std::size_t                    queue_depth{1024};
};

// Populate from the [logging] section of an already-processed config::Config.
// [logging.levels] sub-section sets per-module overrides.
// Missing fields retain defaults.
[[nodiscard]] Config from_config(const config::Config& root) noexcept;

} // namespace marketlib::logging
