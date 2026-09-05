#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "binding/config_field.h"

// ============================================================================
// A config_parser (config_parser.h) that converts a whole document to
// parser-independent Fields once, at load, and navigates that.
//
// One class rather than one per format, because the format only matters
// for the few lines that read the file: XML and JSON both come from
// boost::property_tree, TOML from toml++, and all three land in the same
// Field tree afterward. load() picks by extension:
//
//     .xml           -> boost::property_tree::read_xml
//     .json          -> boost::property_tree::read_json
//     .toml          -> toml::parse_file          (when toml++ is available)
//
// Converting up front rather than navigating the parser's own tree is a
// deliberate choice for this parser, not something the seam imposes.
// ptree's get_child_optional() cannot answer the questions asked here: it
// matches case-sensitively, and an XML attribute (<pool size="10"/>) lives
// under a synthetic <xmlattr> child, so "pool.size" doesn't resolve to it
// at all. from_ptree()/from_toml() normalize both -- flattening each level
// into one uniform FieldList -- which is what makes a config path mean the
// same thing as a struct field name, and what lets one document's tree be
// spliced into another's (see load_into) regardless of which format each
// came from.
//
// Node is `const Field*` -- an 8-byte handle into the tree this parser
// owns, satisfying config_parser.h's lifetime rule: the tree is immutable
// and heap-allocated, shared by every copy of the parser, so a handle stays
// valid for as long as any copy lives.
// ============================================================================

namespace binding {

class FieldTreeParser {
public:
    using Node = const Field*;

    // Throws std::runtime_error if the file is missing, has an unknown
    // extension, or fails to parse.
    static FieldTreeParser load(const std::filesystem::path& file);

    // Loads `file` and splices it into this parser's tree under `name`, so
    // both documents answer to one set of paths -- the unified-lookup case
    // where one config (JSON, say) names another (XML) to pull in.
    // Formats need not match: both become Fields either way. Returns a new
    // parser; this one is untouched.
    [[nodiscard]] FieldTreeParser load_into(std::string_view name,
                                             const std::filesystem::path& file) const;

    Node root() const noexcept { return &doc_->root; }

    // Walks a dot-separated path from `base`, matching each segment
    // case-insensitively -- the same rule struct binding uses, so a path
    // and a struct field name mean the same thing. nullopt if any segment
    // is missing or a non-terminal one isn't a subtree; an empty path is
    // `base` itself.
    std::optional<Node> resolve(Node base, std::string_view path) const;

    // The node's contents, for the caller to consume: its own scalar, or
    // one flattened level of children.
    FieldValue to_value(Node node) const { return node->value; }

private:
    struct Document {
        Field root;
    };

    explicit FieldTreeParser(std::shared_ptr<const Document> doc) noexcept
        : doc_(std::move(doc)) {}

    // Shared, so copying a parser -- which every Configuration and
    // section() of one does -- keeps the document alive without
    // duplicating it, and every Node handed out stays valid.
    std::shared_ptr<const Document> doc_;
};

} // namespace binding
