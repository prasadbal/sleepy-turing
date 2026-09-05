#pragma once
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "binding/config_bind.h"
#include "binding/config_field.h"
#include "binding/config_parser.h"

// ============================================================================
// The config-reading interface: one get(), reached by path, for either a
// single scalar or a whole struct.
//
// Everything is path -> node -> value, and the first step belongs to the
// parser (config_parser.h) -- this layer only decides what to do with the
// node it gets back, which depends entirely on T:
//
//   - a scalar target -> parse the node's own leaf value.
//   - a struct target -> bind the node's children.
//
// Required vs absent is decided by T too, never by an extra argument: a
// plain T throws if it isn't there, std::optional<T> yields nullopt -- for
// a scalar and a struct alike. There's deliberately no defaulting overload:
// std::optional's own value_or() already is one, and composes without
// adding a second way to spell the same thing.
//
//   cfg.get<int>("port")                          // throws if absent
//   cfg.get<std::optional<int>>("port")           // nullopt if absent
//   cfg.get<std::optional<int>>("port").value_or(9000)
//   cfg.get<AppConfig>()                          // whole document
//   cfg.get<Pool>("pool")                         // one section
//   cfg.get<std::optional<Pool>>("pool")          // nullopt if absent
//   cfg.get("pool", existing_pool)                // deduced, filled in place
//
// Note value_or() defaults only an *absent* value: one that is present but
// malformed ("port = abc") still throws, rather than silently reading as
// 9000.
// ============================================================================

namespace binding {

// Everything get() can produce. is_bindable_leaf already covers a leaf and
// an optional-wrapped leaf (see reflect.h); the third clause is what adds
// an optional-wrapped *struct*, so an absent section can be asked for the
// same way an absent scalar is.
template <typename T>
concept gettable = is_bindable_leaf<T> || config_schema<T> ||
                   (is_optional_v<T> && config_schema<optional_value_t<T>>);

class Configuration {
public:
    // `strict`, when true, additionally rejects any config key that doesn't
    // match one of the target struct's own fields, at every nesting level.
    // It's a property of the document, set once here rather than repeated
    // on every call (see bind_from_fields' own comment for why it's a
    // runtime value and not a compile-time flag).
    static Configuration load(const std::filesystem::path& file, bool strict = false) {
        return Configuration(ConfigParser::load(file), strict);
    }

    template <gettable T>
    T get(std::string_view path = "") const {
        T out{};
        get(path, out);
        return out;
    }

    // Same semantics, deduced T, filled in place -- for reading straight
    // into something you already have, without naming the type twice.
    template <gettable T>
    void get(std::string_view path, T& out) const {
        const ConfigNode* node = parser_.resolve(path);

        if constexpr (is_optional_v<T> && config_schema<optional_value_t<T>>) {
            if (!node) {
                out = std::nullopt;
                return;
            }
            optional_value_t<T> value{};
            bind_struct(*node, value, path);
            out = std::move(value);

        } else if constexpr (config_schema<T>) {
            if (!node) throw missing(path);
            bind_struct(*node, out, path);

        } else { // a leaf, or an optional-wrapped leaf
            std::optional<LeafValue> value = node ? parser_.leaf(*node) : std::nullopt;
            if (!value) {
                if constexpr (is_optional_v<T>) {
                    // Absent, or present but a subtree rather than a scalar
                    // -- an optional read treats both as "no value here".
                    out = std::nullopt;
                    return;
                } else if (!node) {
                    throw missing(path);
                } else {
                    throw std::runtime_error("binding: field '" + std::string(path) +
                                             "' expected a plain value");
                }
            }
            // Parsing still throws on malformed input even for an optional
            // T: absence is forgiven, bad data is not.
            using Value = std::conditional_t<is_optional_v<T>, optional_value_t<T>, T>;
            Value parsed{};
            detail::parse_leaf_value(std::move(*value), parsed, path);
            out = std::move(parsed);
        }
    }

private:
    Configuration(ConfigParser parser, bool strict) noexcept
        : parser_(std::move(parser)), strict_(strict) {}

    template <config_schema T>
    void bind_struct(const ConfigNode& node, T& out, std::string_view path) const {
        // fields() hands back a FieldList by value, built for this call --
        // so it's always a temporary and always takes bind_from_fields'
        // moving overload: leaf values move into the target struct rather
        // than being copied out of something about to be discarded.
        FieldList fields = parser_.fields(node);
        if (fields.empty() && parser_.leaf(node)) {
            throw std::runtime_error("binding: field '" + std::string(path) +
                                     "' expected a nested structure");
        }
        bind_from_fields(std::move(fields), out, strict_);
    }

    static std::runtime_error missing(std::string_view path) {
        return std::runtime_error("binding: missing field (from path '" + std::string(path) + "')");
    }

    ConfigParser parser_;
    bool strict_ = false;
};

// FieldList and Field satisfy neither is_bindable_leaf nor config_schema,
// so no instantiation of get() can hand one back out -- asserted directly
// rather than left implied, so a future widening of either concept fails
// here instead of silently reopening what this facade exists to hide.
static_assert(!gettable<FieldList>);
static_assert(!gettable<Field>);

} // namespace binding
