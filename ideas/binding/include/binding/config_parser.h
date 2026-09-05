#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "binding/config_field.h"

// ============================================================================
// The parser interface -- the impl seam. Everything format-specific lives
// behind this: loading a file, what a node *is*, and how a path resolves to
// one. config_parser.cpp is the only translation unit that names the actual
// parser (boost::property_tree today), so swapping it for toml++ or
// anything else is one .cpp, and no caller recompiles for it: no header
// anyone includes mentions the parser at all.
//
// Three operations, which is all Configuration (configuration.h) needs:
//
//   load(file)          parse a document
//   resolve(path)       path -> node, or nullptr if it isn't there
//   fields(node)        node -> FieldList, its children as
//                       parser-independent Fields, for struct binding
//   leaf(node)          node -> its own scalar value, if it is one
//
// Path resolution is deliberately the parser's job rather than something
// Configuration reimplements. It's format knowledge: in XML, for instance,
// an attribute and a child element are both just "a field" to a config
// author, even though boost::property_tree keeps attributes off in a
// synthetic <xmlattr> child -- so ptree's own get_child_optional() cannot
// resolve a path to one, and matches case-sensitively besides. Both are the
// impl's business to smooth over, in one place, not every caller's.
// ============================================================================

namespace binding {

// A node in a parsed document. Deliberately never defined in this header --
// it exists only to be pointed at, so its actual representation is the
// impl's private business and can change with the parser.
struct ConfigNode;

class ConfigParser {
public:
    // Throws std::runtime_error if the file is missing or malformed.
    static ConfigParser load(const std::filesystem::path& file);

    // The node at a dot-separated path, or nullptr if it doesn't resolve.
    // An empty path is the document root. Each segment matches
    // case-insensitively, the same rule struct binding uses.
    const ConfigNode* resolve(std::string_view path) const;

    // That node's children. Empty if the node is a scalar rather than a
    // subtree.
    FieldList fields(const ConfigNode& node) const;

    // That node's own value, or nullopt if it is a subtree rather than a
    // scalar.
    std::optional<LeafValue> leaf(const ConfigNode& node) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    explicit ConfigParser(std::shared_ptr<const Impl> impl) noexcept
        : impl_(std::move(impl)) {}
};

} // namespace binding
