#pragma once
#include <concepts>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

#include "binding/config_field.h"

// ============================================================================
// What a config parser has to provide -- the replaceable seam Configuration
// (configuration.h) reads through, stated as a concept rather than an
// abstract base class: it's a compile-time contract, so there's no vtable,
// no allocation, and no indirection at the call site, and a backend that
// doesn't satisfy it fails at the static_assert in configuration.cpp,
// naming the missing operation, rather than at a link error later.
//
//   P::load(file) -> P            parse a document; the parser owns it
//   parser.root() -> Node         the document's own node
//   parser.resolve(node, path)    walk a dot-separated path from a node;
//        -> optional<Node>        nullopt if it doesn't resolve, and an
//                                 empty path is that same node
//   parser.to_value(node)         that node's contents as parser-
//        -> FieldValue            independent data: its own scalar, or
//                                 one flattened level of children
//
// Node is a *handle*, copied by value, not a node body: whatever is
// cheapest for that backend to carry around -- a pointer, a view, an
// index. That matters because a real parser's node can be substantial
// (boost::property_tree's is dozens of bytes), and nothing here should
// copy one; a backend whose nodes are heavy simply makes Node a pointer
// into storage its parser already owns. Returning by value rather than by
// pointer is also what lets a backend resolve lazily, or hand back a
// view type it materializes on the spot, instead of being forced to keep
// addressable storage for everything it might be asked about.
//
// Nothing outside the impl ever names Node -- Configuration holds it
// type-erased -- so it can be a ptree pointer, a toml::node_view, or a
// Field pointer, without any of that reaching a caller.
//
// Resolution is deliberately the parser's job rather than something
// Configuration does for it. It's format knowledge: in XML an attribute and
// a child element are both just "a field" to whoever writes the config,
// even though boost::property_tree files attributes away under a synthetic
// <xmlattr> child. Only the backend knows things like that, and it should
// only have to know them once.
// ============================================================================

namespace binding {

template <typename P>
concept config_parser =
    std::copyable<typename P::Node> &&
    requires(const P& parser, typename P::Node node, std::string_view path) {
        { P::load(std::declval<const std::filesystem::path&>()) } -> std::same_as<P>;
        { parser.root() } -> std::same_as<typename P::Node>;
        { parser.resolve(node, path) } -> std::same_as<std::optional<typename P::Node>>;
        { parser.to_value(node) } -> std::same_as<FieldValue>;
    };

} // namespace binding
