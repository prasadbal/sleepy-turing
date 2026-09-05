#include "binding/config_parser.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "binding/config_bind.h"   // iequals
#include "binding/ptree_bridge.h"  // from_ptree

// ============================================================================
// The parser impl, and the only translation unit in the project that names
// boost::property_tree.
//
// The whole document is converted to parser-independent Fields once, at
// load, and paths are resolved against that rather than against the ptree.
// That isn't laziness about "leveraging the parser's own lookup" -- ptree's
// get_child_optional() genuinely cannot answer the questions asked here:
// it matches case-sensitively, and an XML attribute (<pool size="10"/>)
// lives under a synthetic <xmlattr> child, so "pool.size" doesn't resolve
// to it at all. from_ptree() already normalizes both -- flattening
// attributes alongside child elements, and trimming -- so resolving against
// its output is what makes a path mean the same thing as a struct field
// name, which is the whole point.
//
// A different backend is free to make a different call here: toml++ has no
// <xmlattr> equivalent, so a TOML impl of this same interface could resolve
// natively against toml::table and convert only the subtree it lands on.
// The interface doesn't care -- that's what makes it the seam.
// ============================================================================

namespace binding {

// ConfigNode is only ever handled through a pointer, and both the cast out
// and the cast back happen right here, in this file -- a round trip through
// an incomplete type's pointer yields back the original pointer, and the
// Field is only ever dereferenced as a Field.
struct ConfigParser::Impl {
    Field root;
};

namespace {

const Field* as_field(const ConfigNode* node) noexcept {
    return reinterpret_cast<const Field*>(node);
}

const ConfigNode* as_node(const Field* field) noexcept {
    return reinterpret_cast<const ConfigNode*>(field);
}

} // namespace

ConfigParser ConfigParser::load(const std::filesystem::path& file) {
    boost::property_tree::ptree document;
    try {
        boost::property_tree::read_xml(file.string(), document);
    } catch (const std::exception& e) {
        throw std::runtime_error("binding: failed to parse config '" + file.string() +
                                 "': " + e.what());
    }

    // An XML document always has exactly one root element, and making every
    // path spell it out ("config.pool.size") would be noise in every call.
    // Descend into it once here, so paths are written relative to the
    // document element rather than to the file.
    const boost::property_tree::ptree& top =
        document.size() == 1 ? document.begin()->second : document;

    auto impl = std::make_shared<Impl>();
    impl->root = Field{std::string(), from_ptree(top)};
    return ConfigParser(std::move(impl));
}

const ConfigNode* ConfigParser::resolve(std::string_view path) const {
    const Field* current = &impl_->root;

    while (!path.empty()) {
        if (!current->is_struct()) return nullptr;

        const auto dot = path.find('.');
        const std::string_view segment = (dot == std::string_view::npos) ? path : path.substr(0, dot);

        const Field* found = nullptr;
        for (const Field& f : current->as_struct()) {
            if (iequals(f.name, segment)) {
                found = &f;
                break;
            }
        }
        if (!found) return nullptr;

        current = found;
        path = (dot == std::string_view::npos) ? std::string_view{} : path.substr(dot + 1);
    }

    return as_node(current);
}

FieldList ConfigParser::fields(const ConfigNode& node) const {
    const Field* field = as_field(&node);
    return field->is_struct() ? field->as_struct() : FieldList{};
}

std::optional<LeafValue> ConfigParser::leaf(const ConfigNode& node) const {
    const Field* field = as_field(&node);
    if (!field->is_leaf()) return std::nullopt;
    return field->as_leaf();
}

} // namespace binding
