#pragma once
#include <concepts>
#include <filesystem>
#include <string_view>
#include <utility>

#include "binding/config_field.h"

// ============================================================================
// What a config parser has to provide -- the replaceable seam Configuration
// (configuration.h) reads through, stated as a concept rather than an
// abstract base class: it's a compile-time contract, so there's no vtable,
// no allocation, and no indirection at the call site, and a backend that
// doesn't satisfy it fails to compile at the static_assert in
// configuration.cpp rather than at some later link error.
//
// Three things, which is all Configuration ever asks for:
//
//   load(file)          parse a document; the parser owns it afterward
//   root()              the document's own node
//   resolve(node, path) walk a dot-separated path from a node to another,
//                       nullptr if it doesn't resolve; an empty path is
//                       that same node
//   to_field(node)      that node as parser-independent Fields -- its
//                       children as a FieldList for a struct, or its own
//                       leaf value for a scalar
//
// Node is the parser's own business: whatever it's cheapest for that
// backend to navigate. Nothing outside the impl ever names it (Configuration
// holds it type-erased in a std::any), so it can be a ptree, a toml::node,
// or -- as the XML backend does -- a Field in a tree converted up front.
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
    requires(const P& parser, const typename P::Node& node, std::string_view path) {
        typename P::Node;
        { P::load(std::declval<const std::filesystem::path&>()) } -> std::same_as<P>;
        { parser.root() } -> std::same_as<const typename P::Node*>;
        { parser.resolve(node, path) } -> std::same_as<const typename P::Node*>;
        { parser.to_field(node) } -> std::same_as<Field>;
    };

} // namespace binding
