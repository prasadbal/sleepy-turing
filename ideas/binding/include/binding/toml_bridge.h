#pragma once
#include <stdexcept>
#include <string>
#include <toml++/toml.h>

#include "binding/config_bind.h"
#include "binding/config_field.h"

// ============================================================================
// Where toml++ is named: converts a parsed toml::table into the
// parser-independent binding::FieldList (see config_field.h), and answers
// what a level contains so a path can be walked over the live document.
// Everything past this header deals in Field/FieldList only -- the same
// rule ptree_bridge.h follows for XML.
//
// Two things are exported, and they are deliberately built on one
// definition of "what fields exist at this level" (for_each_toml_child):
// from_toml() materializes a subtree for binding, and find_toml_child()
// matches one name for ConfigParser::resolve()'s path walk. Sharing that
// visitor is what stops a path from resolving differently than binding
// reads the same document.
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

// This header used to also carry resolve_toml_path and get_leaf/
// try_get_leaf, a third way to read one value by path. They are gone:
// reading by path belongs to ConfigParser::resolve() (config_parser.h),
// which walks a level at a time through find_toml_child() above -- the
// same for_each_toml_child() rules from_toml() applies, so a path resolves
// to exactly what binding sees. Four implementations of "resolve a path"
// was three too many for rules this fiddly, and they had already drifted.

} // namespace binding
