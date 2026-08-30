#pragma once
#include <type_traits>
#include <string_view>
#include <cstddef>
#include <boost/pfr.hpp>

// ============================================================================
// Shared compile-time reflection core, built on boost::pfr.
//
// This is deliberately built only from boost::pfr::tuple_size /
// boost::pfr::tuple_element_t / boost::pfr::for_each_field -- the stable
// primitives that work identically on MSVC, GCC and clang (they rely on
// aggregate init + structured bindings, not on compiler-specific name
// capture). boost::pfr's *field name* reflection (names_of / names_as_array)
// depends on parsing __FUNCSIG__/__PRETTY_FUNCTION__-style compiler output
// and is not consistently available on MSVC, so nothing in this directory
// depends on it -- see oci_client.h, which binds by position instead.
// ============================================================================

namespace binding {

// ----------------------------------------------------------------------------
// Core leaf value validation (arithmetic primitives + string-like layouts).
// ----------------------------------------------------------------------------
template <typename T>
concept is_bindable_leaf =
    !std::is_reference_v<T> &&
    !std::is_const_v<T> &&
    !std::is_pointer_v<T> && (
        std::is_arithmetic_v<T> ||
        std::is_convertible_v<T, std::string_view>
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

} // namespace binding
