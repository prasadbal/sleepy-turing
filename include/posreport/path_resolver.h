// A dotted-path expression evaluator for position-report columns: given a
// string like "Position.book", "ins.expiry", or "db.Desks.description", and
// a Position plus a set of resolvers (how to look up an Instrument, a db
// column, a config value), returns the value it names.
//
// Ordinary field access (Position.book, ins.isin, ...) never needs a
// hand-written getter: it goes through boost::pfr's NAME reflection
// (names_as_array/get_name), so any field on Position or Instrument is
// automatically addressable by its declared C++ name. Confirmed working on
// GCC 15, Clang 21 and MSVC 19.51 (this project's three real compilers)
// before building on it -- PFR's name reflection is the least battle-tested
// part of the library and worth checking, not assuming.
//
// Only the ROOT TRANSITIONS need real code, because they aren't plain field
// access:
//   Position.<field>     PFR field lookup on Position
//   ins.<field>           follows Position's instrument reference (however
//                          that's modeled -- here, sicovam -> Instrument)
//                          and PFR field lookup on the result
//   db.<Table>.<column>   not a struct at all -- Table/column are literal
//                          strings handed to a db lookup function
//   Config.<key>          a string-keyed config lookup
//
// Grammar is deliberately minimal, matching exactly what was asked for --
// dotted paths, nothing else:
//   path := IDENT ('.' IDENT)*
// No operators, literals or function calls yet. Extending later (a
// default(...) wrapper, a type suffix) means adding a case here, not
// redesigning -- the field-access core (PFR name lookup) doesn't change.
#pragma once
#include <boost/pfr.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace posreport {

using Value = std::variant<std::monostate, std::string, std::int64_t, double, bool>;

namespace detail {
template<class T>
Value to_value(const T& v) {
    if constexpr (std::is_same_v<T, std::string> || std::is_convertible_v<T, std::string_view>)
        return Value{std::string(v)};
    else if constexpr (std::is_same_v<T, bool>) // checked before is_integral_v<T>: bool IS integral in C++,
        return Value{v};                        // and would otherwise silently become an int64_t 0/1.
    else if constexpr (std::is_floating_point_v<T>)
        return Value{static_cast<double>(v)};
    else if constexpr (std::is_integral_v<T>)
        return Value{static_cast<std::int64_t>(v)};
    else
        static_assert(sizeof(T) == 0, "posreport: no Value conversion for this field type");
}
} // namespace detail

// Field `name`'s value on `obj`, or nullopt if T has no field by that name.
// The whole point: no per-field getter anywhere -- PFR's compile-time name
// list is walked once per call (a handful of string compares; the schemas
// this is for are dozens of fields, not thousands, so this is not the part
// worth optimizing) to find the matching index, then pfr::get<I> reads it.
template<class T>
std::optional<Value> field_by_name(const T& obj, std::string_view name) {
    std::optional<Value> result;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((boost::pfr::get_name<I, T>() == name ? (result = detail::to_value(boost::pfr::get<I>(obj)), void()) : void()), ...);
    }(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
    return result;
}

// The four roots' resolution behavior, supplied by the caller. Position
// itself is passed to evaluate() directly, not stored here, since it's
// per-row; these are the things that don't change row to row (or that need
// I/O -- a db lookup, an instrument-by-id lookup).
template<class Position, class Instrument>
struct Resolvers {
    std::function<std::optional<Instrument>(const Position&)>                      instrument_of;
    std::function<std::optional<Value>(std::string_view table, std::string_view column, const Position&)> db_lookup;
    std::function<std::optional<Value>(std::string_view key)>                       config_lookup;
};

inline std::vector<std::string_view> split_path(std::string_view expr) {
    std::vector<std::string_view> segs;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= expr.size(); ++i) {
        if (i == expr.size() || expr[i] == '.') {
            if (i == start) return {}; // empty segment: "a..b", leading/trailing '.'
            segs.push_back(expr.substr(start, i - start));
            start = i + 1;
        }
    }
    return segs;
}

template<class Position, class Instrument>
std::optional<Value> evaluate(std::string_view expr, const Position& pos, const Resolvers<Position, Instrument>& res) {
    auto segs = split_path(expr);
    if (segs.empty()) return std::nullopt;

    if (segs[0] == "Position") segs.erase(segs.begin());
    if (segs.empty()) return std::nullopt; // "Position" alone names no value

    if (segs[0] == "ins") {
        if (segs.size() != 2 || !res.instrument_of) return std::nullopt;
        const auto ins = res.instrument_of(pos);
        return ins ? field_by_name(*ins, segs[1]) : std::nullopt;
    }
    if (segs[0] == "db") {
        if (segs.size() != 3 || !res.db_lookup) return std::nullopt;
        return res.db_lookup(segs[1], segs[2], pos);
    }
    if (segs[0] == "Config") {
        if (segs.size() != 2 || !res.config_lookup) return std::nullopt;
        return res.config_lookup(segs[1]);
    }
    if (segs.size() != 1) return std::nullopt; // e.g. "Position.book.extra" -- book has no children
    return field_by_name(pos, segs[0]);
}

} // namespace posreport
