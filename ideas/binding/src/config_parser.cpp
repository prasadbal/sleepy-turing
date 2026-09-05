#include "binding/configuration.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "binding/config_bind.h"   // iequals
#include "binding/ptree_bridge.h"  // from_ptree

// ============================================================================
// The parser impl, and the only translation unit in the project that names
// boost::property_tree. Everything else -- configuration.h and every caller
// of it -- sees only a std::any, so replacing this file is the whole cost
// of swapping the parser.
//
// The document is converted to parser-independent Fields once, at load, and
// paths resolve against that rather than against the ptree. That isn't
// laziness about "leveraging the parser's own lookup": ptree's
// get_child_optional() genuinely cannot answer the question being asked
// here. It matches case-sensitively, and an XML attribute (<pool
// size="10"/>) lives under a synthetic <xmlattr> child, so "pool.size"
// doesn't resolve to it at all. from_ptree() already normalizes both --
// flattening attributes alongside child elements, and trimming -- which is
// what makes a config path mean the same thing as a struct field name.
//
// A different backend can make a different call behind this same seam:
// toml++ has no <xmlattr> equivalent, so a TOML impl could resolve natively
// against toml::table and convert only the subtree it lands on.
// ============================================================================

namespace binding {
namespace {

// The parsed document, kept alive by every Configuration viewing into it.
struct Document {
    Field root;
};

// What actually sits in Configuration's std::any: a node, plus shared
// ownership of the document it points into -- so a section() outliving the
// Configuration it came from still has a valid tree underneath it.
struct NodeRef {
    std::shared_ptr<const Document> doc;
    const Field* node = nullptr;
};

} // namespace

Configuration load_config(const std::filesystem::path& file, bool strict) {
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

    auto doc = std::make_shared<Document>(Document{Field{std::string(), from_ptree(top)}});
    const Field* root = &doc->root;
    return Configuration(std::any(NodeRef{std::move(doc), root}), strict);
}

std::any Configuration::resolve(std::string_view path) const {
    const auto& base = std::any_cast<const NodeRef&>(node_);
    const Field* current = base.node;

    while (!path.empty()) {
        if (!current->is_struct()) return {};

        const auto dot = path.find('.');
        const std::string_view segment = (dot == std::string_view::npos) ? path : path.substr(0, dot);

        const Field* found = nullptr;
        for (const Field& f : current->as_struct()) {
            if (iequals(f.name, segment)) {
                found = &f;
                break;
            }
        }
        if (!found) return {};

        current = found;
        path = (dot == std::string_view::npos) ? std::string_view{} : path.substr(dot + 1);
    }

    return std::any(NodeRef{base.doc, current});
}

Field Configuration::to_field(const std::any& node) const {
    return *std::any_cast<const NodeRef&>(node).node;
}

} // namespace binding
