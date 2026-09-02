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
// struct_field_auditor/is_bindable_leaf/flat_schema/bindable/config_schema
// -- the "does this struct have the right shape" checks -- are
// built only from boost::pfr::tuple_size / tuple_element_t / for_each_field/
// get, walking fields by position. That's independent of whether field
// *names* are used anywhere -- it's just how boost::pfr enumerates a
// struct's fields at all, on every compiler.
//
// Field *names* (boost::pfr::names_as_array()) were initially avoided
// project-wide, on the assumption that the feature depends on
// __FUNCSIG__/__PRETTY_FUNCTION__-style compiler-specific parsing and so
// wasn't reliably available on MSVC, the actual compiler target this idea
// started from. That assumption turned out to be wrong for the MSVC version
// in question -- boost::pfr has a separate consteval/std::source_location-
// based implementation (see BOOST_PFR_CORE_NAME_ENABLED and
// core_name20_static.hpp) that isn't the __FUNCSIG__-parsing one, and it's
// confirmed working there. So:
//
// - config_bind.h uses names_as_array() to match a config field to a
//   same-named XML element/attribute -- necessary, not just convenient,
//   since a repeated element's several same-named entries can't be matched
//   to a single vector<T> field by position at all.
// - oci_client.h's bind_fields() (execute()'s IN parameters, via
//   OCIBindByName) also now uses names_as_array() -- a real improvement
//   there too: OCIBindByPos's "position" meant occurrence order in the SQL
//   text, which forced a struct's field *declaration* order to match
//   wherever its placeholders happened to land across a statement's
//   clauses. Binding by name removes that coupling entirely.
// - oci_client.h's define_fields() (query()'s output columns) stays
//   positional regardless: OCIDefineByPos is the only column-output bind
//   API in raw OCI -- there is no OCIDefineByName -- so this one is an OCI
//   limitation, not a choice.
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
// structs) and bindable (leaf-or-LOB, for OCI bind/row structs) share this
// same MSVC-safe engine instead of duplicating it.
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
// Predicate: every field is a bindable leaf, OR a nested struct (a plain
// aggregate -- is_bindable_struct_v), OR a std::vector<U> of one (a
// repeated element).
//
// Deliberately shallow: unlike leaf_only_predicate/oci_bindable_predicate,
// this does NOT recursively re-verify a nested struct/vector<U> element's
// *own* fields here. It can't -- a genuinely self-referential shape like
// `struct Node { std::string name; std::vector<Node> children; };` (a tree
// node whose children are the same type) needs config_schema<Node> while
// still computing config_schema<Node>: struct_field_auditor<Node,
// config_field_predicate>::value's "used in its own initializer" is a real,
// immediate compiler error, not a timeout -- confirmed directly against
// this header. There is no way to eagerly compute one compile-time boolean
// that recursively validates a self-referential tree's every level up
// front.
//
// So the deep, per-level check is deferred to where it can actually happen
// lazily: config_bind.h's bind_from_fields<T>() is itself templated on
// config_schema<T>, and its nested-struct/vector<U> handling calls
// bind_from_fields<U> recursively. For a self-referential T (U == T), that
// recursive call reuses the *same* function template instantiation calling
// itself -- ordinary runtime recursion over however deep the actual
// FieldList tree happens to be, not a second compile-time instantiation of
// anything. The cost of this shallowness: a malformed *nested* struct
// (e.g. a field type that isn't itself bindable) is only caught when
// bind_from_fields actually recurses into it, not immediately at a
// `static_assert(config_schema<Outer>)` on the outer type -- still a
// compile error, just a level of indirection further from where you'd see
// it with an eager check.
// ----------------------------------------------------------------------------
struct config_field_predicate {
    template <typename U>
    static constexpr bool check() {
        if constexpr (is_bindable_leaf_v<U>) {
            return true;
        } else if constexpr (is_vector_v<U>) {
            return is_bindable_struct_v<vector_value_t<U>>;
        } else {
            return is_bindable_struct_v<U>;
        }
    }
};

// Public concept: T is a config-shaped struct -- fields are leaves,
// std::optional<leaf> (absent when the field is missing), a nested struct
// (a child element's attributes/children), or std::vector<U> of one
// (repeated child elements) -- including U == T itself, for a tree node
// whose children are the same shape. See config_bind.h for the FieldList ->
// T binder that uses this, and does the actual (lazy, recursive-by-function-
// call) per-level verification.
template <typename T>
concept config_schema = struct_field_auditor<T, config_field_predicate>::value;

} // namespace binding
