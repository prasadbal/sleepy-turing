#pragma once
#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace marketlib::cmdline {

struct Args {
    std::filesystem::path    config_file;
    std::vector<std::string> overrides;   // raw "key=value" strings from --set
};

// Owns the CLI11 parser for one application.
//
// Base options are registered at construction:
//   --version,-V   prints app_version and exits
//   --help         prints usage and exits (CLI11 default)
//   --set,-s       repeatable "key=value" config overrides
//   --config,-c    path to a TOML config file — defaults to
//                  "config/<app_name>.toml"; if left at that default and
//                  the file doesn't exist, args().config_file comes back
//                  empty (no config is not an error). An explicitly-passed
//                  --config that doesn't exist is still a hard error,
//                  reported by the config loader.
//
// Applications add their own options — any time before parse() runs, e.g.
// from their constructor — via the typed, chainable addOption()/addFlag():
//
//   cmdline().addOption("--symbol", symbol_, "Symbol to trade")
//             .addFlag("--dry-run", dry_run_, "Don't send live orders");
class Options {
public:
    Options(std::string_view app_name, std::string_view app_version) noexcept;

    // Binds `target` directly (fully typed on T — whatever CLI11 itself
    // supports: string, int, double, filesystem::path, vector<T>, ...).
    // `names` follows CLI11 syntax, e.g. "--symbol,-s".
    template<typename T>
    Options& addOption(std::string_view names, T& target,
                        std::string_view description = "") {
        app_.add_option(std::string(names), target, std::string(description));
        return *this;
    }

    Options& addFlag(std::string_view names, bool& target,
                      std::string_view description = "") {
        app_.add_flag(std::string(names), target, std::string(description));
        return *this;
    }

    // Parses argv against every option registered so far (base + custom).
    // Returns exit_code >= 0 if the process should exit (--help / --version
    // / parse error). Returns -1 if the program should continue running —
    // args() is then valid.
    [[nodiscard]] int parse(int argc, char** argv) noexcept;

    [[nodiscard]] const Args& args() const noexcept { return args_; }

private:
    CLI::App     app_;
    CLI::Option* config_opt_{nullptr};
    Args         args_;
};

} // namespace marketlib::cmdline
