#include <config/config.h>
#include <toml++/toml.h>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace marketlib::config {

// ── Impl ──────────────────────────────────────────────────────────────────────

struct Config::Impl {
    toml::table table;
};

// ── file-local helpers ────────────────────────────────────────────────────────

static const toml::table* walk_path(const toml::table& root,
                                    std::string_view    path) noexcept {
    const toml::table* cur = &root;
    std::string_view   rem = path;
    while (!rem.empty()) {
        const auto dot = rem.find('.');
        const auto key = rem.substr(0, dot);
        const auto* node = cur->get(key);
        if (!node || !node->is_table()) return nullptr;
        cur = node->as_table();
        rem = (dot == std::string_view::npos) ? "" : rem.substr(dot + 1);
    }
    return cur;
}

static bool set_by_path(toml::table& root,
                        std::string_view path,
                        std::string_view value) noexcept {
    toml::table*     cur = &root;
    std::string_view rem = path;
    while (true) {
        const auto dot = rem.find('.');
        if (dot == std::string_view::npos) {
            if (!cur->get(rem)) return false;
            cur->insert_or_assign(rem, std::string(value));
            return true;
        }
        const auto seg  = rem.substr(0, dot);
        auto*      node = cur->get(seg);
        if (!node || !node->is_table()) return false;
        cur = node->as_table();
        rem = rem.substr(dot + 1);
    }
}

static std::string expand_vars(std::string_view s) noexcept {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            const auto end = s.find('}', i + 2);
            if (end != std::string_view::npos) {
                const std::string var{s.substr(i + 2, end - i - 2)};
                if (const char* v = std::getenv(var.c_str()))
                    out += v;
                else
                    out.append(s.data() + i, end - i + 1);
                i = end + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

static void expand_vars_table(toml::table& t) noexcept {
    std::vector<std::pair<std::string, std::string>> mods;
    for (auto& [k, v] : t) {
        if (v.is_string()) {
            auto expanded = expand_vars(*v.value<std::string>());
            if (expanded != *v.value<std::string>())
                mods.emplace_back(std::string(k), std::move(expanded));
        } else if (v.is_table()) {
            expand_vars_table(*v.as_table());
        }
    }
    for (auto& [k, val] : mods)
        t.insert_or_assign(k, std::move(val));
}

static void apply_env_bindings(toml::table& root) noexcept {
    const auto* bindings = root["env_bindings"].as_table();
    if (!bindings) return;
    for (const auto& [env_var, cfg_path_node] : *bindings) {
        const auto* path_val = cfg_path_node.as<std::string>();
        if (!path_val) continue;
        if (const char* env_val = std::getenv(std::string(env_var).c_str()))
            set_by_path(root, **path_val, env_val);
    }
    root.erase("env_bindings");
}

static void apply_overrides(toml::table& root,
                             std::span<const std::string> overrides) noexcept {
    for (const auto& kv : overrides) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        set_by_path(root, std::string_view{kv}.substr(0, eq),
                          std::string_view{kv}.substr(eq + 1));
    }
}

// ── Config::load ──────────────────────────────────────────────────────────────

std::expected<Config, ConfigError>
Config::load(const std::filesystem::path& path,
             std::span<const std::string>  overrides) noexcept {
    if (!std::filesystem::exists(path))
        return std::unexpected(ConfigError::file_not_found);

    toml::table tbl;
    try {
        tbl = toml::parse_file(path.string());
    } catch (...) {
        return std::unexpected(ConfigError::parse_error);
    }

    apply_env_bindings(tbl);
    apply_overrides(tbl, overrides);
    expand_vars_table(tbl);

    auto impl = std::make_shared<Impl>();
    impl->table = std::move(tbl);
    return Config{std::move(impl)};
}

// ── typed accessors ───────────────────────────────────────────────────────────

std::optional<std::string> Config::get_string(std::string_view key) const noexcept {
    if (!impl_) return std::nullopt;
    if (auto v = impl_->table[key].value<std::string>()) return *v;
    return std::nullopt;
}

std::optional<int64_t> Config::get_int(std::string_view key) const noexcept {
    if (!impl_) return std::nullopt;
    if (auto v = impl_->table[key].value<int64_t>()) return *v;
    return std::nullopt;
}

std::optional<double> Config::get_double(std::string_view key) const noexcept {
    if (!impl_) return std::nullopt;
    if (auto v = impl_->table[key].value<double>()) return *v;
    return std::nullopt;
}

std::optional<bool> Config::get_bool(std::string_view key) const noexcept {
    if (!impl_) return std::nullopt;
    if (auto v = impl_->table[key].value<bool>()) return *v;
    return std::nullopt;
}

std::string Config::get_string_or(std::string_view key, std::string fallback) const noexcept {
    return get_string(key).value_or(std::move(fallback));
}

int64_t Config::get_int_or(std::string_view key, int64_t fallback) const noexcept {
    return get_int(key).value_or(fallback);
}

double Config::get_double_or(std::string_view key, double fallback) const noexcept {
    return get_double(key).value_or(fallback);
}

bool Config::get_bool_or(std::string_view key, bool fallback) const noexcept {
    return get_bool(key).value_or(fallback);
}

// ── section ───────────────────────────────────────────────────────────────────

Config Config::section(std::string_view path) const noexcept {
    if (!impl_) return {};
    const toml::table* sub = walk_path(impl_->table, path);
    if (!sub) return {};

    auto impl = std::make_shared<Impl>();
    impl->table = *sub;   // copy sub-table — Config is a startup-only value
    return Config{std::move(impl)};
}

} // namespace marketlib::config
