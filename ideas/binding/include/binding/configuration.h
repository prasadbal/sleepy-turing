#pragma once
#include <any>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "binding/config_bind.h"
#include "binding/config_field.h"

// ============================================================================
// The config-reading interface: one get(), reached by path, for either a
// single scalar or a whole struct.
//
// A Configuration is a view of one node -- the document root to begin with,
// or any section of it (see section()), after which paths are relative to
// that section. The node is held type-erased in a std::any, so this header
// names no parser type at all: config_parser.cpp is the only translation
// unit that knows what's actually inside it, and swapping the parser is
// that one source file, with no caller recompiling for it.
//
// What get() does with the node depends only on T:
//   - a scalar target -> parse the node's own leaf value.
//   - a struct target -> bind the node's children.
//
// Required vs absent is decided by T too, never by an extra argument: a
// plain T throws if it isn't there, std::optional<T> yields nullopt -- for
// a scalar and a struct alike. There's deliberately no defaulting overload:
// std::optional's own value_or() already is one, and composes without
// adding a second way to spell the same thing.
//
//   Configuration cfg = load_config("app.xml");
//   cfg.get<int>("port")                          // throws if absent
//   cfg.get<std::optional<int>>("port")           // nullopt if absent
//   cfg.get<std::optional<int>>("port").value_or(9000)
//   cfg.get<AppConfig>()                          // whole document
//   cfg.get<Pool>("pool")                         // one section
//   cfg.get<std::optional<Pool>>("pool")          // nullopt if absent
//   cfg.get("pool", existing_pool)                // deduced, filled in place
//
//   Configuration logging = cfg.section("logging");
//   logging.get<std::string>("level")             // relative to the section
//
// Note value_or() defaults only an *absent* value: one that is present but
// malformed ("port = abc") still throws, rather than silently reading as
// 9000.
// ============================================================================

namespace binding {

// What Configuration requires of a parser -- stated as a concept rather
// than an abstract base class: a compile-time contract, so there's no
// vtable, no allocation, and no indirection at the call site, and a
// backend that doesn't satisfy it fails at the static_assert in
// configuration.cpp naming the missing operation, rather than at a link
// error later. ConfigParser (config_parser.h) is the implementation
// shipped; this is what a replacement would have to provide.
//
//   P::load(file) -> P            parse a document; the parser owns it
//   parser.root() -> Node         the document's own node
//   parser.resolve(node, path)    walk a dot-separated path from a node;
//        -> optional<Node>        nullopt if it doesn't resolve, and an
//                                 empty path is that same node
//   parser.to_value(node)         that node's contents as parser-
//        -> FieldValue            independent data: its own scalar, or
//                                 one flattened level of children
//
// Node is a *handle*, copied by value, not a node body: whatever is
// cheapest for that backend to carry around -- a pointer, a view, an
// index. That matters because a real parser's node can be substantial
// (boost::property_tree's runs to dozens of bytes), and nothing here
// should copy one; a backend whose nodes are heavy makes Node a pointer
// into storage its parser already owns. Returning by value rather than by
// pointer is also what lets a backend resolve lazily, or hand back a view
// it materializes on the spot.
//
// LIFETIME, which a concept cannot express and an implementation
// therefore has to be told:
//
//   A Node must stay valid for as long as any copy of the parser that
//   produced it is alive, and must not be invalidated by any later call
//   on that parser.
//
// Configuration depends on exactly this. It stores a parser *by value*
// alongside each Node it holds, so owning a Configuration owns a share of
// the document, and every get() is safe however long after the load it
// happens -- including from a section() whose parent Configuration is long
// gone. Copying the parser is what keeps the document alive, so a parser
// must be cheap to copy and must share its document rather than own it
// outright. A backend returning a handle into storage it might reallocate,
// or one invalidated by the next resolve(), would satisfy every
// requirement below and still break Configuration silently.
template <typename P>
concept config_parser =
    std::copyable<typename P::Node> &&
    requires(const P& parser, typename P::Node node, std::string_view path) {
        { P::load(std::declval<const std::filesystem::path&>()) } -> std::same_as<P>;
        { parser.root() } -> std::same_as<typename P::Node>;
        { parser.resolve(node, path) } -> std::same_as<std::optional<typename P::Node>>;
        { parser.to_value(node) } -> std::same_as<FieldValue>;
    };

class Configuration {
public:
    // A view of the node at `path`, with its own paths then relative to it.
    // Throws if `path` doesn't resolve, matching get<T>'s own rule that a
    // plain (non-optional) read of something absent is an error.
    Configuration section(std::string_view path) const {
        std::any node = resolve(path);
        if (!node.has_value()) throw detail::missing_field_error(path);
        return Configuration(std::move(node), strict_);
    }

    template <gettable T>
    T get(std::string_view path = "") const {
        T out{};
        get(path, out);
        return out;
    }

    // Same semantics, deduced T, filled in place -- for reading straight
    // into something you already have, without naming the type twice.
    //
    // This layer's whole job is path -> node; what a node then means for a
    // given T -- leaf or struct, required or absent -- is binding's, so it
    // stays in config_bind.h with the rest of that logic rather than being
    // spelled out a second time here.
    template <gettable T>
    void get(std::string_view path, T& out) const {
        std::any node = resolve(path);
        detail::bind_resolved_node(
            node.has_value() ? std::optional<FieldValue>(to_value(node)) : std::nullopt,
            out, path, strict_);
    }

private:
    friend Configuration load_config(const std::filesystem::path& file, bool strict);

    explicit Configuration(std::any node, bool strict) noexcept
        : node_(std::move(node)), strict_(strict) {}

    // The two format-specific operations, both defined in
    // configuration.cpp, which is the only place that knows which parser
    // is in use. resolve() walks a dot-separated path from this
    // Configuration's own node (an empty path being that node itself) and
    // returns an empty std::any if it doesn't resolve; to_value() hands
    // back that node's contents -- its own scalar, or one flattened level
    // of children -- as parser-independent data.
    std::any resolve(std::string_view path) const;
    FieldValue to_value(const std::any& node) const;

    std::any node_;
    bool strict_ = false;
};

// Parses `file` and returns a Configuration over its root. Standalone
// rather than a static member, so loading -- the one genuinely
// format-specific thing a caller does -- isn't tangled into the reading
// interface, which is format-agnostic. Throws std::runtime_error if the
// file is missing or malformed.
//
// `strict`, when true, additionally rejects any config key that doesn't
// match one of the target struct's own fields, at every nesting level, for
// every get() made through this Configuration and any section() of it. It
// describes the document rather than an individual read, which is why it
// lives here and not on get() (see bind_from_fields' own comment for why
// it's a runtime value and not a compile-time flag).
Configuration load_config(const std::filesystem::path& file, bool strict = false);

// FieldList and Field satisfy neither is_bindable_leaf nor config_schema,
// so no instantiation of get() can hand one back out -- asserted directly
// rather than left implied, so a future widening of either concept fails
// here instead of silently reopening what this facade exists to hide.
static_assert(!gettable<FieldList>);
static_assert(!gettable<Field>);

} // namespace binding
