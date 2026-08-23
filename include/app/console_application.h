#pragma once
#include <app/application.h>

namespace marketlib::app {

// Application that runs to completion and returns an exit code.
// No signal handling — useful for tools, batch jobs, and test harnesses.
//
// Usage:
//   class MyTool : public ConsoleApplication {
//   public:
//       MyTool() : ConsoleApplication("my-tool", "1.0.0") {}
//   protected:
//       int on_run() noexcept override {
//           /* ... do work ... */
//           return 0;
//       }
//   };
class ConsoleApplication : public Application {
protected:
    using Application::Application;

    // Implement the application body. Return the process exit code (0 = success).
    virtual int on_run() noexcept = 0;

    int do_run() noexcept final { return on_run(); }
};

} // namespace marketlib::app
