#include <cmdline/args.h>
#include <iostream>

namespace marketlib::cmdline {

Options::Options(std::string_view app_name, std::string_view app_version) noexcept
    : app_{std::string(app_name)} {
    app_.set_version_flag("--version,-V", std::string(app_version));

    args_.config_file = "config/" + std::string(app_name) + ".toml";
    config_opt_ = app_.add_option("--config,-c", args_.config_file,
                                   "Path to TOML config file")
                      ->capture_default_str();

    app_.add_option("--set,-s", args_.overrides,
                     "Override config key: --set logging.level=debug feed.port=9001")
        ->expected(-1)
        ->allow_extra_args(true);
}

int Options::parse(int argc, char** argv) noexcept {
    try {
        app_.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        std::cout << app_.help() << std::flush;
        return 0;
    } catch (const CLI::CallForVersion&) {
        return 0;
    } catch (const CLI::ParseError& e) {
        std::cerr << "Error: " << e.what() << "\n\n" << app_.help() << std::flush;
        return e.get_exit_code();
    }

    // Left at the "config/<app_name>.toml" default (not passed explicitly)
    // and that file doesn't exist — treat as "no config" rather than an error.
    if (config_opt_->count() == 0 && !std::filesystem::exists(args_.config_file))
        args_.config_file.clear();

    return -1;
}

} // namespace marketlib::cmdline
