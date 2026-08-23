#include <app/application.h>
#include <app/log.h>
#include <logging/drain.h>
#include <logging/macros.h>
#include <iostream>

namespace marketlib::app {

// ── Lifecycle ─────────────────────────────────────────────────────────────────

int Application::run(int argc, char** argv) noexcept {
    // 1. Parse command line
    auto pr = cmdline::parse(argc, argv, name_, version_);
    if (pr.exit_code >= 0)
        return pr.exit_code;
    args_ = std::move(pr.args);

    // 2. Load and process config
    if (!args_.config_file.empty()) {
        auto cfg = config::Config::load(args_.config_file, args_.overrides);
        if (!cfg) {
            const bool missing = (cfg.error() == config::ConfigError::file_not_found);
            std::cerr << "Config " << (missing ? "not found" : "parse error")
                      << ": " << args_.config_file << '\n';
            return 1;
        }
        config_ = std::move(*cfg);
    }

    // 3. Init logging from [logging] section (defaults apply if section absent)
    logging::Drain::start(logging::from_config(config_));
    LOG_INFO(log, "{} {} starting up", name_, version_);

    // 4. Init modules in registration order.
    //    Each module receives only its own config section (keyed by module name).
    for (auto& mod : modules_) {
        if (auto r = mod->init(config_.section(mod->name())); !r) {
            LOG_CRIT(log, "Module '{}' init failed: {}", mod->name(), r.error());
            logging::Drain::stop();
            return 1;
        }
        LOG_DEBUG(log, "Module '{}' init OK", mod->name());
    }

    // 5. Application-specific init (may depend on modules being init'd)
    if (auto r = on_init(); !r) {
        LOG_CRIT(log, "on_init failed: {}", r.error());
        logging::Drain::stop();
        return 1;
    }

    // 6. Start modules in registration order.
    //    If one fails, stop the already-started ones in reverse before exiting.
    std::size_t started = 0;
    for (auto& mod : modules_) {
        if (auto r = mod->start(); !r) {
            LOG_CRIT(log, "Module '{}' start failed: {}", mod->name(), r.error());
            // Stop the modules that did start, in reverse
            for (auto it = modules_.rend() - static_cast<std::ptrdiff_t>(started);
                 it != modules_.rend(); ++it)
                (*it)->stop();
            for (auto it = modules_.rend() - static_cast<std::ptrdiff_t>(started);
                 it != modules_.rend(); ++it)
                (*it)->shutdown();
            logging::Drain::stop();
            return 1;
        }
        LOG_DEBUG(log, "Module '{}' started", mod->name());
        ++started;
    }

    // 7. Run — implemented by ServerApplication or ConsoleApplication
    LOG_INFO(log, "all modules started — entering run");
    const int exit_code = do_run();

    // 8. Stop modules in reverse registration order (signal but don't block)
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it)
        (*it)->stop();

    // 9. Application-level shutdown hook (before modules fully drain)
    on_shutdown();

    // 10. Shutdown modules in reverse order (may block until threads join, etc.)
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it)
        (*it)->shutdown();

    LOG_INFO(log, "{} shut down cleanly", name_);
    logging::Drain::stop();
    return exit_code;
}

} // namespace marketlib::app
