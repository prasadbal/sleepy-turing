#include <app/server_application.h>

namespace marketlib::app::signals {
void install(std::stop_source& src) noexcept;
void uninstall() noexcept;
} // namespace marketlib::app::signals

namespace marketlib::app {

int ServerApplication::do_run() noexcept {
    std::stop_source stop_src;
    signals::install(stop_src);
    on_run(stop_src.get_token());
    signals::uninstall();
    return 0;
}

} // namespace marketlib::app
