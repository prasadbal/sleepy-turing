#define BACKWARD_HAS_DW 1
#include <backward.hpp>

#include <app/log.h>
#include <logging/drain.h>
#include <logging/macros.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stop_token>
#include <thread>
#include <unistd.h>

namespace marketlib::app::signals {

static std::stop_source*  g_stop{nullptr};
static int                g_pipe[2]{-1, -1};
static std::thread        g_crash_thread;
static std::atomic<bool>  g_running{false};

static void graceful_handler(int sig) noexcept {
    if (g_stop) g_stop->request_stop();
    const char s = static_cast<char>(sig);
    (void)::write(g_pipe[1], &s, 1);
}

static void crash_handler(int sig) noexcept {
    const char s = static_cast<char>(sig);
    (void)::write(g_pipe[1], &s, 1);
    ::pause();
}

static void crash_thread_fn() noexcept {
    char sig_byte{};
    while (::read(g_pipe[0], &sig_byte, 1) == 1) {
        const int sig = static_cast<unsigned char>(sig_byte);

        if (sig == SIGINT || sig == SIGTERM) {
            if (!g_running.load()) break;
            continue;
        }

        backward::StackTrace st;
        st.load_here(64);
        backward::Printer p;
        p.color_mode = backward::ColorMode::always;
        p.print(st, stderr);

        LOG_CRIT(log, "Crash on signal {} — stack trace written to stderr", sig);
        logging::Drain::flush();

        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        ::sigaction(sig, &sa, nullptr);
        ::raise(sig);
        break;
    }
}

void install(std::stop_source& src) noexcept {
    g_stop    = &src;
    g_running = true;

    if (::pipe(g_pipe) != 0) return;
    g_crash_thread = std::thread(crash_thread_fn);

    struct sigaction sa{};
    ::sigemptyset(&sa.sa_mask);

    sa.sa_handler = graceful_handler;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    sa.sa_handler = crash_handler;
    sa.sa_flags   = SA_RESETHAND;
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGBUS,  &sa, nullptr);
    ::sigaction(SIGFPE,  &sa, nullptr);
    ::sigaction(SIGILL,  &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
}

void uninstall() noexcept {
    g_running = false;

    if (g_pipe[1] >= 0) {
        const char sentinel = static_cast<char>(SIGTERM);
        (void)::write(g_pipe[1], &sentinel, 1);
    }
    if (g_crash_thread.joinable()) g_crash_thread.join();

    if (g_pipe[0] >= 0) { ::close(g_pipe[0]); g_pipe[0] = -1; }
    if (g_pipe[1] >= 0) { ::close(g_pipe[1]); g_pipe[1] = -1; }

    g_stop = nullptr;
}

} // namespace marketlib::app::signals
