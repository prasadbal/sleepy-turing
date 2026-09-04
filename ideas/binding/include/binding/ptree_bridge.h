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
            // No children -> this node's own text is its whole value. Trimmed
            // because an XML document's indentation and line breaks around a
            // value are formatting, not part of the value: without this, a
            // config pretty-printed as "<threads>\n    8\n  </threads>"
            // reached from_chars as "\n    8\n  " and was rejected as not a
            // number, so only single-line values happened to work.
            fields.push_back(Field{key, std::string(trim(child.data()))});
        } else {
            // Has children (plain child elements and/or attributes) -> a
            // nested struct. Note this also applies to an attribute-only,
            // self-closing element like <pool size="10"/>: it becomes a
            // one-field nested struct {pool: {size: "10"}}, not a bare leaf
            // {pool: "10"} -- the attribute's own name would otherwise be
            // lost.
            FieldList nested = from_ptree(child);

            // An element can have both attributes/children AND its own text
            // ("<host port=\"5432\">db1</host>"). That text used to be
            // dropped silently, because the node is not empty() and only the
            // nested-struct branch ran. It is kept under the reserved
            // kTextFieldKey name instead, so no configured value is lost.
            // Whitespace-only data is genuinely just formatting between child
            // elements and is still discarded.
            const std::string_view own_text = trim(child.data());
            if (!own_text.empty()) {
                nested.push_back(Field{std::string(kTextFieldKey), std::string(own_text)});
            }

            fields.push_back(Field{key, std::move(nested)});
        }
    }

    return fields;
}

} // namespace binding
