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

namespace detail {

// `strict` is a runtime bool, deliberately, threaded through every level
// of recursion -- see bind_from_fields's `strict` parameter (the public
// entry point, at the bottom of this file) for why this can't be a
// compile-time flag: that would bake the choice into the compiled
// binary, so relaxing validation for a config with legitimate leftover
// keys would need a recompile, not a config or deployment change. This
// matches how .NET's actual BinderOptions.ErrorOnUnknownConfiguration
// works too -- a runtime property, not a template parameter.
template <config_schema T>
void bind_from_fields_impl(const FieldList& fields, T& out, bool strict);

// The mutable-source overload -- see its own definition, further down,
// for why this is safe (never reached on anything actually const) and
// what it buys (a leaf value moves instead of copies, all the way down
// through bind_one_field/parse_leaf_value).
template <config_schema T>
void bind_from_fields_impl(FieldList& fields, T& out, bool strict);

// Case-insensitive name -> Field* index over one FieldList level, built once
// per bind_from_fields() call rather than re-scanning `fields` linearly for
// every field of T. Binding a T with M fields against an N-entry FieldList
// via a linear scan (the original implementation) is O(M*N); building this
// index costs O(N) once, after which each of T's M lookups is O(1) average,
// for O(N+M) overall. Keeps a vector per name, not a single Field*, because
// a repeated element (see config_field.h) means several entries can
// legitimately share a name.
// FieldPtr is `const Field*` for a read-only source -- the common case,
// used whenever `fields` might reasonably be read again later by some
// other call (bind_from_fields' const-lvalue overload; see its own
// comment for why that has to stay non-destructive) -- or `Field*` for a
// source this index is allowed to move values out of. That's only ever
// safe over a genuinely non-const FieldList, which is exactly what
// bind_from_fields' rvalue overload guarantees: templating on the
// pointer type, rather than always storing const Field*, is what makes
// that guarantee checked by the type system instead of asserted by a
// const_cast somewhere.
template <typename FieldPtr>
class FieldIndexT {
public:
    using SourceRef = std::conditional_t<std::is_const_v<std::remove_pointer_t<FieldPtr>>,
                                          const FieldList&, FieldList&>;

    explicit FieldIndexT(SourceRef fields) {
        by_name_.reserve(fields.size());
        for (auto& f : fields) {
            by_name_[to_lower(f.name)].push_back(&f);
        }
    }

    // The one entry with this name, or nullptr if there are none. Calling
    // this is itself an assertion that the field isn't repeated -- if two
    // or more entries share `name`, that's a genuinely ambiguous config
    // (a duplicate key where at most one was expected), not something to
    // silently resolve by picking whichever happened to come first, so
    // this throws instead. A field that's actually meant to repeat should
    // use all() below, never this.
    FieldPtr single(std::string_view name) const {
        auto it = by_name_.find(to_lower(name));
        if (it == by_name_.end() || it->second.empty()) return nullptr;
        if (it->second.size() > 1) {
            throw std::runtime_error("binding: field '" + std::string(name) +
                                      "' appears " + std::to_string(it->second.size()) +
                                      " times, expected at most one");
        }
        return it->second.front();
    }

    // Every entry with this name, in FieldList order (empty if none). By
    // value rather than by pointer into by_name_, so this has the same
    // signature as LinearFieldScannerT::all() below -- bind_one_field/
    // bind_all_fields are templated on whichever of the (now four, two
    // strategies times two constness modes) concrete types
    // bind_from_fields_impl picks, and need one common interface to call
    // through regardless of which it is.
    std::vector<FieldPtr> all(std::string_view name) const {
        auto it = by_name_.find(to_lower(name));
        return it == by_name_.end() ? std::vector<FieldPtr>{} : it->second;
    }

private:
    std::unordered_map<std::string, std::vector<FieldPtr>> by_name_;
};

using FieldIndex = FieldIndexT<const Field*>;
using MutableFieldIndex = FieldIndexT<Field*>;

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
template <typename FieldPtr>
class LinearFieldScannerT {
public:
    using SourceRef = std::conditional_t<std::is_const_v<std::remove_pointer_t<FieldPtr>>,
                                          const FieldList&, FieldList&>;

    explicit LinearFieldScannerT(SourceRef fields) : fields_(fields) {}

    // Same contract as FieldIndexT::single() above: at most one match is
    // ever legal here, so a second one found while scanning throws rather
    // than being silently ignored in favor of the first.
    FieldPtr single(std::string_view name) const {
        FieldPtr found = nullptr;
        for (auto& f : fields_) {
            if (to_lower(f.name) == to_lower(name)) {
                if (found) {
                    throw std::runtime_error("binding: field '" + std::string(name) +
                                              "' appears more than once, expected at most one");
                }
                found = &f;
            }
        }
        return found;
    }

    std::vector<FieldPtr> all(std::string_view name) const {
        std::vector<FieldPtr> matches;
        for (auto& f : fields_) {
            if (to_lower(f.name) == to_lower(name)) matches.push_back(&f);
        }
        return matches;
    }

private:
    SourceRef fields_;
};

using LinearFieldScanner = LinearFieldScannerT<const Field*>;
using MutableLinearFieldScanner = LinearFieldScannerT<Field*>;

// Parses one raw text value into a bindable leaf value -- the text-source
// path (XML/ptree; also used for a TOML string value, which arrives as
// text same as always). std::string fields just take the raw text; bool
// takes true/false, Y/N, or 1/0 (case-insensitive) rather than going
// through from_chars, which has no bool overload at all --
// std::is_arithmetic_v<bool> is true, so without this branch a bool field
// would pass is_bindable_leaf_v's compile-time check and then fail to
// compile at the from_chars call below, naming neither the field nor why;
// arithmetic fields go through from_chars and reject anything that
// doesn't fully consume the text (a leading-number match like "10abc" ->
// 10 would silently hide a typo in a config file).
// RawRef is a forwarding reference, not a fixed const std::string& --
// when the caller (ultimately bind_from_fields' rvalue overload, via
// parse_leaf_value below) actually owns a movable std::string, `out =
// std::forward<RawRef>(raw)` moves instead of copies. Only the
// std::string branch below does that: it's the only branch that
// consumes `raw` exactly once, so it's the only one a move is safe (and
// meaningful) in -- the bool/arithmetic branches read `raw` normally
// (trim/from_chars/the error message), which works identically
// regardless of RawRef's deduced value category, since none of those
// reads move anything themselves.
template <typename RawRef, typename T>
void parse_leaf_text(RawRef&& raw, T& out, std::string_view field_name) {
    if constexpr (std::is_same_v<T, std::string>) {
        out = std::forward<RawRef>(raw);
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
        static_assert(is_bindable_leaf_v<T>, "parse_leaf_text: unsupported leaf type");
    }
}

// Converts an already-typed TOML scalar (int64_t/double/bool) directly
// into a bindable leaf value -- no text round trip. V is whichever
// LeafValue alternative was actually stored (see config_field.h);
// converting it to a string field is the one direction that still
// produces text, for a caller whose struct field happens to be
// std::string even though the source value was typed.
template <typename T, typename V>
void assign_typed_leaf(const V& v, T& out, std::string_view field_name) {
    if constexpr (std::is_same_v<T, std::string>) {
        if constexpr (std::is_same_v<V, bool>) {
            out = v ? "true" : "false";
        } else {
            out = std::to_string(v);
        }
    } else if constexpr (std::is_same_v<T, bool> || std::is_arithmetic_v<T>) {
        out = static_cast<T>(v);
    } else {
        static_assert(is_bindable_leaf_v<T>, "assign_typed_leaf: unsupported leaf type");
    }
    (void)field_name; // only used in the thrown-error paths of the text branch above
}

// Parses one leaf's value into a bindable leaf value T -- dispatches on
// which LeafValue alternative is actually present: raw text goes through
// parse_leaf_text (from_chars, the bool text rules); an already-typed
// TOML scalar goes through assign_typed_leaf (a direct conversion, no
// text round trip at all -- see LeafValue's own comment in config_field.h
// for why that matters).
//
// LeafRef is a forwarding reference: std::visit propagates its argument's
// value category to the visitor (the active alternative comes out as an
// rvalue reference when `raw` itself is one), so when this is ultimately
// reached from bind_from_fields' rvalue overload, the std::string
// alternative arrives at parse_leaf_text as a genuine rvalue and gets
// moved, not copied -- int64_t/double/bool are cheap to copy regardless,
// so assign_typed_leaf doesn't need (or take) the same treatment.
template <typename LeafRef, typename T>
void parse_leaf_value(LeafRef&& raw, T& out, std::string_view field_name) {
    std::visit([&](auto&& v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, std::string>) {
            parse_leaf_text(std::forward<decltype(v)>(v), out, field_name);
        } else {
            assign_typed_leaf(v, out, field_name);
        }
    }, std::forward<LeafRef>(raw));
}

// Parses one already-resolved leaf Field's value into `out` -- the shared
// tail end for every place a bindable-leaf value gets pulled out of a
// Field*: a plain leaf field, a vector<leaf> field's own elements, and an
// optional<leaf>/optional<vector<leaf>> field's present value, all via
// bind_one_value below instead of each calling parse_leaf_value directly.
//
// std::move is safe here regardless of f's constness: moving from a
// const Field* degrades to an ordinary copy (a std::string's
// move-assignment can't bind to a const rvalue, so overload resolution
// falls back to its copy-assignment instead -- see parse_leaf_text/
// parse_leaf_value's own comments), and only actually moves when f is a
// genuine, non-const Field*.
template <typename FieldPtr, typename T>
void bind_leaf(FieldPtr f, T& out, std::string_view name) {
    if (!f->is_leaf()) {
        throw std::runtime_error("binding: field '" + std::string(name) + "' expected a plain value");
    }
    parse_leaf_value(std::move(f->as_leaf()), out, name);
}

// Binds one already-resolved Field into a target of type T. T is never
// itself a vector here: config_field_predicate only allows a vector<U>
// field's own U to be a leaf or a struct, never another vector, so
// config_schema<Outer> already rejects a vector<vector<T>> field before
// this is ever reached -- there's no third shape to handle. That leaves
// exactly the leaf-or-struct choice bind_one_field itself needs for a
// plain field, which is what lets this one function serve three call
// sites that used to each duplicate it: a plain (non-optional,
// non-vector) field, one element of a vector<T> field, and an
// optional<T> field's present value (once unwrapped).
template <typename FieldPtr, typename T>
void bind_one_value(FieldPtr f, T& out, std::string_view name, bool strict) {
    if constexpr (is_bindable_leaf_v<T>) {
        bind_leaf(f, out, name);
    } else { // nested config_schema struct
        if (!f->is_struct()) {
            throw std::runtime_error("binding: field '" + std::string(name) + "' expected a nested structure");
        }
        bind_from_fields_impl(f->as_struct(), out, strict);
    }
}

template <std::size_t I, typename IndexT, config_schema T>
void bind_one_field(const IndexT& index, T& out, std::string_view name, bool strict) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(out);

    // `auto*`, not `const Field*` -- IndexT may be one of the Mutable*
    // strategies (see FieldIndexT/LinearFieldScannerT's comment), whose
    // first()/all() return plain Field*. Forcing that into a const Field*
    // here would compile fine (Field* converts implicitly) but silently
    // throw away the mutability that's the entire point of this being
    // reachable from bind_from_fields' rvalue overload -- f->as_leaf()
    // would then always pick the const overload regardless.
    if constexpr (is_optional_v<FieldType>) {
        using ValueType = optional_value_t<FieldType>;

        if constexpr (is_vector_v<ValueType>) {
            // optional<vector<U>>: no matches by name at all means the
            // field is absent (nullopt); one or more means present,
            // collected exactly like a plain vector<U> field below. The
            // Field/FieldList model has no way to distinguish "declared
            // with zero elements" from "never declared" -- both are zero
            // matches by name -- so both read back the same way, as
            // nullopt; there's no third state being lost here.
            using ElemType = vector_value_t<ValueType>;
            auto matches = index.all(name);
            if (matches.empty()) {
                field = std::nullopt;
                return;
            }
            ValueType value;
            for (auto* f : matches) {
                ElemType elem{};
                bind_one_value(f, elem, name, strict);
                value.push_back(std::move(elem));
            }
            field = std::move(value);

        } else {
            auto* f = index.single(name);
            if (!f) {
                field = std::nullopt;
                return;
            }
            ValueType value{};
            bind_one_value(f, value, name, strict);
            field = std::move(value);
        }

    } else if constexpr (is_vector_v<FieldType>) {
        using ElemType = vector_value_t<FieldType>;
        field.clear();
        // Every match must actually agree with ElemType's own shape (leaf
        // or struct) -- config_field_predicate only guarantees the
        // *field*'s element type is one of those, not that the parsed
        // data agrees; bind_one_value throws, naming `name`, if it doesn't.
        for (auto* f : index.all(name)) {
            ElemType elem{};
            bind_one_value(f, elem, name, strict);
            field.push_back(std::move(elem));
        }

    } else { // plain leaf, or nested config_schema struct -- bind_one_value
             // picks which via is_bindable_leaf_v<FieldType>
        auto* f = index.single(name);
        if (!f) {
            throw std::runtime_error("binding: missing required field '" + std::string(name) + "'");
        }
        bind_one_value(f, field, name, strict);
    }
}

template <typename IndexT, config_schema T, std::size_t... Is>
void bind_all_fields(const IndexT& index, T& out, std::index_sequence<Is...>, bool strict) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_field<Is>(index, out, names[Is], strict), ...);
}

// Checks that every entry in `fields` has a name matching one of T's own
// declared field names (case-insensitive) -- throws, naming the
// offending entry, on the first one that doesn't. Only ever called when
// `strict` is true (see bind_from_fields_impl below); doesn't itself
// recurse into nested structs' own contents -- that happens naturally
// when bind_one_field's nested-struct/vector<T> branches call
// bind_from_fields_impl on each sub-FieldList with `strict` propagated,
// so each level gets checked against its own type's names.
template <config_schema T>
void check_no_unknown_fields(const FieldList& fields) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    for (const Field& f : fields) {
        bool known = false;
        for (const auto& name : names) {
            if (to_lower(f.name) == to_lower(name)) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw std::runtime_error("binding: field '" + f.name + "' is not recognized by the target struct");
        }
    }
}

template <config_schema T>
void bind_from_fields_impl(const FieldList& fields, T& out, bool strict) {
    if (strict) {
        check_no_unknown_fields<T>(fields);
    }
    // std::conditional_t picks the lookup strategy TYPE at compile time
    // (see kLinearScanFieldThreshold's comment for why -- that part IS
    // still a compile-time choice, since it depends only on T's own
    // field count, never on `strict`), collapsing what would otherwise
    // be two branches -- identical except for which strategy type they
    // construct -- into one.
    using Scanner = std::conditional_t<(boost::pfr::tuple_size_v<T> < kLinearScanFieldThreshold),
                                        LinearFieldScanner, FieldIndex>;
    Scanner scanner(fields);
    bind_all_fields(scanner, out, std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}, strict);
}

// Same as the const-lvalue overload above, but over a genuinely mutable
// `fields` -- only ever reached from bind_from_fields' rvalue overload
// (a temporary FieldList the caller has no further use for), so this
// picks the Mutable* scanner variant, letting leaf values move out
// instead of copy all the way down through bind_one_field/
// parse_leaf_value. Never called on anything actually const: overload
// resolution alone decides which of these two bind_from_fields_impl
// overloads a given call reaches, based on whether the caller's `fields`
// argument is an lvalue or rvalue.
template <config_schema T>
void bind_from_fields_impl(FieldList& fields, T& out, bool strict) {
    if (strict) {
        check_no_unknown_fields<T>(fields);
    }
    using Scanner = std::conditional_t<(boost::pfr::tuple_size_v<T> < kLinearScanFieldThreshold),
                                        MutableLinearFieldScanner, MutableFieldIndex>;
    Scanner scanner(fields);
    bind_all_fields(scanner, out, std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}, strict);
}

// Splits "a.b.c" into ("a", "b.c"); splits "a" (no dot) into ("a", "").
inline std::pair<std::string_view, std::string_view> split_first_path_segment(std::string_view path) {
    const auto dot = path.find('.');
    if (dot == std::string_view::npos) return {path, {}};
    return {path.substr(0, dot), path.substr(dot + 1)};
}

// Finds the Field named `name` in `fields` -- a plain linear scan, since a
// one-off path lookup (get_leaf/try_get_leaf below) doesn't warrant
// building any index over `fields` just to find a single entry.
inline const Field* find_field(const FieldList& fields, std::string_view name) {
    for (const Field& f : fields) {
        if (to_lower(f.name) == to_lower(name)) return &f;
    }
    return nullptr;
}

// Walks `path`'s dot-separated segments through nested structs, returning
// the terminal Field* -- or nullptr if any segment is missing, or an
// intermediate (non-terminal) segment isn't itself a nested struct to
// descend into. Doesn't distinguish which of those two happened, or which
// segment failed; get_leaf/try_get_leaf don't need that distinction, and
// resolve_leaf_path stays a single, simple pass either way.
inline const Field* resolve_leaf_path(const FieldList& fields, std::string_view path) {
    const FieldList* current = &fields;
    std::string_view remaining = path;
    for (;;) {
        auto [segment, rest] = split_first_path_segment(remaining);
        const Field* found = find_field(*current, segment);
        if (!found) return nullptr;
        if (rest.empty()) return found;
        if (!found->is_struct()) return nullptr;
        current = &found->as_struct();
        remaining = rest;
    }
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
//
// `strict`, when true, also rejects any entry in `fields` (at every
// nesting level -- inside a nested struct, and inside each element of a
// vector<T>, not just the top level) whose name doesn't match one of its
// containing struct's own declared fields. Catches a typo'd or stale/
// deprecated config key that would otherwise be silently ignored. This
// is a runtime bool, not a compile-time flag, deliberately: whether an
// old config with legitimate leftover keys should still load has to be a
// deployment/config-time decision (an env var, a CLI flag, a setting in
// the config itself), not something that requires recompiling the
// binary to change -- the same way .NET's actual
// BinderOptions.ErrorOnUnknownConfiguration is a runtime property on the
// binder, not a template parameter or a separately-compiled function.
template <config_schema T>
void bind_from_fields(const FieldList& fields, T& out, bool strict = false) {
    detail::bind_from_fields_impl(fields, out, strict);
}

// Same binding, over a `fields` the caller is handing off rather than
// keeping -- a temporary built inline (e.g. from_toml(tbl), or a
// path-resolved sub-FieldList that's about to be discarded either way),
// not something read again afterward. Every std::string leaf value is
// moved into its target field instead of copied, all the way down
// through bind_one_field/parse_leaf_value -- for a config with many
// string fields, that's real, free savings for exactly the call shape
// this overload exists for. Ordinary overload resolution routes a call
// here automatically for an rvalue `fields` argument; a named FieldList
// variable still goes through the const-lvalue overload above and is
// left completely untouched, so nothing changes for existing callers
// that keep and reuse their FieldList.
template <config_schema T>
void bind_from_fields(FieldList&& fields, T& out, bool strict = false) {
    detail::bind_from_fields_impl(fields, out, strict);
}

// Same as bind_from_fields, but requires flat_schema<T> instead of the more
// permissive config_schema<T>. Use this at call sites where a struct is
// meant to stay strictly flat -- a plain DB row/record shape being the
// common case -- so a nested or vector<U> field added to it later is a
// compile error right here, instead of silently being accepted by the more
// general config_schema (flat_schema is a strict subset of it: every
// flat_schema struct already satisfies config_schema, so this just forwards).
template <flat_schema T>
void bind_flat_fields(const FieldList& fields, T& out, bool strict = false) {
    bind_from_fields(fields, out, strict);
}

// The rvalue counterpart to bind_flat_fields above, same relationship
// the two bind_from_fields overloads have to each other.
template <flat_schema T>
void bind_flat_fields(FieldList&& fields, T& out, bool strict = false) {
    bind_from_fields(std::move(fields), out, strict);
}

// Reads a single scalar value at a dot-separated path (e.g. "pool.size",
// or just "port" for a top-level field), parsed as T -- the escape hatch
// for reading one ad hoc setting where defining a whole config_schema
// struct and calling bind_from_fields() just to read one field would be
// overkill. Each path segment matches case-insensitively, same as
// bind_from_fields(). A plain linear scan per segment, not a FieldIndex --
// a one-off lookup has nothing to amortize an index's construction cost
// against.
//
// Throws std::runtime_error, naming the path, if any segment is missing,
// a non-terminal segment isn't a nested struct, the terminal isn't a leaf
// value, or its text doesn't parse as T -- the same failure modes
// bind_from_fields() has for a required (non-optional) field, for the
// same reason: a missing or malformed ad hoc value is a real config
// problem to fail loudly on, not something to paper over with a default.
template <is_bindable_leaf T>
T get_leaf(const FieldList& fields, std::string_view path) {
    const Field* f = detail::resolve_leaf_path(fields, path);
    if (!f) {
        throw std::runtime_error("binding: missing field (from path '" + std::string(path) + "')");
    }
    if (!f->is_leaf()) {
        throw std::runtime_error("binding: field '" + std::string(path) + "' expected a plain value");
    }
    T value{};
    detail::parse_leaf_value(f->as_leaf(), value, path);
    return value;
}

// Same as get_leaf, but returns std::nullopt instead of throwing when
// `path` itself doesn't resolve to an existing leaf -- for a setting
// that's genuinely allowed to be absent, read ad hoc (the std::optional<T>
// struct-field equivalent of get_leaf, for callers not binding a whole
// struct). A value that IS present but fails to parse as T still throws:
// that's a real data error, not absence, exactly as a std::optional<T>
// field in bind_from_fields() only tolerates the field being missing, not
// being present with garbage in it.
template <is_bindable_leaf T>
std::optional<T> try_get_leaf(const FieldList& fields, std::string_view path) {
    const Field* f = detail::resolve_leaf_path(fields, path);
    if (!f || !f->is_leaf()) return std::nullopt;
    T value{};
    detail::parse_leaf_value(f->as_leaf(), value, path);
    return value;
}

} // namespace binding
