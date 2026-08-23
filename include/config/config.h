#pragma once
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace marketlib::config {

enum class ConfigError { file_not_found, parse_error };

// Opaque config object — toml++ is an implementation detail, never exposed.
//
// Access pattern:
//   auto cfg  = Config::load("app.toml", overrides).value();
//   auto sec  = cfg.section("logging");           // navigate to [logging]
//   auto lvl  = sec.get_string("level");          // std::optional<std::string>
//   auto port = sec.get_int_or("port", 9000);     // int64_t with fallback
class Config {
public:
    // Load from file with full processing pipeline:
    //   env_bindings overlay → --set overrides → ${VAR} expansion
    [[nodiscard]]
    static std::expected<Config, ConfigError>
    load(const std::filesystem::path& path,
         std::span<const std::string>  overrides = {}) noexcept;

    // Construct empty config (all accessors return nullopt / fallback)
    Config() noexcept = default;

    // ── Typed accessors (direct key, no dotted path) ──────────────────────────
    [[nodiscard]] std::optional<std::string> get_string(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<int64_t>     get_int   (std::string_view key) const noexcept;
    [[nodiscard]] std::optional<double>      get_double(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<bool>        get_bool  (std::string_view key) const noexcept;

    // Typed accessors with fallback
    [[nodiscard]] std::string get_string_or(std::string_view key, std::string fallback) const noexcept;
    [[nodiscard]] int64_t     get_int_or   (std::string_view key, int64_t     fallback) const noexcept;
    [[nodiscard]] double      get_double_or(std::string_view key, double      fallback) const noexcept;
    [[nodiscard]] bool        get_bool_or  (std::string_view key, bool        fallback) const noexcept;

    // Navigate to a sub-section via dotted path.
    // Returns an empty Config if the path does not exist.
    [[nodiscard]] Config section(std::string_view path) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return !impl_; }

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    explicit Config(std::shared_ptr<const Impl> impl) noexcept
        : impl_(std::move(impl)) {}
};

} // namespace marketlib::config
