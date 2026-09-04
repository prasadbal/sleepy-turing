#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ============================================================================
// A parser-independent tree of (name, value) fields -- the intermediate form
// config parsing is converted into, before it ever meets a user struct or
// boost::pfr. Two bridges produce this same shape today: from_ptree()
// (ptree_bridge.h, for XML, where every leaf is text -- XML has no type
// system beyond that) and from_toml() (toml_bridge.h, for TOML, which IS
// natively typed at the parser level: an integer, float, or bool is stored
// as that C++ type, not as text someone happens to write digits into).
// Keeping this decoupled from boost::property_tree/toml++ means the
// struct-binding side never needs to know either parser exists, matching
// this project's existing rule that toml++ stays out of public headers
// (see core/config).
// ============================================================================

namespace binding {

// The reserved name an element's own text content is stored under when that
// element also has attributes or child elements, and so becomes a nested
// struct rather than a leaf (see ptree_bridge.h). Deliberately not a legal
// XML element name or C++ identifier, so it can never collide with a real
// child element's name.
inline constexpr std::string_view kTextFieldKey = "#text";

// Strips leading/trailing ASCII whitespace. Shared by the parser bridges (an
// XML document's indentation is formatting, not part of a config value) and
// by config_bind.h's numeric parsing.
inline std::string_view trim(std::string_view s) noexcept {
    constexpr std::string_view ws = " \t\r\n\f\v";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

struct Field;
using FieldList = std::vector<Field>;

// A leaf's actual value: either still raw text (the only thing a format
// like XML/ptree ever has -- a leaf is just whatever characters sat
// between the tags) or already one of TOML's native scalar types. This
// exists so from_toml() never has to force a typed value through a
// stringify-then-reparse round trip just to fit a string-only
// representation -- that would be wasted work at best, and lossy at
// worst: formatting a double and parsing it back is not guaranteed to
// reproduce the exact same bit pattern. config_bind.h's parse_leaf_value()
// takes whichever alternative is actually here: a fast direct conversion
// for an already-typed value, the existing text parsing (from_chars, the
// true/false/Y/N/1/0 rules for bool) for a string one.
using LeafValue = std::variant<std::string, std::int64_t, double, bool>;

// A leaf is a (name, LeafValue) pair. A struct is a name plus its own
// ordered FieldList. There's no separate "this is an array" case: a
// repeated element (the vector<T> case, e.g. multiple <replica> children
// under the same parent, or a TOML array of tables) isn't a distinct
// Field variant -- it's just several Field entries in the same enclosing
// FieldList that happen to share a name, which is also how ptree itself
// represents repetition (a duplicated key, not an array type). A
// struct-binder maps that to vector<T> by grouping same-named entries; a
// plain leaf lookup just takes the first (or only) match.
struct Field {
    std::string name;
    std::variant<LeafValue, FieldList> value;

    bool is_leaf() const noexcept { return std::holds_alternative<LeafValue>(value); }
    bool is_struct() const noexcept { return std::holds_alternative<FieldList>(value); }

    const LeafValue& as_leaf() const { return std::get<LeafValue>(value); }
    const FieldList& as_struct() const { return std::get<FieldList>(value); }

    // Non-const overloads -- exist only so a Field reached through a
    // genuinely non-const FieldList (see config_bind.h's FieldIndexT/
    // LinearFieldScannerT, templated on Field*/const Field* so this is
    // never called on something actually const) can have its leaf value
    // moved out during binding instead of copied, without ever needing a
    // const_cast anywhere in that path.
    LeafValue& as_leaf() { return std::get<LeafValue>(value); }
    FieldList& as_struct() { return std::get<FieldList>(value); }
};

} // namespace binding
