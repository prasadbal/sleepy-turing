#pragma once
#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <boost/pfr.hpp>

#include "binding/config_field.h"
#include "binding/reflect.h"

// ============================================================================
// Binds a parser-independent FieldList (see config_field.h) onto a
// config_schema struct (see reflect.h), matching a struct field to a
// same-named Field case-insensitively via boost::pfr::names_as_array<T>().
//
// Deliberately name-based, not positional like oci_client.h: a repeated
// element collapses onto a single vector<T> field regardless of where its
// several same-named entries land among its differently-named siblings, so
// there's no single struct-field-index <-> FieldList-index correspondence
// to walk positionally. Matching by name is also just what a human editing
// a config file expects -- reordering keys shouldn't break parsing the way
// reordering SQL bind parameters legitimately can.
// ============================================================================

namespace binding {

inline std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

template <config_schema T>
void bind_from_fields(const FieldList& fields, T& out);

namespace detail {

// Case-insensitive name -> Field* index over one FieldList level, built once
// per bind_from_fields() call rather than re-scanning `fields` linearly for
// every field of T. Binding a T with M fields against an N-entry FieldList
// via a linear scan (the original implementation) is O(M*N); building this
// index costs O(N) once, after which each of T's M lookups is O(1) average,
// for O(N+M) overall. Keeps a vector per name, not a single Field*, because
// a repeated element (see config_field.h) means several entries can
// legitimately share a name.
class FieldIndex {
public:
    explicit FieldIndex(const FieldList& fields) {
        by_name_.reserve(fields.size());
        for (const auto& f : fields) {
            by_name_[to_lower(f.name)].push_back(&f);
        }
    }

    // The first entry with this name, or nullptr. For a non-repeated field
    // there's at most one; for a repeated element, use all() instead.
    const Field* first(std::string_view name) const {
        auto it = by_name_.find(to_lower(name));
        return (it == by_name_.end() || it->second.empty()) ? nullptr : it->second.front();
    }

    // Every entry with this name, in FieldList order (empty if none). By
    // value rather than by pointer into by_name_, so this has the same
    // signature as LinearFieldScanner::all() below -- bind_one_field/
    // bind_all_fields are templated on whichever of the two strategies
    // bind_from_fields<T> picks, and need one common interface to call
    // through regardless of which it is.
    std::vector<const Field*> all(std::string_view name) const {
        auto it = by_name_.find(to_lower(name));
        return it == by_name_.end() ? std::vector<const Field*>{} : it->second;
    }

private:
    std::unordered_map<std::string, std::vector<const Field*>> by_name_;
};

// Threshold matching examples/lookup_benchmark.cpp's measured crossover:
// below roughly this many fields, building FieldIndex's hash map costs
// more than a linear scan saves (real fixed overhead -- allocating
// buckets, hashing every string -- that a handful of string comparisons
// doesn't pay). This is a compile-time constant, so bind_from_fields<T>'s
// choice below is fixed once per T at compile time, not decided per call:
// one instantiation of bind_from_fields<ElemType> serves every element of
// a vector<ElemType> the same way, whether it's called once (a nested
// struct) or a thousand times (a thousand repeated <replica> elements),
// always through the same strategy for that ElemType.
inline constexpr std::size_t kLinearScanFieldThreshold = 16;

// Scans `fields` linearly for each lookup instead of building an index
// upfront -- the right tool for a small FieldList (the common case for
// one repeated element's own handful of sub-fields, e.g. a single
// <replica>'s host/port/priority): O(1) to construct, and each lookup is
// only a handful of string comparisons, cheaper overall than FieldIndex's
// hash map for a T with few fields (see kLinearScanFieldThreshold above).
class LinearFieldScanner {
public:
    explicit LinearFieldScanner(const FieldList& fields) : fields_(fields) {}

    const Field* first(std::string_view name) const {
        for (const Field& f : fields_) {
            if (to_lower(f.name) == to_lower(name)) return &f;
        }
        return nullptr;
    }

    std::vector<const Field*> all(std::string_view name) const {
        std::vector<const Field*> matches;
        for (const Field& f : fields_) {
            if (to_lower(f.name) == to_lower(name)) matches.push_back(&f);
        }
        return matches;
    }

private:
    const FieldList& fields_;
};

// Parses one leaf's raw string into a bindable leaf value. std::string
// fields just take the raw text; bool takes true/false, Y/N, or 1/0
// (case-insensitive) rather than going through from_chars, which has no
// bool overload at all -- std::is_arithmetic_v<bool> is true, so without
// this branch a bool field would pass is_bindable_leaf_v's compile-time
// check and then fail to compile at the from_chars call below, naming
// neither the field nor why; arithmetic fields go through from_chars and
// reject anything that doesn't fully consume the text (a leading-number
// match like "10abc" -> 10 would silently hide a typo in a config file).
template <typename T>
void parse_leaf_value(const std::string& raw, T& out, std::string_view field_name) {
    if constexpr (std::is_same_v<T, std::string>) {
        out = raw;
    } else if constexpr (std::is_same_v<T, bool>) {
        const std::string lower = to_lower(trim(raw));
        if (lower == "true" || lower == "y" || lower == "1") {
            out = true;
        } else if (lower == "false" || lower == "n" || lower == "0") {
            out = false;
        } else {
            throw std::runtime_error("binding: field '" + std::string(field_name) +
                                      "' value '" + raw + "' is not a valid bool "
                                      "(expected true/false, Y/N, or 1/0)");
        }
    } else if constexpr (std::is_arithmetic_v<T>) {
        // Trimmed first: from_chars rejects leading whitespace outright, so a
        // value that arrived with any surrounding formatting would otherwise
        // fail as "not a valid number". The XML bridge already trims (see
        // ptree_bridge.h); this covers every other FieldList source too.
        const std::string_view text = trim(raw);
        const auto* begin = text.data();
        const auto* end = text.data() + text.size();
        auto [ptr, ec] = std::from_chars(begin, end, out);
        if (ec != std::errc{} || ptr != end) {
            throw std::runtime_error("binding: field '" + std::string(field_name) +
                                      "' value '" + raw + "' is not a valid number");
        }
    } else {
        static_assert(is_bindable_leaf_v<T>, "parse_leaf_value: unsupported leaf type");
    }
}

template <std::size_t I, typename IndexT, config_schema T>
void bind_one_field(const IndexT& index, T& out, std::string_view name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(out);

    if constexpr (is_optional_v<FieldType>) {
        using ValueType = optional_value_t<FieldType>;
        const Field* f = index.first(name);
        if (!f) {
            field = std::nullopt;
            return;
        }
        if (!f->is_leaf()) {
            throw std::runtime_error("binding: field '" + std::string(name) + "' expected a plain value");
        }
        ValueType value{};
        parse_leaf_value(f->as_leaf(), value, name);
        field = std::move(value);

    } else if constexpr (is_vector_v<FieldType>) {
        using ElemType = vector_value_t<FieldType>;
        field.clear();
        for (const Field* f : index.all(name)) {
            if constexpr (is_bindable_leaf_v<ElemType>) {
                // A repeated leaf element, e.g. several <port>8080</port>
                // siblings -> std::vector<int>. Every match must actually
                // be a leaf, not a struct -- config_field_predicate only
                // guarantees the *field*'s element type is leaf-shaped,
                // not that the parsed data agrees.
                if (!f->is_leaf()) {
                    throw std::runtime_error("binding: field '" + std::string(name) + "' expected a plain value");
                }
                ElemType elem{};
                parse_leaf_value(f->as_leaf(), elem, name);
                field.push_back(std::move(elem));
            } else {
                if (!f->is_struct()) {
                    throw std::runtime_error("binding: field '" + std::string(name) + "' expected a nested structure");
                }
                ElemType elem{};
                bind_from_fields(f->as_struct(), elem);
                field.push_back(std::move(elem));
            }
        }

    } else if constexpr (is_bindable_leaf_v<FieldType>) {
        const Field* f = index.first(name);
        if (!f) {
            throw std::runtime_error("binding: missing required field '" + std::string(name) + "'");
        }
        if (!f->is_leaf()) {
            throw std::runtime_error("binding: field '" + std::string(name) + "' expected a plain value");
        }
        parse_leaf_value(f->as_leaf(), field, name);

    } else { // nested config_schema struct
        const Field* f = index.first(name);
        if (!f) {
            throw std::runtime_error("binding: missing required field '" + std::string(name) + "'");
        }
        if (!f->is_struct()) {
            throw std::runtime_error("binding: field '" + std::string(name) + "' expected a nested structure");
        }
        bind_from_fields(f->as_struct(), field);
    }
}

template <typename IndexT, config_schema T, std::size_t... Is>
void bind_all_fields(const IndexT& index, T& out, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_field<Is>(index, out, names[Is]), ...);
}

} // namespace detail

// Binds `fields` onto `out` field-by-field, matching each field's own
// (compiler-derived) name against a Field of the same name, case-insensitive.
// Throws std::runtime_error, naming the offending field, on a missing
// required field or a value that doesn't parse as its field's type.
//
// Picks its lookup strategy once per T, at compile time: below
// kLinearScanFieldThreshold fields, detail::LinearFieldScanner (O(1) to
// construct, O(M*N) total for T's M fields against an N-entry FieldList,
// which wins outright for small M/N -- see that constant's comment); at
// or above it, detail::FieldIndex (O(N) to build once, then O(1) average
// per lookup, O(N+M) total). Either way this is the same choice for every
// call of bind_from_fields<T>, including once per element of a
// vector<T> -- the type's own field count decides it, not how many times
// or where it's invoked.
template <config_schema T>
void bind_from_fields(const FieldList& fields, T& out) {
    if constexpr (boost::pfr::tuple_size_v<T> < detail::kLinearScanFieldThreshold) {
        detail::LinearFieldScanner scanner(fields);
        detail::bind_all_fields(scanner, out, std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
    } else {
        detail::FieldIndex index(fields);
        detail::bind_all_fields(index, out, std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
    }
}

// Same as bind_from_fields, but requires flat_schema<T> instead of the more
// permissive config_schema<T>. Use this at call sites where a struct is
// meant to stay strictly flat -- a plain DB row/record shape being the
// common case -- so a nested or vector<U> field added to it later is a
// compile error right here, instead of silently being accepted by the more
// general config_schema (flat_schema is a strict subset of it: every
// flat_schema struct already satisfies config_schema, so this just forwards).
template <flat_schema T>
void bind_flat_fields(const FieldList& fields, T& out) {
    bind_from_fields(fields, out);
}

} // namespace binding
