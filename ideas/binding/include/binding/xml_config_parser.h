#pragma once
#include <filesystem>
#include <memory>
#include <string_view>

#include "binding/config_field.h"

// ============================================================================
// The XML backend: one implementation of the config_parser concept
// (config_parser.h). boost::property_tree appears only in this class's own
// .cpp, never here, so including this header costs no Boost dependency.
//
// Its Node is a Field: the document is converted to parser-independent
// Fields once, at load, and paths are resolved against that. That is a
// deliberate choice for *this* backend, not a limitation of the seam.
// ptree's own get_child_optional() genuinely cannot answer the question
// being asked -- it matches case-sensitively, and an XML attribute
// (<pool size="10"/>) lives under a synthetic <xmlattr> child, so
// "pool.size" doesn't resolve to it at all. from_ptree() already
// normalizes both, which is what makes a config path mean the same thing
// as a struct field name. A TOML backend has no <xmlattr> equivalent and
// could just as well navigate toml::table directly, converting only the
// subtree it lands on -- the concept doesn't care either way.
// ============================================================================

namespace binding {

class XmlConfigParser {
public:
    using Node = Field;

    // Throws std::runtime_error if the file is missing or malformed.
    static XmlConfigParser load(const std::filesystem::path& file);

    const Node* root() const noexcept { return &doc_->root; }

    // Walks a dot-separated path from `base`, matching each segment
    // case-insensitively -- the same rule struct binding uses, so a path
    // and a struct field name mean the same thing. nullptr if any segment
    // is missing or a non-terminal one isn't a subtree; an empty path is
    // `base` itself.
    const Node* resolve(const Node& base, std::string_view path) const;

    // This backend's Node is already a Field, so there is nothing to
    // convert -- the copy is the caller's to consume (Configuration binds
    // out of it and drops it).
    Field to_field(const Node& node) const { return node; }

private:
    struct Document {
        Field root;
    };

    explicit XmlConfigParser(std::shared_ptr<const Document> doc) noexcept
        : doc_(std::move(doc)) {}

    // Shared, so copying a parser (which every Configuration and section()
    // of one does) keeps the parsed document alive without duplicating it.
    std::shared_ptr<const Document> doc_;
};

} // namespace binding
