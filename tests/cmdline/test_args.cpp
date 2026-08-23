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

TEST_CASE("parse: no arguments — continue running, empty config", "[cmdline]") {
    auto av = make_argv<1>({"app"});
    auto r  = parse(1, av.data(), "app", "0.1.0");
    REQUIRE(r.exit_code == -1);
    REQUIRE(r.args.config_file.empty());
    REQUIRE(r.args.overrides.empty());
}

TEST_CASE("parse: --config sets config_file", "[cmdline]") {
    auto av = make_argv<3>({"app", "--config", "/etc/sleepy.toml"});
    auto r  = parse(3, av.data(), "app", "0.1.0");
    REQUIRE(r.exit_code == -1);
    REQUIRE(r.args.config_file == "/etc/sleepy.toml");
}

TEST_CASE("parse: --set repeated flags populate overrides", "[cmdline]") {
    auto av = make_argv<5>({"app", "--set", "logging.level=debug", "--set", "feed.port=9001"});
    auto r  = parse(5, av.data(), "app", "0.1.0");
    REQUIRE(r.exit_code == -1);
    REQUIRE(r.args.overrides.size() == 2);
    REQUIRE(r.args.overrides[0] == "logging.level=debug");
    REQUIRE(r.args.overrides[1] == "feed.port=9001");
}

TEST_CASE("parse: --set single flag multiple values", "[cmdline]") {
    auto av = make_argv<4>({"app", "--set", "logging.level=debug", "feed.port=9001"});
    auto r  = parse(4, av.data(), "app", "0.1.0");
    REQUIRE(r.exit_code == -1);
    REQUIRE(r.args.overrides.size() == 2);
    REQUIRE(r.args.overrides[0] == "logging.level=debug");
    REQUIRE(r.args.overrides[1] == "feed.port=9001");
}

TEST_CASE("parse: --help exits with code 0", "[cmdline]") {
    auto av = make_argv<2>({"app", "--help"});
    auto r  = parse(2, av.data(), "app", "0.1.0");
    REQUIRE(r.exit_code == 0);
}

TEST_CASE("parse: --version exits with code 0", "[cmdline]") {
    auto av = make_argv<2>({"app", "--version"});
    auto r  = parse(2, av.data(), "app", "0.1.0");
    REQUIRE(r.exit_code == 0);
}

TEST_CASE("parse: unknown flag exits with non-zero code", "[cmdline]") {
    auto av = make_argv<2>({"app", "--unknown-flag"});
    auto r  = parse(2, av.data(), "app", "0.1.0");
    REQUIRE(r.exit_code > 0);
}
