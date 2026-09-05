#pragma once
#include <boost/property_tree/ptree.hpp>
#include <stdexcept>
#include <string>

#include "binding/config_bind.h"
#include "binding/config_field.h"

// ============================================================================
// The one place boost::property_tree is allowed to be named: converts a
// parsed ptree (from read_xml, but nothing here is XML-specific) into the
// parser-independent binding::FieldList (see config_field.h). Everything past
// this header deals in Field/FieldList only -- with one exception: get_leaf/
// try_get_leaf below, which read a single ad hoc value straight off a live
// ptree without first materializing a whole FieldList to throw away. They
// still finish through the exact same detail::parse_leaf_value() the
// FieldList-based get_leaf() (config_bind.h) uses -- only the "walk the path
// down to one node" step is ptree-specific, leveraging get_child_optional()
// (ptree's own dotted-path support) instead of reimplementing it.
// ============================================================================

namespace binding {

// True for the shape json_parser uses to represent an array: a node with
// children, every one of which has an empty key. XML cannot produce this
// (an element always has a tag name, and <xmlattr> is handled separately),
// so testing for it costs XML nothing and needs no format flag threaded
// through -- the tree itself says which case this is.
inline bool is_ptree_array(const boost::property_tree::ptree& node) {
    if (node.empty()) return false; // a scalar, or an empty array -- see below
    for (const auto& [key, child] : node) {
        (void)child;
        if (!key.empty()) return false;
    }
    return true;
}

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

        if (is_ptree_array(child)) {
            // json_parser represents an array as a child whose own children
            // all have empty keys: "ports": [8080, 8443] parses to
            // ports -> { ""->8080, ""->8443 }. Left alone, that would bind
            // as a nested struct named "ports" holding two nameless
            // entries, which no vector<T> field can collect.
            //
            // Emit one same-named entry per element instead, which is
            // exactly how repetition is already represented everywhere else
            // (see config_field.h): a repeated XML element and a TOML array
            // both arrive as several Fields sharing a name in the enclosing
            // list, and bind_one_field's vector<T> branch groups them. So a
            // JSON array of scalars becomes vector<leaf> and an array of
            // objects becomes vector<Struct>, matching what the other two
            // formats already do for the same config shape.
            for (const auto& [element_key, element] : child) {
                (void)element_key; // empty by construction -- that's the marker
                if (element.empty()) {
                    fields.push_back(Field{key, std::string(trim(element.data()))});
                } else {
                    fields.push_back(Field{key, from_ptree(element)});
                }
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

namespace detail {

// Converts one already-resolved ptree node into a LeafValue -- the
// single-node equivalent of from_ptree()'s leaf branch above, reused here
// so get_leaf/try_get_leaf never have to duplicate that trimming rule.
inline LeafValue leaf_value_from_ptree_node(const boost::property_tree::ptree& node) {
    return LeafValue(std::string(trim(node.data())));
}

} // namespace detail

// Reads a single scalar value at a dot-separated path straight off a live
// ptree, parsed as T -- path -> node -> LeafValue -> parse_leaf_value(),
// the same last step get_leaf() (config_bind.h) uses once it has a
// FieldList's Field in hand. Never materializes a FieldList for the parts
// of the document outside `path`: get_child_optional() is ptree's own
// dotted-path navigation (the same '.' convention config_bind.h's
// FieldList-based get_leaf uses), so resolving the path costs nothing
// beyond what ptree already does internally.
//
// Throws std::runtime_error, naming the path, if the path doesn't
// resolve, the target node isn't a leaf (has children), or its text
// doesn't parse as T -- same failure modes as the FieldList-based
// get_leaf(), for the same reason.
template <is_bindable_leaf T>
T get_leaf(const boost::property_tree::ptree& root, std::string_view path) {
    auto found = root.get_child_optional(std::string(path));
    if (!found) {
        throw std::runtime_error("binding: missing field (from path '" + std::string(path) + "')");
    }
    if (!found->empty()) {
        throw std::runtime_error("binding: field '" + std::string(path) + "' expected a plain value");
    }
    T value{};
    detail::parse_leaf_value(detail::leaf_value_from_ptree_node(*found), value, path);
    return value;
}

// Same as get_leaf above, but returns std::nullopt instead of throwing
// when `path` itself doesn't resolve -- the ptree-source equivalent of
// config_bind.h's FieldList-based try_get_leaf(). A value that IS present
// but fails to parse as T still throws: a real data error, not absence.
template <is_bindable_leaf T>
std::optional<T> try_get_leaf(const boost::property_tree::ptree& root, std::string_view path) {
    auto found = root.get_child_optional(std::string(path));
    if (!found || !found->empty()) return std::nullopt;
    T value{};
    detail::parse_leaf_value(detail::leaf_value_from_ptree_node(*found), value, path);
    return value;
}

} // namespace binding
