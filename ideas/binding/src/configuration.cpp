#include "binding/configuration.h"

#include <utility>

#include "binding/config_parser.h"

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

using ActiveParser = ConfigParser;
static_assert(config_parser<ActiveParser>);

// What sits in Configuration's std::any: a parser plus one of its node
// handles, deliberately stored together.
//
// The parser is held *by value*, not referred to, and that is the whole
// mechanism keeping this safe: a parser copy shares ownership of the
// parsed document (see config_parser.h's lifetime rule), so holding a
// Configuration holds the document up, and `node` cannot outlive what it
// points into. That makes a get() valid however long after the load it
// happens, and makes a section() independent of the Configuration it came
// from -- the parent can be destroyed and the child still reads.
//
// Storing the parser by reference, or hoisting one copy into a global,
// would both reintroduce exactly the dangling case this avoids.
struct NodeRef {
    ActiveParser parser;
    ActiveParser::Node node;
};

} // namespace

Configuration load_config(const std::filesystem::path& file, bool strict) {
    ActiveParser parser = ActiveParser::load(file);
    ActiveParser::Node root = parser.root();
    return Configuration(std::any(NodeRef{std::move(parser), root}), strict);
}

std::any Configuration::resolve(std::string_view path) const {
    const auto& base = std::any_cast<const NodeRef&>(node_);
    std::optional<ActiveParser::Node> found = base.parser.resolve(base.node, path);
    if (!found) return {};
    return std::any(NodeRef{base.parser, *found});
}

FieldValue Configuration::to_value(const std::any& node) const {
    const auto& ref = std::any_cast<const NodeRef&>(node);
    return ref.parser.to_value(ref.node);
}

} // namespace binding
