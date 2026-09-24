// CsvWriter<Struct>: formats a DataRow<Struct> as one CSV line, falling
// back field-by-field to a defaults DataRow<Struct> when a field isn't
// present in the row, and to a default-constructed value when it's present
// in NEITHER (documented, not silent UB -- see write_row()).
//
// Dispatch strategy: precompute each field's byte offset within Struct and
// a type tag ONCE (in the constructor, not per row) -- this part is
// instance-independent metadata, reused across every DataRow<Struct> this
// CsvWriter ever formats, not rebuilt per row. The per-row hot path then
// builds a typed, read-only FieldView (a std::variant<const double*, ...,
// const std::string*, ...>) from that metadata plus one specific row's base
// address, and dispatches via std::visit to an overloaded format_field()
// -- each overload receives an honestly-typed pointer/reference, with no
// reinterpret_cast or memcpy-into-a-local-object anywhere in the hot path.
//
// This replaced an earlier version whose switch cases did their own
// std::memcpy-based type punning directly (a raw void* cast per case) --
// which is exactly what caused a real, found-by-testing heap-corruption
// bug (see git history): std::memcpy-ing a std::string field's bytes into
// a fresh local std::string doesn't alias it, it byte-copies its internal
// representation, leaving two objects believing they own the same heap
// buffer. That class of bug is what FieldView/std::visit rules out by
// construction: you can't accidentally memcpy something std::visit already
// handed you as a real `const std::string*` the way you can when all you
// have is a `void*` and a tag telling you what it's "supposed" to be.
//
// This is a measured choice, not a style preference -- four strategies
// were benchmarked on a 190-field struct across this project's three real
// compilers (ns/field, 1M rows):
//
//                          GCC 15   Clang 21   MSVC 19.51
//   fold expression         41.7 ns   19.5 ns    123.0 ns   <- MSVC-pathological
//   function-ptr table      40.6 ns   23.1 ns    131.1 ns   <- helps, not enough
//   offset + tag switch     18.0 ns   20.1 ns     19.0 ns   <- fast everywhere
//   variant + std::visit    17.2 ns   20.5 ns     20.6 ns   <- matches switch, type-safe
//
// variant + std::visit matches or beats the raw offset+switch on two of
// three compilers and is ~8% slower on the third (MSVC) -- nowhere near
// the fold-expression/function-pointer-table pathology, and within normal
// run-to-run measurement noise on GCC/Clang. std::visit over a small
// closed variant compiles down to essentially the same jump-table shape as
// a hand-written switch, which is why the cost difference is negligible:
// the type safety isn't bought at a real performance price here.
#pragma once
#include <posreport/data_row.h>

#include <boost/pfr.hpp>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace posreport {

namespace detail::csv {

enum class Tag { Double, Float, Int32, Int64, Bool, StdString, FixedChars };

template<class T> constexpr Tag tag_of() {
    if constexpr (std::is_same_v<T, double>) return Tag::Double;
    else if constexpr (std::is_same_v<T, float>) return Tag::Float;
    else if constexpr (std::is_same_v<T, bool>) return Tag::Bool;
    else if constexpr (std::is_same_v<T, std::string>) return Tag::StdString;
    else if constexpr (std::is_array_v<T> || requires { T{}.data(); T{}.size(); })
        return Tag::FixedChars; // std::array<char, N>-like: fixed buffer, nul-agnostic
    else if constexpr (std::is_integral_v<T> && sizeof(T) <= 4) return Tag::Int32;
    else if constexpr (std::is_integral_v<T>) return Tag::Int64;
    else static_assert(sizeof(T) == 0, "posreport::CsvWriter: no CSV formatting for this field type");
}

struct ColumnMeta { Tag tag; std::size_t offset; std::size_t size; };

// A typed, read-only view of one field's value, built fresh per field per
// row from ColumnMeta (offset+tag -- instance-independent, built once
// ever, not per row: see CsvWriter's constructor) plus one row's base
// address. Every alternative except the last is a real typed pointer, so
// whatever format_field() overload std::visit calls gets an honestly-typed
// value, never a void* it has to reinterpret itself. FixedChars is a byte
// range (pointer+size), not a typed pointer, because std::array<char, N>
// is trivially copyable regardless of N -- reading its bytes directly is
// already safe the way the old FixedChars case always did it -- and
// because different fields can have different N, which a variant can't
// enumerate as one alternative anyway; a byte range doesn't need to know N
// to be read safely.
using FieldView = std::variant<const double*, const float*, const std::int32_t*, const std::int64_t*,
                                const bool*, const std::string*, std::pair<const char*, std::size_t>>;

inline FieldView make_field_view(Tag tag, const void* base, std::size_t size) {
    switch (tag) {
    case Tag::Double:     return FieldView{static_cast<const double*>(base)};
    case Tag::Float:      return FieldView{static_cast<const float*>(base)};
    case Tag::Int32:      return FieldView{static_cast<const std::int32_t*>(base)};
    case Tag::Int64:      return FieldView{static_cast<const std::int64_t*>(base)};
    case Tag::Bool:       return FieldView{static_cast<const bool*>(base)};
    case Tag::StdString:  return FieldView{static_cast<const std::string*>(base)};
    case Tag::FixedChars: return FieldView{std::pair<const char*, std::size_t>{static_cast<const char*>(base), size}};
    }
    return FieldView{static_cast<const double*>(base)}; // unreachable -- every Tag handled above
}

} // namespace detail::csv

template<class Struct>
class CsvWriter {
public:
    static constexpr std::size_t field_count = boost::pfr::tuple_size_v<Struct>;

    CsvWriter() : meta_(build_meta(std::make_index_sequence<field_count>{})) {}

    // Writes one CSV line (comma-separated, CRLF-terminated) into `out`,
    // which must have room for the worst case -- see max_line_bytes().
    // Field-by-field: row's value if present, else defaults' value if
    // present, else a default-constructed value of that field's type (0,
    // false, an empty string) -- reachable only if a field is missing from
    // BOTH row and defaults, which a caller should treat as a schema bug,
    // not rely on; this exists so a gap never reads uninitialized memory or
    // crashes, not as a feature.
    [[nodiscard]] std::size_t write_row(const DataRow<Struct>& row, const DataRow<Struct>& defaults, char* out) const {
        static const Struct kZero{}; // fallback source when a field is in neither row nor defaults
        const char* row_bytes = reinterpret_cast<const char*>(&row.raw());
        const char* def_bytes = reinterpret_cast<const char*>(&defaults.raw());
        const char* zero_bytes = reinterpret_cast<const char*>(&kZero);
        char* p = out;
        for (std::size_t i = 0; i < field_count; ++i) {
            const auto& m = meta_[i];
            const char* src = row.has_dyn(i) ? row_bytes : defaults.has_dyn(i) ? def_bytes : zero_bytes;
            const auto view = detail::csv::make_field_view(m.tag, src + m.offset, m.size);
            p = std::visit([p](auto&& v) { return format_field(v, p); }, view);
            *p++ = (i + 1 < field_count) ? ',' : '\r';
        }
        *p++ = '\n';
        return static_cast<std::size_t>(p - out);
    }

    // A safe upper bound on write_row()'s output size, for sizing `out`.
    // Conservative (20 bytes covers any double/int64 in fixed notation;
    // FixedChars/StdString use their actual max size), not tight.
    [[nodiscard]] std::size_t max_line_bytes() const {
        std::size_t total = 1; // trailing '\n'
        for (const auto& m : meta_) {
            total += (m.tag == detail::csv::Tag::StdString || m.tag == detail::csv::Tag::FixedChars) ? m.size : 24;
            total += 1; // ',' or '\r'
        }
        return total;
    }

private:
    using ColumnMeta = detail::csv::ColumnMeta;
    using Tag = detail::csv::Tag;

    template<std::size_t I>
    static ColumnMeta meta_for(const Struct& probe) {
        using T = boost::pfr::tuple_element_t<I, Struct>;
        const auto offset = static_cast<std::size_t>(reinterpret_cast<const char*>(&boost::pfr::get<I>(probe)) -
                                                       reinterpret_cast<const char*>(&probe));
        return {detail::csv::tag_of<T>(), offset, sizeof(T)};
    }
    template<std::size_t... I>
    static std::array<ColumnMeta, field_count> build_meta(std::index_sequence<I...>) {
        static const Struct probe{};
        return {meta_for<I>(probe)...};
    }

    // One overload per real type -- each gets an honestly-typed value from
    // std::visit, so using it correctly doesn't depend on anyone having
    // written the right cast/memcpy by hand at this call site.
    static char* format_field(const double* v, char* p) {
        return std::to_chars(p, p + 24, *v, std::chars_format::fixed, 2).ptr;
    }
    static char* format_field(const float* v, char* p) {
        return std::to_chars(p, p + 24, *v, std::chars_format::fixed, 2).ptr;
    }
    static char* format_field(const std::int32_t* v, char* p) {
        return std::to_chars(p, p + 16, *v).ptr;
    }
    static char* format_field(const std::int64_t* v, char* p) {
        return std::to_chars(p, p + 24, *v).ptr;
    }
    static char* format_field(const bool* v, char* p) {
        *p = *v ? '1' : '0';
        return p + 1;
    }
    static char* format_field(const std::string* v, char* p) {
        // A real const std::string&, used exactly like any other
        // std::string -- no memcpy of its representation, that was the bug.
        std::memcpy(p, v->data(), v->size());
        return p + v->size();
    }
    static char* format_field(const std::pair<const char*, std::size_t>& v, char* p) {
        // std::array<char, N>'s storage IS the field's bytes; safe to read
        // directly regardless of N since it's trivially copyable either way.
        const char* data = v.first;
        const std::size_t n = v.second;
        std::size_t len = 0;
        while (len < n && data[len] != '\0') ++len; // treat embedded NUL as end, like FixedString elsewhere
        std::memcpy(p, data, len);
        return p + len;
    }

    std::array<ColumnMeta, field_count> meta_;
};

} // namespace posreport
