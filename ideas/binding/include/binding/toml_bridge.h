#pragma once
#include <toml++/toml.h>

#include "binding/config_field.h"

// ============================================================================
// The one place toml++ is allowed to be named: converts a parsed
// toml::table into the parser-independent binding::FieldList (see
// config_field.h). Everything past this header deals in Field/FieldList
// only -- the same rule ptree_bridge.h follows for XML.
//
// Unlike ptree_bridge.h, this preserves TOML's native typing: an integer,
// float, or bool value becomes that LeafValue alternative directly, never
// stringified-then-reparsed. Only a genuine TOML string becomes the text
// alternative -- config_bind.h's parse_leaf_value() takes whichever one
// is actually there.
// ============================================================================

namespace binding {

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

} // namespace binding
