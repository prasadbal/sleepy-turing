#include <catch2/catch_test_macros.hpp>
#include <cmdline/args.h>
#include <array>

using namespace marketlib::cmdline;

// Build a mutable argv-style array from string literals
template<std::size_t N>
static std::array<char*, N> make_argv(std::array<const char*, N> src) {
    std::array<char*, N> out{};
    for (std::size_t i = 0; i < N; ++i)
        out[i] = const_cast<char*>(src[i]);
    return out;
}

TEST_CASE("parse: no arguments — continue running, missing default config skipped", "[cmdline]") {
    auto av = make_argv<1>({"app"});
    Options opts("no-such-app-xyz", "0.1.0");
    REQUIRE(opts.parse(1, av.data()) == -1);
    // Default is "config/no-such-app-xyz.toml", which doesn't exist — skipped, not an error.
    REQUIRE(opts.args().config_file.empty());
    REQUIRE(opts.args().overrides.empty());
}

TEST_CASE("parse: --config sets config_file even if the file doesn't exist", "[cmdline]") {
    auto av = make_argv<3>({"app", "--config", "/etc/sleepy.toml"});
    Options opts("app", "0.1.0");
    REQUIRE(opts.parse(3, av.data()) == -1);
    REQUIRE(opts.args().config_file == "/etc/sleepy.toml");
}

TEST_CASE("parse: --set repeated flags populate overrides", "[cmdline]") {
    auto av = make_argv<5>({"app", "--set", "logging.level=debug", "--set", "feed.port=9001"});
    Options opts("app", "0.1.0");
    REQUIRE(opts.parse(5, av.data()) == -1);
    REQUIRE(opts.args().overrides.size() == 2);
    REQUIRE(opts.args().overrides[0] == "logging.level=debug");
    REQUIRE(opts.args().overrides[1] == "feed.port=9001");
}

TEST_CASE("parse: --set single flag multiple values", "[cmdline]") {
    auto av = make_argv<4>({"app", "--set", "logging.level=debug", "feed.port=9001"});
    Options opts("app", "0.1.0");
    REQUIRE(opts.parse(4, av.data()) == -1);
    REQUIRE(opts.args().overrides.size() == 2);
    REQUIRE(opts.args().overrides[0] == "logging.level=debug");
    REQUIRE(opts.args().overrides[1] == "feed.port=9001");
}

TEST_CASE("parse: --help exits with code 0", "[cmdline]") {
    auto av = make_argv<2>({"app", "--help"});
    Options opts("app", "0.1.0");
    REQUIRE(opts.parse(2, av.data()) == 0);
}

TEST_CASE("parse: --version exits with code 0", "[cmdline]") {
    auto av = make_argv<2>({"app", "--version"});
    Options opts("app", "0.1.0");
    REQUIRE(opts.parse(2, av.data()) == 0);
}

TEST_CASE("parse: unknown flag exits with non-zero code", "[cmdline]") {
    auto av = make_argv<2>({"app", "--unknown-flag"});
    Options opts("app", "0.1.0");
    REQUIRE(opts.parse(2, av.data()) > 0);
}

TEST_CASE("addOption: binds a custom typed option before parse", "[cmdline]") {
    auto av = make_argv<3>({"app", "--symbol", "ESZ4"});
    Options opts("app", "0.1.0");
    std::string symbol;
    opts.addOption("--symbol", symbol, "Symbol to trade");
    REQUIRE(opts.parse(3, av.data()) == -1);
    REQUIRE(symbol == "ESZ4");
}

TEST_CASE("addOption/addFlag: chain and coexist with base options", "[cmdline]") {
    auto av = make_argv<6>({"app", "--symbol", "ESZ4", "--dry-run", "--set", "feed.port=9001"});
    Options opts("app", "0.1.0");
    std::string symbol;
    bool        dry_run = false;
    opts.addOption("--symbol", symbol, "Symbol to trade")
        .addFlag("--dry-run", dry_run, "Don't send live orders");
    REQUIRE(opts.parse(6, av.data()) == -1);
    REQUIRE(symbol == "ESZ4");
    REQUIRE(dry_run == true);
    REQUIRE(opts.args().overrides.size() == 1);
    REQUIRE(opts.args().overrides[0] == "feed.port=9001");
}
