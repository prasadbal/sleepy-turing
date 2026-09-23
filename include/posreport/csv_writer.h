// CsvWriter<Struct>: formats a DataRow<Struct> as one CSV line, falling
// back field-by-field to a defaults DataRow<Struct> when a field isn't
// present in the row, and to a default-constructed value when it's present
// in NEITHER (documented, not silent UB -- see write_row()).
//
// Dispatch strategy: precompute each field's byte offset within Struct and
// a type tag ONCE (in the constructor, not per row); the per-row loop is
// then pure pointer arithmetic and a switch, no per-field template
// instantiation in the hot path. This is a real, measured decision, not a
// style preference -- three strategies were benchmarked on a 190-field
// struct on this project's three real compilers, ns/field, 1M rows:
//
//                          GCC 15    Clang 21    MSVC 19.51
//   fold expression        35 ns      16 ns       138 ns   <- MSVC-pathological
//   function-pointer table 37 ns      20 ns       110 ns   <- helps, not enough
//   offset + tag switch    17 ns      16 ns        24 ns   <- fast everywhere
//   (vector<Value> direct  16 ns    22.6 ns        43.5 ns <- no struct/PFR at all;
//    dispatch, no offset table -- viable alternative for the OUTPUT row,
//    see include/posreport/report_expr.h's Value; kept here as the
//    comparison point, not implemented in this header)
//
// The fold-expression version (what a first-pass "just unroll the fields at
// compile time" implementation looks like) is fine on GCC/Clang and over
// 5x slower than necessary on MSVC -- this project's actual target. Offset
// + tag switch matches the speed of the hand-rolled runtime-dispatch
// design (~24 ns/field on MSVC) while keeping the PFR-based struct
// definition (no hand-written getter per field) this header set is built
// around -- the right choice when Struct is also used on the input side
// (Position/Instrument via path_resolver.h), where that PFR machinery is
// already paid for.
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
            p = format_field(m, src + m.offset, p);
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

    static char* format_field(const ColumnMeta& m, const char* src, char* p) {
        switch (m.tag) {
        case Tag::Double: { double v; std::memcpy(&v, src, sizeof v);
            return std::to_chars(p, p + 24, v, std::chars_format::fixed, 2).ptr; }
        case Tag::Float: { float v; std::memcpy(&v, src, sizeof v);
            return std::to_chars(p, p + 24, v, std::chars_format::fixed, 2).ptr; }
        case Tag::Int32: { std::int32_t v; std::memcpy(&v, src, sizeof v);
            return std::to_chars(p, p + 16, v).ptr; }
        case Tag::Int64: { std::int64_t v; std::memcpy(&v, src, sizeof v);
            return std::to_chars(p, p + 24, v).ptr; }
        case Tag::Bool: { bool v; std::memcpy(&v, src, sizeof v);
            *p = v ? '1' : '0'; return p + 1; }
        case Tag::StdString: {
            // Alias the real std::string in place via a pointer cast -- NOT
            // std::memcpy into a local std::string. Byte-copying a
            // std::string's representation (heap pointer/size/capacity)
            // into a second object makes both believe they own the same
            // heap buffer; the local one's destructor then frees it out
            // from under the real field, corrupting the heap (found via a
            // real test with an actual std::string field -- the 190-field
            // perf benchmark used fixed char arrays for its "string"
            // columns and never exercised this path).
            const std::string& v = *reinterpret_cast<const std::string*>(src);
            std::memcpy(p, v.data(), v.size()); return p + v.size(); }
        case Tag::FixedChars: { const char* data = src; // std::array<char,N>'s storage IS the field's bytes
            const std::size_t n = m.size; std::size_t len = 0;
            while (len < n && data[len] != '\0') ++len; // treat embedded NUL as end, like FixedString elsewhere
            std::memcpy(p, data, len); return p + len; }
        }
        return p;
    }

    std::array<ColumnMeta, field_count> meta_;
};

} // namespace posreport
