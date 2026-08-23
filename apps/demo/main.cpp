#include <app/server_application.h>
#include <app/module.h>
#include <app/log.h>
#include <config/config.h>
#include <logging/macros.h>
#include <lockfree/spsc_queue.h>
#include <perfmeasure/tsc.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace marketlib;

// ── TickModule ─────────────────────────────────────────────────────────────────
// Reads config from [demo] section, simulates a market data tick generator.
// In a real system this would open a feed socket and push ticks to the book.
class TickModule : public app::Module {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "demo"; }

    std::expected<void, std::string> init(const config::Config& cfg) noexcept override {
        tick_interval_ms_ = static_cast<uint32_t>(cfg.get_int_or("tick_interval_ms", 500));
        symbol_           = cfg.get_string_or("symbol", "ESZ4");

        LOG_INFO(log, "[TickModule] symbol={} tick_interval_ms={}", symbol_, tick_interval_ms_);
        return {};
    }

    std::expected<void, std::string> start() noexcept override {
        // SpscQueue smoke test — normally you'd start a feed thread here
        struct Tick { uint64_t tsc; double price; uint32_t qty; };
        lockfree::SpscQueue<Tick, 64> q;

        for (int i = 0; i < 8; ++i)
            q.try_push(Tick{perfmeasure::rdtsc(), 5432.25 + i * 0.25, 100u + static_cast<uint32_t>(i)});

        Tick tk{};
        int  popped = 0;
        while (q.try_pop(tk)) ++popped;

        LOG_INFO(log, "[TickModule] SpscQueue smoke: pushed 8, popped {}", popped);
        return {};
    }

    void stop()     noexcept override { LOG_INFO(log, "[TickModule] stop"); }
    void shutdown() noexcept override { LOG_INFO(log, "[TickModule] shutdown"); }

    [[nodiscard]] std::string_view symbol()           const noexcept { return symbol_; }
    [[nodiscard]] uint32_t         tick_interval_ms() const noexcept { return tick_interval_ms_; }

private:
    logging::Logger& log = logging::Logger::get("marketlib.demo.tick");

    std::string symbol_{"ESZ4"};
    uint32_t    tick_interval_ms_{500};
};

// ── DemoApp ───────────────────────────────────────────────────────────────────
class DemoApp : public app::ServerApplication {
public:
    DemoApp() : ServerApplication("marketlib-demo", "0.1.0") {
        add(std::make_unique<TickModule>());
    }

protected:
    std::expected<void, std::string> on_init() noexcept override {
        // Modules are already init'd — safe to access their state
        auto* tick = module<TickModule>();
        LOG_INFO(app::log, "on_init: using symbol '{}' from TickModule", tick->symbol());

        // TSC calibration report
        const auto clk = perfmeasure::TscClock::calibrate();
        LOG_INFO(app::log, "TSC: {:.3f} ns/tick", clk.ns_per_tick());
        return {};
    }

    void on_run(std::stop_token st) noexcept override {
        auto* tick = module<TickModule>();
        const auto interval = std::chrono::milliseconds(tick->tick_interval_ms());

        uint64_t n = 0;
        while (!st.stop_requested()) {
            ++n;
            // Fast path: const char* and numeric args — all trivially copyable
            LOG_INFO(app::log, "tick={} symbol={} price={:.2f} qty={}",
                     n, tick->symbol().data(), 5432.25 + (n % 10) * 0.25, 100u);

            if (n % 10 == 0)
                LOG_WARN(app::log, "heartbeat tick={}", n);

            std::this_thread::sleep_for(interval);
        }
        LOG_INFO(app::log, "stop requested after {} ticks", n);
    }

    void on_shutdown() noexcept override {
        LOG_INFO(app::log, "on_shutdown");
    }
};

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    return DemoApp{}.run(argc, argv);
}
