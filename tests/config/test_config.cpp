#include <catch2/catch_test_macros.hpp>
#include <config/config.h>
#include <filesystem>
#include <fstream>

using namespace marketlib::config;

static std::filesystem::path write_toml(const char* content) {
    auto p = std::filesystem::temp_directory_path() / "sleepy_test.toml";
    std::ofstream{p} << content;
    return p;
}

TEST_CASE("load: file not found", "[config]") {
    auto r = Config::load("/nonexistent/sleepy.toml");
    REQUIRE(!r);
    REQUIRE(r.error() == ConfigError::file_not_found);
}

TEST_CASE("load: invalid TOML returns parse_error", "[config]") {
    auto r = Config::load(write_toml("not = valid toml [[["));
    REQUIRE(!r);
    REQUIRE(r.error() == ConfigError::parse_error);
}

TEST_CASE("section: returns empty Config for missing path", "[config]") {
    auto r = Config::load(write_toml("[logging]\nlevel = \"info\"\n"));
    REQUIRE(r.has_value());
    REQUIRE(r->section("missing").empty());
}

TEST_CASE("get_string: reads value from section", "[config]") {
    auto r = Config::load(write_toml("[logging]\nlevel = \"warn\"\n"));
    REQUIRE(r.has_value());
    auto sec = r->section("logging");
    REQUIRE(!sec.empty());
    REQUIRE(sec.get_string("level") == "warn");
}

TEST_CASE("get_int_or: returns fallback when key absent", "[config]") {
    auto r = Config::load(write_toml("[logging]\n"));
    REQUIRE(r.has_value());
    REQUIRE(r->section("logging").get_int_or("port", 9000) == 9000);
}

TEST_CASE("load: --set override updates existing key", "[config]") {
    auto p = write_toml("[logging]\nlevel = \"info\"\n");
    std::vector<std::string> overrides{"logging.level=debug"};
    auto r = Config::load(p, overrides);
    REQUIRE(r.has_value());
    REQUIRE(r->section("logging").get_string("level") == "debug");
}

TEST_CASE("load: --set silently ignores unknown key", "[config]") {
    auto p = write_toml("[logging]\nlevel = \"info\"\n");
    std::vector<std::string> overrides{"logging.typo=value"};
    auto r = Config::load(p, overrides);
    REQUIRE(r.has_value());
    REQUIRE(r->section("logging").get_string("typo") == std::nullopt);
}

TEST_CASE("load: env_bindings section removed from result", "[config]") {
    auto p = write_toml(
        "[env_bindings]\n\"SOME_VAR\" = \"logging.level\"\n"
        "[logging]\nlevel = \"info\"\n");
    auto r = Config::load(p);
    REQUIRE(r.has_value());
    REQUIRE(r->section("env_bindings").empty());
}

TEST_CASE("load: env_bindings overlays matching env var", "[config]") {
    auto p = write_toml(
        "[env_bindings]\n\"SLEEPY_TEST_LEVEL\" = \"logging.level\"\n"
        "[logging]\nlevel = \"info\"\n");
    ::setenv("SLEEPY_TEST_LEVEL", "warn", 1);
    auto r = Config::load(p);
    ::unsetenv("SLEEPY_TEST_LEVEL");
    REQUIRE(r.has_value());
    REQUIRE(r->section("logging").get_string("level") == "warn");
}

TEST_CASE("load: ${VAR} expanded in string values", "[config]") {
    auto p = write_toml("[logging]\nfile = \"${SLEEPY_TEST_DIR}/app.log\"\n");
    ::setenv("SLEEPY_TEST_DIR", "/var/log/sleepy", 1);
    auto r = Config::load(p);
    ::unsetenv("SLEEPY_TEST_DIR");
    REQUIRE(r.has_value());
    REQUIRE(r->section("logging").get_string("file") == "/var/log/sleepy/app.log");
}
