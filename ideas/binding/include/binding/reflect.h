#pragma once
#include <type_traits>
#include <string_view>
#include <cstddef>
#include <optional>
#include <vector>
#include <boost/pfr.hpp>

// ============================================================================
// Shared compile-time reflection core, built on boost::pfr.
//
// The position-based half of this (struct_field_auditor, is_bindable_leaf,
// flat_schema/oci_row_schema) is deliberately built only from
// boost::pfr::tuple_size / tuple_element_t / for_each_field/get -- the
// stable primitives that work identically on MSVC, GCC and clang (they rely
// on aggregate init + structured bindings, not on compiler-specific name
// capture). oci_client.h binds/defines by field position for exactly this
// reason.
//
// config_schema (below) is the one place that breaks that rule on purpose:
// nested/repeated config structs (see field_tree.h) need to match a struct
// field to a same-named XML element or attribute wherever it appears, which
// position alone can't express once repeated elements are involved. That
// needs boost::pfr::names_as_array()'s field-name reflection, which is the
// __FUNCSIG__/__PRETTY_FUNCTION__-parsing feature the paragraph above avoids
// -- config_bind.h's use of it is a deliberate, confirmed-working choice for
// config binding specifically, not a change of position on the OCI side.
// ============================================================================

namespace binding {

// ----------------------------------------------------------------------------
// std::optional<U> recognition. A nullable column/config value is modeled as
// std::optional<U> where U is itself a bindable leaf -- an empty optional
// means SQL NULL (or "absent" for config), a set one means U's value.
// ----------------------------------------------------------------------------
template <typename T>
struct is_optional_impl : std::false_type {};
template <typename U>
struct is_optional_impl<std::optional<U>> : std::true_type {};

template <typename T>
inline constexpr bool is_optional_v = is_optional_impl<std::remove_cv_t<T>>::value;

template <typename T>
struct optional_value_impl { using type = void; };
template <typename U>
struct optional_value_impl<std::optional<U>> { using type = U; };

// The U in std::optional<U>; only meaningful when is_optional_v<T> is true.
template <typename T>
using optional_value_t = typename optional_value_impl<std::remove_cv_t<T>>::type;

// ----------------------------------------------------------------------------
// Core leaf value validation (arithmetic primitives + string-like layouts,
// optionally wrapped in std::optional to mark the column/value nullable).
// ----------------------------------------------------------------------------
template <typename T>
concept is_bindable_leaf =
    !std::is_reference_v<T> &&
    !std::is_const_v<T> &&
    !std::is_pointer_v<T> && (
        std::is_arithmetic_v<T> ||
        std::is_convertible_v<T, std::string_view> ||
        (is_optional_v<T> && std::is_arithmetic_v<optional_value_t<T>>) ||
        (is_optional_v<T> && std::is_convertible_v<optional_value_t<T>, std::string_view>)
    );

// Hides the concept behind a plain bool so callers (and fold expressions)
// never need to fold a concept directly -- MSVC's parser has had bugs here.
template <typename T>
inline constexpr bool is_bindable_leaf_v = is_bindable_leaf<T>;

// ----------------------------------------------------------------------------
// Parent struct guard: T must be a plain aggregate class type.
// ----------------------------------------------------------------------------
template <typename T>
inline constexpr bool is_bindable_struct_v =
    std::is_class_v<T> &&
    std::is_aggregate_v<T>;

// ----------------------------------------------------------------------------
// Generic compile-time field auditor.
//
// Walks every field of T (via boost::pfr) and checks it against `Predicate`,
// a type exposing `template <typename U> static constexpr bool check()`.
// Parameterizing on Predicate is what lets flat_schema (leaf-only, for config
// structs) and oci_row_schema (leaf-or-LOB, for OCI bind/row structs) share
// this same MSVC-safe engine instead of duplicating it.
//
// The `bool IsStruct` non-type parameter (rather than an `if constexpr`
// inside a single template) is what keeps this parseable on MSVC: folding a
// concept directly, or branching with `if constexpr` around a boost::pfr
// call on a non-aggregate T, both trip known MSVC front-end bugs.
// ----------------------------------------------------------------------------
template <typename T, typename Predicate, bool IsStruct = is_bindable_struct_v<T>>
struct struct_field_auditor {
    static constexpr bool value = false;
};

template <typename T, typename Predicate>
struct struct_field_auditor<T, Predicate, true> {
private:
    static constexpr std::size_t fields_count = boost::pfr::tuple_size_v<T>;

    // Pulled out of verify()'s fold expression on purpose: a nested-template
    // expression ending in ">>" sitting directly next to the fold's "&&" is
    // a known MSVC template-parser weak spot (consecutive '>' closing two
    // angle-bracket lists, immediately followed by the fold operator, has
    // been mis-tokenized on some MSVC toolsets even though the standard
    // mandates ">>" split into two '>' there). Folding over a plain function
    // call instead avoids the shape entirely.
    template <std::size_t I>
    static constexpr bool check_field() noexcept {
        using field_t = boost::pfr::tuple_element_t<I, T>;
        return Predicate::template check<field_t>();
    }

    template <std::size_t... Is>
    static constexpr bool verify(std::index_sequence<Is...>) noexcept {
        return (check_field<Is>() && ...);
    }

public:
    static constexpr bool value = verify(std::make_index_sequence<fields_count>{});
};

// ----------------------------------------------------------------------------
// Predicate: every field must be a plain bindable leaf.
// ----------------------------------------------------------------------------
struct leaf_only_predicate {
    template <typename U>
    static constexpr bool check() { return is_bindable_leaf_v<U>; }
};

// Public concept: T is a flat struct made entirely of bindable leaves.
// Use this for config-section structs (and any vector<T> of them for
// repeated sections) -- no pointers, no const members, no nested structs.
template <typename T>
concept flat_schema = struct_field_auditor<T, leaf_only_predicate>::value;

// ----------------------------------------------------------------------------
// std::vector<U> recognition -- the "repeated element" case for config_schema
// below (see field_tree.h: a repeated XML element isn't a distinct Field
// variant, just several same-named entries, so the struct side needs to know
// which of its fields are meant to collect all of them).
// ----------------------------------------------------------------------------
template <typename T>
struct is_vector_impl : std::false_type {};
template <typename U>
struct is_vector_impl<std::vector<U>> : std::true_type {};

template <typename T>
inline constexpr bool is_vector_v = is_vector_impl<std::remove_cv_t<T>>::value;

template <typename T>
struct vector_value_impl { using type = void; };
template <typename U>
struct vector_value_impl<std::vector<U>> { using type = U; };

// The U in std::vector<U>; only meaningful when is_vector_v<T> is true.
template <typename T>
using vector_value_t = typename vector_value_impl<std::remove_cv_t<T>>::type;

// ----------------------------------------------------------------------------
// Predicate: every field is a bindable leaf, OR a nested config_schema
// struct, OR a std::vector<U> of a nested config_schema struct (a repeated
// element). Recurses through struct_field_auditor itself rather than through
// the config_schema concept directly, for the same reason check_field()
// folds over a function call instead of a raw expression -- keeping the
// recursive step a plain function body, not something folded/expanded
// inline, avoids giving MSVC's template parser another nested-angle-bracket
// shape to trip on.
// ----------------------------------------------------------------------------
struct config_field_predicate {
    template <typename U>
    static constexpr bool check() {
        if constexpr (is_bindable_leaf_v<U>) {
            return true;
        } else if constexpr (is_vector_v<U>) {
            return struct_field_auditor<vector_value_t<U>, config_field_predicate>::value;
        } else if constexpr (is_bindable_struct_v<U>) {
            return struct_field_auditor<U, config_field_predicate>::value;
        } else {
            return false;
        }
    }
};

// Public concept: T is a config-shaped struct -- fields are leaves,
// std::optional<leaf> (absent when the field is missing), nested
// config_schema structs (a child element's attributes/children), or
// std::vector<U> of a nested config_schema struct (repeated child
// elements). See config_bind.h for the FieldList -> T binder that uses this.
template <typename T>
concept config_schema = struct_field_auditor<T, config_field_predicate>::value;

} // namespace binding
