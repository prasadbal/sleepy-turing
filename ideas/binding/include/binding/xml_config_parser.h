#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "binding/config_field.h"

// ============================================================================
// The XML backend: one implementation of the config_parser concept
// (config_parser.h). boost::property_tree appears only in this class's own
// .cpp, never here, so including this header costs no Boost dependency.
//
// Its Node is a `const Field*` -- a pointer into a tree this parser owns,
// which is what a Node is meant to be: an 8-byte handle, not a node body.
// The tree itself is built once at load, converting the whole document to
// parser-independent Fields, with each level flattened (an XML attribute
// and a child element both becoming plain entries in the same FieldList).
//
// That up-front conversion is a deliberate choice for *this* backend, not
// something the seam imposes. ptree's own get_child_optional() genuinely
// cannot answer the question being asked: it matches case-sensitively, and
// an attribute (<pool size="10"/>) lives under a synthetic <xmlattr>
// child, so "pool.size" doesn't resolve to it at all. from_ptree()
// normalizes both, which is what makes a config path mean the same thing
// as a struct field name. A TOML backend has no <xmlattr> equivalent and
// could navigate toml::table directly, converting only the subtree it
// lands on -- the concept doesn't care either way.
// ============================================================================

namespace binding {

class XmlConfigParser {
public:
    // A handle into the tree below, not a copy of anything in it.
    using Node = const Field*;

    // Throws std::runtime_error if the file is missing or malformed.
    static XmlConfigParser load(const std::filesystem::path& file);

    Node root() const noexcept { return &doc_->root; }

    // Walks a dot-separated path from `base`, matching each segment
    // case-insensitively -- the same rule struct binding uses, so a path
    // and a struct field name mean the same thing. nullopt if any segment
    // is missing or a non-terminal one isn't a subtree; an empty path is
    // `base` itself.
    std::optional<Node> resolve(Node base, std::string_view path) const;

    // The node's contents, which the caller consumes: its scalar value, or
    // its children as one flattened FieldList ready for binding.
    FieldValue to_value(Node node) const { return node->value; }

private:
    struct Document {
        Field root;
    };

    explicit XmlConfigParser(std::shared_ptr<const Document> doc) noexcept
        : doc_(std::move(doc)) {}

    // Shared, so copying a parser (which every Configuration and section()
    // of one does) keeps the parsed document alive without duplicating it,
    // and every Node handed out stays valid for as long as any copy lives.
    std::shared_ptr<const Document> doc_;
};

} // namespace binding
