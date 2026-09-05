#include "binding/xml_config_parser.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "binding/config_bind.h"   // iequals
#include "binding/ptree_bridge.h"  // from_ptree

// The only translation unit in the project that names boost::property_tree.

namespace binding {

XmlConfigParser XmlConfigParser::load(const std::filesystem::path& file) {
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

    return XmlConfigParser(
        std::make_shared<const Document>(Document{Field{std::string(), from_ptree(top)}}));
}

const XmlConfigParser::Node* XmlConfigParser::resolve(const Node& base,
                                                       std::string_view path) const {
    const Field* current = &base;

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

    return current;
}

} // namespace binding
