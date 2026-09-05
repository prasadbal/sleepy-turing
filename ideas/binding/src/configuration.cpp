#include "binding/configuration.h"

#include <utility>

#include "binding/config_parser.h"
#include "binding/xml_config_parser.h"

// ============================================================================
// Configuration's two private hooks, plus load_config -- the only places
// that know which parser is actually in play, and the only ones that name
// its Node type (Configuration itself holds it type-erased in a std::any,
// so no header does).
//
// Swapping the backend is the alias below and nothing else: the concept
// check on the next line is what makes that safe, failing here and naming
// the missing operation rather than at some call site further away.
// ============================================================================

namespace binding {
namespace {

using ActiveParser = XmlConfigParser;
static_assert(config_parser<ActiveParser>);

// What sits in Configuration's std::any. The parser is held by value and
// shares ownership of the parsed document, so `node` stays valid for as
// long as any Configuration pointing into that document does -- including
// a section() that outlives the Configuration it came from.
struct NodeRef {
    ActiveParser parser;
    const ActiveParser::Node* node = nullptr;
};

} // namespace

Configuration load_config(const std::filesystem::path& file, bool strict) {
    ActiveParser parser = ActiveParser::load(file);
    const ActiveParser::Node* root = parser.root();
    return Configuration(std::any(NodeRef{std::move(parser), root}), strict);
}

std::any Configuration::resolve(std::string_view path) const {
    const auto& base = std::any_cast<const NodeRef&>(node_);
    const ActiveParser::Node* found = base.parser.resolve(*base.node, path);
    if (!found) return {};
    return std::any(NodeRef{base.parser, found});
}

Field Configuration::to_field(const std::any& node) const {
    const auto& ref = std::any_cast<const NodeRef&>(node);
    return ref.parser.to_field(*ref.node);
}

} // namespace binding
