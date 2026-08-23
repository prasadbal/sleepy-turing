#pragma once
#include <app/application.h>
#include <stop_token>

namespace marketlib::app {

// Application that runs until SIGINT/SIGTERM (or stop_source::request_stop()).
//
// Installs signal handlers in do_run(), delivers a stop_token to on_run(),
// then tears down handlers cleanly before returning.
//
// Usage:
//   class MyServer : public ServerApplication {
//   public:
//       MyServer() : ServerApplication("my-server", "1.0.0") {}
//   protected:
//       void on_run(std::stop_token st) noexcept override {
//           while (!st.stop_requested()) { /* ... */ }
//       }
//   };
class ServerApplication : public Application {
protected:
    using Application::Application;

    // Implement the server loop. Returns when stop is requested.
    // The stop_token fires on SIGINT, SIGTERM, or a manual request_stop() call.
    virtual void on_run(std::stop_token st) noexcept = 0;

    // Installs OS signal handlers, calls on_run(stop_token), then uninstalls.
    int do_run() noexcept final;
};

} // namespace marketlib::app
