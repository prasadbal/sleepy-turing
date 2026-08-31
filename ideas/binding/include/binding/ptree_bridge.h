#pragma once
#include <boost/property_tree/ptree.hpp>

#include "binding/field_tree.h"

// ============================================================================
// The one place boost::property_tree is allowed to be named: converts a
// parsed ptree (from read_xml, but nothing here is XML-specific) into the
// parser-independent binding::FieldList (see field_tree.h). Everything past
// this header deals in Field/FieldList only.
// ============================================================================

namespace binding {

inline FieldList from_ptree(const boost::property_tree::ptree& pt) {
    FieldList fields;

    for (const auto& [key, child] : pt) {
        if (key == "<xmlattr>") {
            // xml_parser nests attributes one level down under a synthetic
            // "<xmlattr>" child instead of listing them alongside child
            // elements. Flatten them into this level so a caller never has
            // to know that's a property_tree/xml_parser implementation
            // detail rather than part of the config's own shape.
            for (const auto& [attr_name, attr_value] : child) {
                fields.push_back(Field{attr_name, attr_value.data()});
            }
            continue;
        }

        if (child.empty()) {
            // No children -> this node's own text is its whole value.
            fields.push_back(Field{key, child.data()});
        } else {
            // Has children (plain child elements and/or attributes) -> a
            // nested struct. Note this also applies to an attribute-only,
            // self-closing element like <pool size="10"/>: it becomes a
            // one-field nested struct {pool: {size: "10"}}, not a bare leaf
            // {pool: "10"} -- the attribute's own name would otherwise be
            // lost.
            fields.push_back(Field{key, from_ptree(child)});
        }
    }

    return fields;
}

} // namespace binding
