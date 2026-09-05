#include "binding/field_tree_parser.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "binding/config_bind.h"   // iequals
#include "binding/ptree_bridge.h"  // from_ptree

#if __has_include(<toml++/toml.h>)
#  define BINDING_HAS_TOML 1
#  include "binding/toml_bridge.h" // from_toml
#else
#  define BINDING_HAS_TOML 0
#endif

// The only translation unit that names a parser library.

namespace binding {
namespace {

std::string lower_extension(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// Reads one file into a FieldList, choosing the reader by extension. Every
// format converges here: what comes back is the same flattened Field tree
// regardless of which library did the reading.
FieldList read_document(const std::filesystem::path& file) {
    const std::string ext = lower_extension(file);

    if (ext == ".xml" || ext == ".json") {
        boost::property_tree::ptree document;
        try {
            if (ext == ".xml") {
                boost::property_tree::read_xml(file.string(), document);
            } else {
                boost::property_tree::read_json(file.string(), document);
            }
        } catch (const std::exception& e) {
            throw std::runtime_error("binding: failed to parse config '" + file.string() +
                                     "': " + e.what());
        }

        // An XML document always has exactly one root element, and making
        // every path spell it out ("config.pool.size") would be noise in
        // every call -- so paths are written relative to the document
        // element. JSON has no such wrapper (its root object *is* the
        // top level), and read_json leaves it as a multi-child root, so
        // the same test distinguishes them without needing to know which
        // format produced the tree.
        const boost::property_tree::ptree& top =
            document.size() == 1 && document.begin()->second.size() > 0
                ? document.begin()->second
                : document;
        return from_ptree(top);
    }

    if (ext == ".toml") {
#if BINDING_HAS_TOML
        try {
            return from_toml(toml::parse_file(file.string()));
        } catch (const std::exception& e) {
            throw std::runtime_error("binding: failed to parse config '" + file.string() +
                                     "': " + e.what());
        }
#else
        throw std::runtime_error("binding: cannot read '" + file.string() +
                                 "': this build has no TOML support (toml++ was not available)");
#endif
    }

    throw std::runtime_error("binding: unrecognized config format for '" + file.string() +
                             "' (expected .xml, .json, or .toml)");
}

} // namespace

FieldTreeParser FieldTreeParser::load(const std::filesystem::path& file) {
    return FieldTreeParser(
        std::make_shared<const Document>(Document{Field{std::string(), read_document(file)}}));
}

FieldTreeParser FieldTreeParser::load_into(std::string_view name,
                                            const std::filesystem::path& file) const {
    // Copy this tree, then splice the new document in under `name`. Both
    // documents are Fields by now, so nothing here depends on either
    // format -- a JSON config naming an XML one to pull in is the same
    // operation as any other combination.
    Field merged = doc_->root;
    merged.as_struct().push_back(Field{std::string(name), read_document(file)});
    return FieldTreeParser(std::make_shared<const Document>(Document{std::move(merged)}));
}

std::optional<FieldTreeParser::Node> FieldTreeParser::resolve(Node base,
                                                               std::string_view path) const {
    const Field* current = base;

    while (!path.empty()) {
        if (!current->is_struct()) return std::nullopt;

        const auto dot = path.find('.');
        const std::string_view segment = (dot == std::string_view::npos) ? path : path.substr(0, dot);

        const Field* found = nullptr;
        for (const Field& f : current->as_struct()) {
            if (iequals(f.name, segment)) {
                found = &f;
                break;
            }
        }
        if (!found) return std::nullopt;

        current = found;
        path = (dot == std::string_view::npos) ? std::string_view{} : path.substr(dot + 1);
    }

    return current;
}

} // namespace binding
