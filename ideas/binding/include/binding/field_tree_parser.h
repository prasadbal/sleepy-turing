#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "binding/config_field.h"

// ============================================================================
// A config_parser (config_parser.h) over boost::property_tree (.xml, .json)
// and toml++ (.toml), chosen by extension at load.
//
// The parsed document is kept in its *native* form and navigated natively.
// A FieldList is materialized only for the subtree actually being bound, by
// to_value(), and dies when that bind finishes -- nothing here holds one.
// So the persistent representation is the parser's own tree, and Fields are
// transient, which is what binding wants anyway: bind_from_fields takes the
// resulting FieldList by rvalue and moves its leaf values into the target
// struct.
//
// What keeps that from becoming a second, subtly different reading of the
// same document is that "what fields exist at this level" is defined once
// per format, as a visitor (for_each_ptree_child / for_each_toml_child),
// with both the path walk and the conversion built on it. They cannot
// disagree about attribute flattening, array shapes, or case-insensitive
// matching. Walking paths with ptree's own get_child_optional() instead was
// tried and does not work: it cannot see XML attributes and matches
// case-sensitively, so "pool.size" resolved to nothing while binding found
// it fine.
// ============================================================================

namespace binding {

class FieldTreeParser {
public:
    // An opaque handle into the parsed document -- copyable, cheap, and
    // deliberately not naming any parser type, so this header carries no
    // parser dependency. What's inside is field_tree_parser.cpp's business.
    // Valid for as long as any copy of the parser that produced it lives,
    // as config_parser.h's lifetime rule requires: the document is
    // immutable and shared, so a handle can't outlive it.
    struct Node {
        const void* impl = nullptr;
        int kind = 0;
    };

    // Throws std::runtime_error if the file is missing, has an unknown
    // extension, or fails to parse.
    static FieldTreeParser load(const std::filesystem::path& file);

    // Loads `file` and attaches it under `name`, so both documents answer
    // to one set of paths -- the unified-lookup case where one config
    // (JSON, say) carries the path of another (XML) to pull in. The two
    // need not share a format: each keeps its own native tree, and `name`
    // resolves into the attached one. Returns a new parser; this one is
    // untouched.
    [[nodiscard]] FieldTreeParser load_into(std::string_view name,
                                             const std::filesystem::path& file) const;

    Node root() const;

    // Walks a dot-separated path from `base`, matching each segment
    // case-insensitively -- the same rule struct binding uses, so a path
    // and a struct field name mean the same thing. nullopt if any segment
    // is missing or a non-terminal one isn't a subtree; an empty path is
    // `base` itself.
    std::optional<Node> resolve(Node base, std::string_view path) const;

    // That node's contents, for the caller to consume: its own scalar, or
    // one flattened level of children. This is the only place a FieldList
    // comes into existence, and the caller owns the one it gets.
    FieldValue to_value(Node node) const;

private:
    struct Document;

    // Parses one file into a document, shared by load() and load_into() so
    // an attached document is read exactly the way a primary one is.
    static std::shared_ptr<Document> read(const std::filesystem::path& file);

    explicit FieldTreeParser(std::shared_ptr<const Document> doc) noexcept
        : doc_(std::move(doc)) {}

    std::shared_ptr<const Document> doc_;
};

} // namespace binding
