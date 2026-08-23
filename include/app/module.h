#pragma once
#include <config/config.h>
#include <expected>
#include <string>
#include <string_view>

namespace marketlib::app {

// Base class for all application modules/subsystems.
//
// Lifecycle (orchestrated by Application::run):
//   init(cfg) → start() → [running] → stop() → shutdown()
//
// name() must match the TOML section key — the module receives only its own
// config slice:  config.section(module.name())
//
// Each module should declare its own logger:
//   logging::Logger& log_ = logging::Logger::get("marketlib.<name>");
class Module {
public:
    virtual ~Module() = default;

    // Section key in the config file and logger sub-name.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Receives only the config section for this module.
    // Called before start() — allocate resources, validate config.
    virtual std::expected<void, std::string> init(const config::Config&) noexcept
        { return {}; }

    // Begin processing — open sockets, start threads, etc.
    virtual std::expected<void, std::string> start() noexcept { return {}; }

    // Signal stop — set flags, close inputs, wake blocked threads.
    // Must not block.
    virtual void stop() noexcept {}

    // Wait for full stop and release resources.
    // Called after stop() — may block until threads have joined.
    virtual void shutdown() noexcept {}
};

} // namespace marketlib::app
