#pragma once
#include <stdexcept>
#include <string>
#include <toml++/toml.h>

#include "binding/config_bind.h"
#include "binding/config_field.h"

// ============================================================================
// The one place toml++ is allowed to be named: converts a parsed
// toml::table into the parser-independent binding::FieldList (see
// config_field.h). Everything past this header deals in Field/FieldList
// only -- the same rule ptree_bridge.h follows for XML -- with the same
// one exception ptree_bridge.h has: get_leaf/try_get_leaf below read a
// single ad hoc value straight off a live toml::table without
// materializing a whole FieldList first. Unlike ptree, toml++ has no
// built-in dotted-path lookup (no get_child_optional equivalent), so the
// "walk the path down to one node" step here is a small manual per-segment
// walk -- the one piece of this file that can't lean on the library the
// way ptree_bridge.h's version does. Both still finish through the exact
// same detail::parse_leaf_value() from config_bind.h.
//
// Unlike ptree_bridge.h, this preserves TOML's native typing: an integer,
// float, or bool value becomes that LeafValue alternative directly, never
// stringified-then-reparsed. Only a genuine TOML string becomes the text
// alternative -- config_bind.h's parse_leaf_value() takes whichever one
// is actually there.
// ============================================================================

namespace binding {

// Visits one level of `t` as the config sees it, calling
// fn(name, const toml::node&) for each entry -- the single definition of
// "what fields exist here" on the TOML side, which both from_toml()
// (converting a subtree) and find_toml_child() (walking a path) are built
// on, so a path can't resolve differently than binding sees the same
// document. The ptree bridge has the same split for the same reason.
//
// An array yields one entry per element, all under the same name: that is
// how repetition is represented (see config_field.h), and it makes a TOML
// array of tables ([[replicas]]) and a repeated XML element arrive in the
// identical shape.
template <typename F>
void for_each_toml_child(const toml::table& t, F&& fn) {
    for (const auto& [key, node] : t) {
        const std::string name(key.str());
        if (node.is_array()) {
            for (const auto& elem : *node.as_array()) {
                fn(name, elem);
            }
        } else {
            fn(name, node);
        }
    }
}

// The child named `name` at this level, or nullptr. Case-insensitive, and
// sharing for_each_toml_child's rules. A repeated name (an array) yields
// the first element, which is all a path can mean.
inline const toml::node* find_toml_child(const toml::table& t, std::string_view name) {
    const toml::node* found = nullptr;
    for_each_toml_child(t, [&](const std::string& key, const toml::node& node) {
        if (!found && iequals(key, name)) found = &node;
    });
    return found;
}

// Converts one already-resolved toml::node into a LeafValue, or nullopt if
// it isn't a scalar this library can represent (a table, or a date/time --
// see the note at the bottom of from_toml).
inline std::optional<LeafValue> toml_leaf(const toml::node& node) {
    if (node.is_string()) return LeafValue(std::string(**node.as_string()));
    if (node.is_integer()) return LeafValue(**node.as_integer());
    if (node.is_floating_point()) return LeafValue(**node.as_floating_point());
    if (node.is_boolean()) return LeafValue(**node.as_boolean());
    return std::nullopt;
}

inline FieldList from_toml(const toml::table& t) {
    FieldList fields;

    for (const auto& [key, node] : t) {
        std::string name(key.str());

        if (node.is_table()) {
            fields.push_back(Field{name, from_toml(*node.as_table())});
        } else if (node.is_string()) {
            fields.push_back(Field{name, LeafValue(std::string(**node.as_string()))});
        } else if (node.is_integer()) {
            fields.push_back(Field{name, LeafValue(**node.as_integer())});
        } else if (node.is_floating_point()) {
            fields.push_back(Field{name, LeafValue(**node.as_floating_point())});
        } else if (node.is_boolean()) {
            fields.push_back(Field{name, LeafValue(**node.as_boolean())});
        } else if (node.is_array()) {
            // A TOML array -- most usefully an array of tables (the TOML
            // idiom for "a list of things", e.g. [[replicas]] blocks) --
            // becomes several Field entries sharing `name`, exactly how a
            // repeated XML element already works (see config_field.h):
            // there's no separate "this is an array" Field variant, just
            // multiple same-named entries for bind_from_fields' vector<T>
            // handling to collect. An array of scalars (e.g. ports =
            // [8080, 8443]) becomes repeated leaf entries the same way,
            // matching the vector<leaf> support in config_bind.h.
            for (const auto& elem : *node.as_array()) {
                if (elem.is_table()) {
                    fields.push_back(Field{name, from_toml(*elem.as_table())});
                } else if (elem.is_string()) {
                    fields.push_back(Field{name, LeafValue(std::string(**elem.as_string()))});
                } else if (elem.is_integer()) {
                    fields.push_back(Field{name, LeafValue(**elem.as_integer())});
                } else if (elem.is_floating_point()) {
                    fields.push_back(Field{name, LeafValue(**elem.as_floating_point())});
                } else if (elem.is_boolean()) {
                    fields.push_back(Field{name, LeafValue(**elem.as_boolean())});
                }
                // Nested arrays and date/time array elements aren't
                // produced by any config shape this bridge has been
                // exercised against yet -- silently skipped rather than
                // guessed at.
            }
        }
        // date/time/date_time nodes: TOML has no equivalent in
        // is_bindable_leaf today (see reflect.h) -- there's no target
        // type to convert one into yet, so these are silently skipped
        // rather than guessed at. Add an OciDate-style wrapper and a
        // LeafValue alternative for it if a real config needs one.
    }

    return fields;
}

namespace detail {

// Walks `path`'s dot-separated segments through nested tables using
// toml++'s own per-key table::get(), one segment at a time -- there's no
// single library call to delegate the whole walk to the way ptree's
// get_child_optional() lets ptree_bridge.h do it, so this is the one
// place that has to do it by hand. Returns nullptr if any segment is
// missing, or a non-terminal segment isn't itself a table to descend into.
inline const toml::node* resolve_toml_path(const toml::table& root, std::string_view path) {
    const toml::table* current = &root;
    std::string_view remaining = path;
    for (;;) {
        const auto dot = remaining.find('.');
        const std::string_view segment = (dot == std::string_view::npos) ? remaining : remaining.substr(0, dot);
        const toml::node* found = current->get(segment);
        if (!found) return nullptr;
        if (dot == std::string_view::npos) return found;
        if (!found->is_table()) return nullptr;
        current = found->as_table();
        remaining = remaining.substr(dot + 1);
    }
}

// Converts one already-resolved toml::node into a LeafValue -- the
// single-node equivalent of from_toml()'s scalar branches above, reused
// here so get_leaf/try_get_leaf preserve the same native typing from_toml
// does (no stringify-then-reparse for an int/float/bool). Throws, naming
// `field_name`, for a table/array/date-time node -- the same set of
// LeafValue-incompatible shapes from_toml() otherwise just skips when
// building a whole FieldList, but which a single-value lookup can't
// silently ignore.
inline LeafValue leaf_value_from_toml_node(const toml::node& node, std::string_view field_name) {
    if (node.is_string()) return LeafValue(std::string(**node.as_string()));
    if (node.is_integer()) return LeafValue(**node.as_integer());
    if (node.is_floating_point()) return LeafValue(**node.as_floating_point());
    if (node.is_boolean()) return LeafValue(**node.as_boolean());
    throw std::runtime_error("binding: field '" + std::string(field_name) + "' expected a plain value");
}

} // namespace detail

// Reads a single scalar value at a dot-separated path straight off a live
// toml::table, parsed as T -- path -> node -> LeafValue -> parse_leaf_value(),
// the same last step get_leaf() (config_bind.h) uses once it has a
// FieldList's Field in hand, and the same shape as ptree_bridge.h's
// get_leaf() overload. Never materializes a FieldList for any part of the
// document outside `path`.
//
// Throws std::runtime_error, naming the path, if the path doesn't
// resolve, the target node isn't a scalar (a table or array), or its
// value doesn't convert to T -- same failure modes as the FieldList-based
// get_leaf(), for the same reason.
template <is_bindable_leaf T>
T get_leaf(const toml::table& root, std::string_view path) {
    const toml::node* found = detail::resolve_toml_path(root, path);
    if (!found) {
        throw std::runtime_error("binding: missing field (from path '" + std::string(path) + "')");
    }
    T value{};
    detail::parse_leaf_value(detail::leaf_value_from_toml_node(*found, path), value, path);
    return value;
}

// Same as get_leaf above, but returns std::nullopt instead of throwing
// when `path` itself doesn't resolve -- the toml-source equivalent of
// config_bind.h's FieldList-based try_get_leaf(). A value that IS present
// but fails to convert to T (including a table/array at that path) still
// throws: a real data error, not absence.
template <is_bindable_leaf T>
std::optional<T> try_get_leaf(const toml::table& root, std::string_view path) {
    const toml::node* found = detail::resolve_toml_path(root, path);
    if (!found) return std::nullopt;
    T value{};
    detail::parse_leaf_value(detail::leaf_value_from_toml_node(*found, path), value, path);
    return value;
}

} // namespace binding
