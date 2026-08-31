#pragma once
#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <boost/pfr.hpp>

#include "binding/field_tree.h"
#include "binding/reflect.h"

// ============================================================================
// Binds a parser-independent FieldList (see field_tree.h) onto a
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

inline bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

template <config_schema T>
void bind_from_fields(const FieldList& fields, T& out);

namespace detail {

inline const Field* find_field(const FieldList& fields, std::string_view name) {
    for (const auto& f : fields) {
        if (iequals(f.name, name)) return &f;
    }
    return nullptr;
}

// Parses one leaf's raw string into a bindable leaf value. std::string
// fields just take the raw text; arithmetic fields go through from_chars
// and reject anything that doesn't fully consume the text (a leading-number
// match like "10abc" -> 10 would silently hide a typo in a config file).
template <typename T>
void parse_leaf_value(const std::string& raw, T& out, std::string_view field_name) {
    if constexpr (std::is_same_v<T, std::string>) {
        out = raw;
    } else if constexpr (std::is_arithmetic_v<T>) {
        const auto* begin = raw.data();
        const auto* end = raw.data() + raw.size();
        auto [ptr, ec] = std::from_chars(begin, end, out);
        if (ec != std::errc{} || ptr != end) {
            throw std::runtime_error("binding: field '" + std::string(field_name) +
                                      "' value '" + raw + "' is not a valid number");
        }
    } else {
        static_assert(is_bindable_leaf_v<T>, "parse_leaf_value: unsupported leaf type");
    }
}

template <std::size_t I, config_schema T>
void bind_one_field(const FieldList& fields, T& out, std::string_view name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(out);

    if constexpr (is_optional_v<FieldType>) {
        using ValueType = optional_value_t<FieldType>;
        const Field* f = find_field(fields, name);
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
        for (const auto& f : fields) {
            if (!iequals(f.name, name)) continue;
            if (!f.is_struct()) {
                throw std::runtime_error("binding: field '" + std::string(name) + "' expected a nested structure");
            }
            ElemType elem{};
            bind_from_fields(f.as_struct(), elem);
            field.push_back(std::move(elem));
        }

    } else if constexpr (is_bindable_leaf_v<FieldType>) {
        const Field* f = find_field(fields, name);
        if (!f) {
            throw std::runtime_error("binding: missing required field '" + std::string(name) + "'");
        }
        if (!f->is_leaf()) {
            throw std::runtime_error("binding: field '" + std::string(name) + "' expected a plain value");
        }
        parse_leaf_value(f->as_leaf(), field, name);

    } else { // nested config_schema struct
        const Field* f = find_field(fields, name);
        if (!f) {
            throw std::runtime_error("binding: missing required field '" + std::string(name) + "'");
        }
        if (!f->is_struct()) {
            throw std::runtime_error("binding: field '" + std::string(name) + "' expected a nested structure");
        }
        bind_from_fields(f->as_struct(), field);
    }
}

template <config_schema T, std::size_t... Is>
void bind_all_fields(const FieldList& fields, T& out, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_field<Is>(fields, out, names[Is]), ...);
}

} // namespace detail

// Binds `fields` onto `out` field-by-field, matching each field's own
// (compiler-derived) name against a Field of the same name, case-insensitive.
// Throws std::runtime_error, naming the offending field, on a missing
// required field or a value that doesn't parse as its field's type.
template <config_schema T>
void bind_from_fields(const FieldList& fields, T& out) {
    detail::bind_all_fields(fields, out, std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

} // namespace binding
