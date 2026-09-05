#include "binding/config_parser.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "binding/config_bind.h"   // iequals
#include "binding/ptree_bridge.h"  // find_ptree_child, from_ptree

#if __has_include(<toml++/toml.h>)
#  define BINDING_HAS_TOML 1
#  include "binding/toml_bridge.h" // find_toml_child, toml_leaf, from_toml
#else
#  define BINDING_HAS_TOML 0
#endif

// The only translation unit that names a parser library.
//
// The document stays in its native form; a FieldList is built only for the
// subtree being bound, by to_value(), and the caller consumes it. See the
// header for why navigating natively is safe here -- the "what fields exist
// at this level" rules live once per format, as a visitor that both the
// path walk and the conversion are built on.

namespace binding {
namespace {

using PtreeDoc = boost::property_tree::ptree;

constexpr int kPtreeNode = 1;
constexpr int kTomlNode = 2;

std::string lower_extension(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

const PtreeDoc* as_ptree(ConfigParser::Node node) noexcept {
    return node.kind == kPtreeNode ? static_cast<const PtreeDoc*>(node.impl) : nullptr;
}

#if BINDING_HAS_TOML
const toml::node* as_toml(ConfigParser::Node node) noexcept {
    return node.kind == kTomlNode ? static_cast<const toml::node*>(node.impl) : nullptr;
}
#endif

// One path segment's child, in whichever native tree `node` points into --
// the single place the two formats are told apart, so resolve() below reads
// as the plain segment walk it is.
//
// This deliberately isn't the parser's own path lookup. ptree's
// get_child_optional() is ruled out by two things: an XML attribute lives
// under a synthetic <xmlattr> child, so "pool.size" for <pool size="10"/>
// is really "pool.<xmlattr>.size" and nothing says per segment which form
// it will take; and ptree compares keys exactly, while config matching is
// case-insensitive (HOST binds to host). find_ptree_child/find_toml_child
// apply the same rules the conversion does, because both are built on the
// same for_each_*_child visitor -- which is what stops a path from
// resolving differently than binding reads the document.
std::optional<ConfigParser::Node> find_child(ConfigParser::Node node, std::string_view segment) {
    if (const PtreeDoc* pt = as_ptree(node)) {
        if (const PtreeDoc* child = find_ptree_child(*pt, segment)) {
            return ConfigParser::Node{child, kPtreeNode};
        }
        return std::nullopt;
    }
#if BINDING_HAS_TOML
    if (const toml::node* tn = as_toml(node)) {
        if (const toml::table* table = tn->as_table()) {
            if (const toml::node* child = find_toml_child(*table, segment)) {
                return ConfigParser::Node{child, kTomlNode};
            }
        }
    }
#endif
    return std::nullopt;
}

} // namespace

// A document in whichever form its parser produced, plus any documents
// attached to it by name (load_into). Both natives are held in a variant
// rather than behind a common base: the two libraries have nothing to
// abstract over, and switching on two alternatives in three short functions
// is less machinery than a virtual interface would be.
struct ConfigParser::Document {
#if BINDING_HAS_TOML
    std::variant<PtreeDoc, toml::table> native;
#else
    std::variant<PtreeDoc> native;
#endif
    // Kept alive by this document, so a Node into an attached tree is as
    // valid as one into the primary.
    std::vector<std::pair<std::string, std::shared_ptr<const Document>>> attached;

    Node root_node() const {
#if BINDING_HAS_TOML
        if (const auto* table = std::get_if<toml::table>(&native)) {
            return Node{static_cast<const toml::node*>(table), kTomlNode};
        }
#endif
        return Node{&std::get<PtreeDoc>(native), kPtreeNode};
    }
};

ConfigParser ConfigParser::load(const std::filesystem::path& file) {
    return ConfigParser(read(file));
}

ConfigParser ConfigParser::load_into(std::string_view name,
                                            const std::filesystem::path& file) const {
    auto merged = std::make_shared<Document>(*doc_);
    merged->attached.emplace_back(std::string(name), read(file));
    return ConfigParser(std::move(merged));
}

ConfigParser::Node ConfigParser::root() const { return doc_->root_node(); }

std::optional<ConfigParser::Node> ConfigParser::resolve(Node base,
                                                              std::string_view path) const {
    Node current = base;

    while (!path.empty()) {
        const auto dot = path.find('.');
        const std::string_view segment = (dot == std::string_view::npos) ? path : path.substr(0, dot);

        std::optional<Node> next = find_child(current, segment);

        // A document attached by load_into() answers for its own name, but
        // only at the top: it was attached to the document, not to a node
        // inside it, so this is the one level where the two trees meet.
        // Checked only as a fallback, so an attachment can never shadow a
        // key the primary document actually has.
        if (!next && current.impl == doc_->root_node().impl) {
            for (const auto& [attached_name, attached_doc] : doc_->attached) {
                if (iequals(attached_name, segment)) {
                    next = attached_doc->root_node();
                    break;
                }
            }
        }

        if (!next) return std::nullopt;

        current = *next;
        path = (dot == std::string_view::npos) ? std::string_view{} : path.substr(dot + 1);
    }

    return current;
}

FieldValue ConfigParser::to_value(Node node) const {
    if (const PtreeDoc* pt = as_ptree(node)) {
        // Childless -> the node's own text is its whole value, trimmed for
        // the reason from_ptree trims: an XML document's indentation around
        // a value is formatting, not part of it.
        if (pt->empty()) return LeafValue(std::string(trim(pt->data())));
        return from_ptree(*pt);
    }
#if BINDING_HAS_TOML
    if (const toml::node* tn = as_toml(node)) {
        if (const toml::table* table = tn->as_table()) return from_toml(*table);
        if (auto leaf = toml_leaf(*tn)) return std::move(*leaf);
    }
#endif
    // A date/time, or anything else with no LeafValue representation yet
    // (see from_toml's closing note). An empty FieldList reads as "a
    // subtree with nothing in it", which binding already handles.
    return FieldList{};
}

std::shared_ptr<ConfigParser::Document> ConfigParser::read(const std::filesystem::path& file) {
    const std::string ext = lower_extension(file);
    auto doc = std::make_shared<ConfigParser::Document>();

    if (ext == ".xml" || ext == ".json") {
        PtreeDoc parsed;
        try {
            if (ext == ".xml") {
                boost::property_tree::read_xml(file.string(), parsed);
            } else {
                boost::property_tree::read_json(file.string(), parsed);
            }
        } catch (const std::exception& e) {
            throw std::runtime_error("binding: failed to parse config '" + file.string() +
                                     "': " + e.what());
        }

        // An XML document always has exactly one root element, and making
        // every path spell it out ("config.pool.size") would be noise in
        // every call -- so for XML, paths are written relative to the
        // document element. JSON gets no such treatment: its root object
        // *is* the top level.
        if (ext == ".xml" && parsed.size() == 1) {
            doc->native = parsed.begin()->second;
        } else {
            doc->native = std::move(parsed);
        }
        return doc;
    }

    if (ext == ".toml") {
#if BINDING_HAS_TOML
        try {
            doc->native = toml::parse_file(file.string());
        } catch (const std::exception& e) {
            throw std::runtime_error("binding: failed to parse config '" + file.string() +
                                     "': " + e.what());
        }
        return doc;
#else
        throw std::runtime_error("binding: cannot read '" + file.string() +
                                 "': this build has no TOML support (toml++ was not available)");
#endif
    }

    throw std::runtime_error("binding: unrecognized config format for '" + file.string() +
                             "' (expected .xml, .json, or .toml)");
}

} // namespace binding
