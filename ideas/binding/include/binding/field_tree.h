#pragma once
#include <string>
#include <variant>
#include <vector>

// ============================================================================
// A parser-independent tree of (name, value) fields -- the intermediate form
// config parsing (XML today, via from_ptree() below; TOML/INI could produce
// the same shape) is converted into, before it ever meets a user struct or
// boost::pfr. Keeping this decoupled from boost::property_tree means the
// struct-binding side (not written yet) never needs to know a ptree exists,
// matching this project's existing rule that toml++ stays out of public
// headers (see core/config).
// ============================================================================

namespace binding {

struct Field;
using FieldList = std::vector<Field>;

// A leaf is a plain (name, string value) pair. A struct is a name plus its
// own ordered FieldList. There's no separate "this is an array" case: a
// repeated element (the vector<T> case, e.g. multiple <replica> children
// under the same parent) isn't a distinct Field variant -- it's just several
// Field entries in the same enclosing FieldList that happen to share a name,
// which is also how ptree itself represents repetition (a duplicated key,
// not an array type). A struct-binder maps that to vector<T> by grouping
// same-named consecutive entries; a plain leaf lookup just takes the first
// (or only) match.
struct Field {
    std::string name;
    std::variant<std::string, FieldList> value;

    bool is_leaf() const noexcept { return std::holds_alternative<std::string>(value); }
    bool is_struct() const noexcept { return std::holds_alternative<FieldList>(value); }

    const std::string& as_leaf() const { return std::get<std::string>(value); }
    const FieldList& as_struct() const { return std::get<FieldList>(value); }
};

} // namespace binding
