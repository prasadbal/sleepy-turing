#pragma once
// The reflection layer ideas/binding has (oci_client.h there) and
// db/oracle was missing until now: execute()/select_rows()/select()/
// insert_rows(), each walking a plain C++ struct via boost::pfr the same
// way ideas/binding's do. The difference is what they're built on top
// of -- ideas/binding's version talks to raw OCI calls directly; this
// one drives marketlib::db::oracle::OciStatement (oci_statement.h), so it gets that
// class's state checking, call_oci-based error retrieval, and the
// OCI_ATTR_STMT_STATE-driven lifecycle for free, all in one place,
// instead of duplicating any of it here.
//
// LOB handling is the one place the field-level *type* story is
// identical to ideas/binding (OciClob/OciBlob, plain std::string/vector
// value types -- see oci_lob.h) but the *plumbing* underneath it isn't:
// ideas/binding hand-rolls a locator's allocate/write-or-read/free
// lifecycle inline in bind_one_param/define_one_column/apply_one_column
// (details/oci_client.h there). Here, that lifecycle is OCILob's own
// job -- the staging slot for a LOB field is an OCILob object (or a
// std::vector<OCILob>, one per batch row, on the fetch side), and this
// file's own bind/define/apply code just calls its create_temporary()/
// write()/read() methods and lets its destructor free the locator, never
// touching OCIDescriptorAlloc/OCILobWrite2/OCILobRead2/
// OCILobFreeTemporary directly itself.
//
// Same deliberately narrow scope as ideas/binding's version: arithmetic
// fields, FixedString<N>, OciDate, OciClob/OciBlob, each optionally
// wrapped in std::optional<U> to mark it nullable -- except OciClob/
// OciBlob themselves (not nullable yet, same reason as ideas/binding:
// see oci_lob.h). LOB fields don't participate in insert_rows()'s
// array-bind path either, for the same reason as there: no fixed-stride
// buffer for a locator's per-value lifecycle to stride over.
//
// No retry: every entry point runs once and returns an ExecResult.
// A ConnectionLost result is the caller's cue to reconnect and call the
// same entry point again -- nothing here does that automatically.
//
// Implementation in details/oci_client.h.
#include <db/oracle/oci_datetime.h>
#include <db/oracle/oci_fixed_string.h>
#include <db/oracle/oci_lob.h>
#include <db/oracle/oci_statement.h>

#include <boost/pfr.hpp>
#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace marketlib::db::oracle {

// ----------------------------------------------------------------------------
// optional_value_t<U>: void if U isn't std::optional<something>, else the
// wrapped type -- boost::pfr walks plain structs, so this file needs its
// own small trait rather than pulling in ideas/binding's reflect.h.
// ----------------------------------------------------------------------------
template <typename U> struct optional_value { using type = void; };
template <typename U> struct optional_value<std::optional<U>> { using type = U; };
template <typename U> using optional_value_t = typename optional_value<U>::type;
template <typename U> inline constexpr bool is_optional_v = !std::is_void_v<optional_value_t<U>>;

// ----------------------------------------------------------------------------
// reflective_struct<T>: T supplies its own field names directly, as an
// explicit static field_names() -- an alternative to boost::pfr's own name
// derivation, which works by parsing each compiler's pretty-function output
// (__PRETTY_FUNCTION__/__FUNCSIG__). That's real, working reflection, but
// more compiler-specific than the structured-bindings-based mechanism
// boost::pfr::get<I>/tuple_size_v use for *value* access -- every T here
// still goes through that mechanism regardless of whether it's a
// reflective_struct, only *name* derivation is affected. A codegen'd
// struct, generated straight from a query's known XML field names, has no
// reason to make the compiler re-derive what its own generator already
// knows for certain -- it emits field_names() directly instead.
// ----------------------------------------------------------------------------
template <typename T>
concept reflective_struct = requires(std::size_t i) {
    { T::field_names().size() } -> std::convertible_to<std::size_t>;
    { T::field_names()[i] } -> std::convertible_to<std::string_view>;
};

// The field-name list for T: T::field_names() for a reflective_struct,
// boost::pfr::names_as_array<T>() (ordinary compiler-derived reflection)
// for everything else. Every name-based lookup in details/oci_client.h
// goes through this one dispatch point, so a plain hand-written struct's
// behavior is completely unchanged.
template <typename T>
constexpr auto field_names_of() {
    if constexpr (reflective_struct<T>) {
        return T::field_names();
    } else {
        return boost::pfr::names_as_array<T>();
    }
}

// ----------------------------------------------------------------------------
// Compile-time OCI external type code for a scalar field. std::optional<U>
// takes U's type code -- the indicator, not the type code, is what tells
// OCI a value is NULL.
// ----------------------------------------------------------------------------
template <typename T> struct OciTypeBinder {
    static_assert(sizeof(T) == 0,
                  "db: no OCI type code for this field type -- oci_client.h only supports "
                  "arithmetic fields (or optional<arithmetic> for a nullable one)");
};
template <> struct OciTypeBinder<short>              { static constexpr ub2 type_code = SQLT_INT; };
template <> struct OciTypeBinder<int>                { static constexpr ub2 type_code = SQLT_INT; };
template <> struct OciTypeBinder<long>               { static constexpr ub2 type_code = SQLT_INT; };
template <> struct OciTypeBinder<long long>          { static constexpr ub2 type_code = SQLT_INT; };
template <> struct OciTypeBinder<unsigned short>     { static constexpr ub2 type_code = SQLT_UIN; };
template <> struct OciTypeBinder<unsigned int>       { static constexpr ub2 type_code = SQLT_UIN; };
template <> struct OciTypeBinder<unsigned long>      { static constexpr ub2 type_code = SQLT_UIN; };
template <> struct OciTypeBinder<unsigned long long> { static constexpr ub2 type_code = SQLT_UIN; };
template <> struct OciTypeBinder<float>              { static constexpr ub2 type_code = SQLT_BFLOAT; };
template <> struct OciTypeBinder<double>             { static constexpr ub2 type_code = SQLT_BDOUBLE; };
template <std::size_t N> struct OciTypeBinder<FixedString<N>> { static constexpr ub2 type_code = SQLT_CHR; };
template <> struct OciTypeBinder<OciDate> { static constexpr ub2 type_code = SQLT_ODT; };
template <> struct OciTypeBinder<OciClob> { static constexpr ub2 type_code = SQLT_CLOB; };
template <> struct OciTypeBinder<OciBlob> { static constexpr ub2 type_code = SQLT_BLOB; };

template <typename T> struct oci_type_code_of { static constexpr ub2 value = OciTypeBinder<T>::type_code; };
template <typename U> struct oci_type_code_of<std::optional<U>> { static constexpr ub2 value = OciTypeBinder<U>::type_code; };
template <typename T> inline constexpr ub2 oci_type_code_v = oci_type_code_of<std::remove_cv_t<T>>::value;

// ----------------------------------------------------------------------------
// A struct usable as a bind-parameter or result-row type here: every field
// is arithmetic, FixedString<N>, OciDate, OciClob/OciBlob, or
// std::optional<U> of the first three to mark it nullable. Checked directly
// against boost::pfr's own tuple_element_t rather than a shared field-
// walker helper (ideas/binding's reflect.h, MSVC-safe struct_field_auditor)
// -- not pulled in here since nothing else in db/oracle needs it yet.
// ----------------------------------------------------------------------------
namespace detail {
template <typename U>
inline constexpr bool is_scalar_bindable_field_v =
    std::is_arithmetic_v<optional_value_t<U>> || std::is_arithmetic_v<U> ||
    is_fixed_string_v<U> || // deliberately not is_fixed_string_v<optional_value_t<U>> too -- see
                             // details/oci_client.h's bind_one_param/define_one_column: a plain
                             // FixedString<N> is never wrapped in std::optional<> here. Oracle
                             // can't store an empty VARCHAR2/CHAR distinct from NULL, so a plain
                             // FixedString<N> field is already nullable via its own length()==0 --
                             // std::optional<FixedString<N>> would be a second, redundant way to
                             // say the same thing (nullopt vs. an empty string), so it's excluded
                             // the same way is_oci_lob_v<optional_value_t<U>> is excluded below.
    is_oci_date_v<optional_value_t<U>> || is_oci_date_v<U> ||
    is_oci_lob_v<U>; // deliberately not is_oci_lob_v<optional_value_t<U>> too -- see oci_client.h's file
                      // comment and oci_lob.h: a nullable LOB isn't wired in yet.

template <typename T, std::size_t... I>
constexpr bool scalar_bindable_impl(std::index_sequence<I...>) {
    return (is_scalar_bindable_field_v<boost::pfr::tuple_element_t<I, T>> && ...);
}
} // namespace detail

template <typename T>
concept scalar_bindable = requires { boost::pfr::tuple_size_v<T>; } &&
    detail::scalar_bindable_impl<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});

// ----------------------------------------------------------------------------
// execute() -- no bind parameters. DDL, or DML that's fully literal in the
// text. No fetch: a statement of this shape returns no rows.
// ----------------------------------------------------------------------------
ExecResult execute(OciConnection& conn, const std::string& sql);

// execute() with a bind-parameter struct -- DML with named parameters
// (":field_name", bound by the field's own compiler-derived name, any order,
// any number of times it appears in the text). A field declared
// std::optional<U> binds SQL NULL when empty. Logged automatically via
// OciStatement's own set_statement_logger() when one is installed -- every
// bindName() call this makes logs its own value, so there's no separate
// query-logging machinery in this file the way ideas/binding needs.
template <scalar_bindable T>
ExecResult execute(OciConnection& conn, const std::string& sql, T& params);

// ----------------------------------------------------------------------------
// select_rows() -- runs a query and fetches its rows in batches, calling
// `on_batch` once per batch with a pointer to (up to) fetch_batch_size rows
// and how many of them are actually valid (the last batch of a result set is
// usually partial). Columns are matched to OutT's fields *by name*, via
// OciStatement::describeColumnPosition() (falling back to declaration order
// when the backend can't describe at all -- true of the mock, never true of
// a real Oracle result set with at least one column). A field with no
// matching column name is a QueryError, not a silent fetch of whatever
// happened to be at some position.
//
// prefetch_rows and fetch_batch_size are two separate numbers for the same
// reason as ideas/binding: prefetch_rows is OciStatement::set_prefetch_rows
// (Oracle's own client-side round-trip batching); fetch_batch_size is how
// many rows this code asks for per OciStatement::fetch() call.
template <scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT* rows, std::size_t count)>& on_batch);

// Same as above, but also binds `input`'s fields as named IN parameters
// first (e.g. a WHERE clause) -- the read-side counterpart to
// execute(conn, sql, params).
template <scalar_bindable InT, scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql, InT& input,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT* rows, std::size_t count)>& on_batch);

// A field type usable as a bare positional output argument to select()
// below -- no std::optional<U> and no LOB, same restriction and same
// reason as ideas/binding: an empty optional has no address to define
// into, and a LOB needs its own locator lifecycle, not a raw address.
template <typename T>
concept positional_bindable = std::is_arithmetic_v<T> || is_fixed_string_v<T> || is_oci_date_v<T>;

// select() -- a struct-free, single-row fetch: each argument is an output
// reference, bound positionally. See ideas/binding's oci_client.h for the
// full rationale (identical here) -- e.g.
//   long long count;
//   auto r = marketlib::db::oracle::select(conn, "SELECT COUNT(*) FROM t", count);
template <positional_bindable... T>
ExecResult select(OciConnection& conn, const std::string& sql, T&... outputs);

// ----------------------------------------------------------------------------
// select_generic() -- no struct, no reflection, no type mapping at all:
// describes `sql`'s result columns via OciStatement::describeColumns()
// and defines each one using its own described OCI type and size,
// completely unconverted (a NUMBER column stays SQLT_NUM, raw
// Oracle-internal bytes; a VARCHAR2/CHAR column stays SQLT_CHR/SQLT_AFC,
// raw bytes plus a real per-row content length). For a caller that
// doesn't know a row shape ahead of time, or wants to measure this
// layer's raw fetch throughput without any decode/convert step between
// OCI and the callback -- select_rows<T>() decodes into real C++ types
// as it goes; this hands back exactly what Oracle described, nothing
// more.
//
// LOB columns (SQLT_CLOB/SQLT_BLOB) are not supported: a locator is not
// a flat byte buffer the way every other described type here is. A
// query that selects one returns QueryError immediately, before any
// fetch is attempted, rather than defining garbage -- use
// select_rows<T>() with an OciClob/OciBlob field for a LOB column
// instead.
struct GenericBatch {
    const std::vector<ColumnInfo>& columns;
    std::size_t row_count;
    // One entry per column: raw bytes for up to fetch_batch_size rows,
    // each row exactly columns[i].data_size bytes apart (tightly packed,
    // one buffer per column -- not an array of a shared row struct).
    const std::vector<std::vector<unsigned char>>& column_data;
    // One entry per column, one sb2 per row (OCI_IND_NULL/OCI_IND_NOTNULL).
    const std::vector<std::vector<sb2>>& indicators;
    // One entry per column, one ub2 per row -- only meaningful for a
    // SQLT_CHR/SQLT_AFC column (the real fetched length; that data isn't
    // null-terminated). 0 and unused for every other column type.
    const std::vector<std::vector<ub2>>& lengths;
};
using GenericBatchCallback = std::function<void(const GenericBatch&)>;

ExecResult select_generic(OciConnection& conn, const std::string& sql,
                          std::size_t prefetch_rows, std::size_t fetch_batch_size,
                          const GenericBatchCallback& on_batch);

// insert_rows() -- a real Oracle array bind of `rows`, executed in bounded
// chunks of at most `chunk_size` rows per OciStatement::execute() call,
// rebinding fresh per chunk via OciStatement::bindNameArray() (never
// OCIStmtExecute's own rowoff parameter -- see ideas/binding's own README
// and docs/oci_statement_lifecycle_notes.md for why that crashed against a
// real database and isn't used anywhere in this codebase either).
//
// Scope: only a plain (non-optional) arithmetic, FixedString<N>, or OciDate
// field binds this way -- an optional<U> or OciClob/OciBlob field
// static_asserts here, same reasons as ideas/binding: no per-row NULL
// indicator in this path, and no fixed-stride buffer for a LOB locator's
// lifecycle to stride over.
template <scalar_bindable T>
ExecResult insert_rows(OciConnection& conn, const std::string& sql, std::vector<T>& rows, std::size_t chunk_size);

} // namespace marketlib::db::oracle

#include <db/oracle/details/oci_client.h>
