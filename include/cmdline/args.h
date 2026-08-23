#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace marketlib::cmdline {

struct Args {
    std::filesystem::path    config_file;
    std::vector<std::string> overrides;   // raw "key=value" strings from --set
};

// Returns exit_code >= 0 if the process should exit (--help / --version / parse error).
// Returns -1 if the program should continue running.
struct ParseResult {
    Args args;
    int  exit_code{-1};
};

[[nodiscard]]
ParseResult parse(int argc, char** argv,
                  std::string_view app_name,
                  std::string_view app_version) noexcept;

} // namespace marketlib::cmdline
