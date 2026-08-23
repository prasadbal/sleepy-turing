#pragma once
#include <app/module.h>
#include <cmdline/args.h>
#include <config/config.h>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace marketlib::app {

namespace detail {
// Per-type identity without RTTI: each instantiation of type_tag<T> gets its
// own storage, so its address is a unique, stable key for T. Built with
// -fno-rtti project-wide, so typeid/std::type_index are unavailable here.
template<typename T>
inline const char type_tag = 0;

template<typename T>
[[nodiscard]] constexpr const void* type_id() noexcept { return &type_tag<T>; }
} // namespace detail

// Base class for all marketlib applications.
//
// Lifecycle orchestrated by run():
//   parse args → load config → init logging
//     → init modules (in order)
//     → on_init()
//     → start modules (in order)
//     → do_run()          ← ServerApplication / ConsoleApplication
//     → stop  modules (reverse)
//     → on_shutdown()
//     → shutdown modules (reverse)
//     → stop logging
//
// Modules are registered via add() before run() is called.
// Each module receives the config section matching its name().
//
// Usage:
//   class MyApp : public ServerApplication {   // or ConsoleApplication
//   public:
//       MyApp() : ServerApplication("my-app", "1.0.0") {
//           add(std::make_unique<FeedModule>());
//           add(std::make_unique<RiskModule>());
//       }
//   protected:
//       std::expected<void,std::string> on_init() noexcept override { ... }
//       void on_run(std::stop_token st) noexcept override { ... }
//   };
class Application {
public:
    virtual ~Application() = default;

    int run(int argc, char** argv) noexcept;

    // Register a module.
    // Modules are init'd/started in registration order,
    // stopped/shutdown in reverse order.
    // Must be called before run().
    template<typename T>
    void add(std::unique_ptr<T> mod) {
        static_assert(std::is_base_of_v<Module, T>);
        module_index_[detail::type_id<T>()] = mod.get();
        modules_.push_back(std::move(mod));
    }

    // Retrieve a registered module by its concrete type.
    // Returns nullptr if no module of that type was registered.
    template<typename T>
    [[nodiscard]] T* module() noexcept {
        auto it = module_index_.find(detail::type_id<T>());
        return it != module_index_.end() ? static_cast<T*>(it->second) : nullptr;
    }

protected:
    Application(std::string_view name, std::string_view version)
        : name_{name}, version_{version}, cli_{name, version} {}

    // Called after all modules are init'd, before they are started.
    // Override to perform app-level init that depends on modules being ready.
    virtual std::expected<void, std::string> on_init() noexcept { return {}; }

    // The run body — ServerApplication installs signals and calls on_run(stop_token);
    // ConsoleApplication calls on_run() and forwards its exit code.
    // Returns the process exit code.
    virtual int do_run() noexcept = 0;

    // Called after modules are stopped, before they are shutdown.
    virtual void on_shutdown() noexcept {}

    // Register app-specific CLI options — call from the derived class's
    // constructor, before run() parses argv. See cmdline::Options.
    [[nodiscard]] cmdline::Options& cmdline() noexcept { return cli_; }

    [[nodiscard]] const config::Config& config()  const noexcept { return config_; }
    [[nodiscard]] const cmdline::Args&  args()    const noexcept { return cli_.args(); }
    [[nodiscard]] std::string_view      name()    const noexcept { return name_; }
    [[nodiscard]] std::string_view      version() const noexcept { return version_; }

private:
    std::string      name_;
    std::string      version_;
    config::Config    config_;
    cmdline::Options  cli_;

    std::vector<std::unique_ptr<Module>>        modules_;
    std::unordered_map<const void*, Module*>    module_index_;
};

} // namespace marketlib::app
