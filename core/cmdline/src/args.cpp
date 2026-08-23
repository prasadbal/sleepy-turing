#include <cmdline/args.h>
#include <CLI/CLI.hpp>

namespace marketlib::cmdline {

ParseResult parse(int argc, char** argv,
                  std::string_view app_name,
                  std::string_view app_version) noexcept {
    CLI::App app{std::string(app_name)};
    app.set_version_flag("--version,-V", std::string(app_version));

    Args args;
    app.add_option("--config,-c", args.config_file,
                   "Path to TOML config file");
    app.add_option("--set,-s", args.overrides,
                   "Override config key: --set logging.level=debug feed.port=9001")
       ->expected(-1)
       ->allow_extra_args(true);

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        std::cout << app.help() << std::flush;
        return {args, 0};
    } catch (const CLI::CallForVersion&) {
        return {args, 0};
    } catch (const CLI::ParseError& e) {
        std::cerr << "Error: " << e.what() << "\n\n" << app.help() << std::flush;
        return {args, e.get_exit_code()};
    }

    return {args, -1};
}

} // namespace marketlib::cmdline
