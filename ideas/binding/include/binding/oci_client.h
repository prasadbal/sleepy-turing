#pragma once
// Bind IN parameters by name, run a statement against a connection, and (for
// a SELECT) fetch rows in batches, handing each batch to a callback.
//
// Deliberately narrow: arithmetic fields, FixedString<N> (a fixed-capacity
// character buffer -- see oci_fixed_string.h), and OciDate (see
// oci_datetime.h), each optionally wrapped in std::optional<U> to mark a
// value/column nullable. No LOB, no OciTimestamp (its OCIDateTime* is a
// per-value descriptor, not a fixed-stride value -- a different shape of
// problem from everything else here), no dynamic-width anything, no
// IN-list/collection support -- those are separate concerns for later, not
// half-built in here. Reworked from an earlier, considerably larger version
// of this file that also handled all of those; that version is still in
// git history if any of it is worth resurrecting.
//
// No retry: every entry point here runs once and returns an ExecResult
// (OciConnection::execute's classification, or QueryError from a bind/fetch
// failure that never reached execute at all). A ConnectionLost result is the
// caller's cue to reconnect and call the same entry point again -- nothing
// here does that automatically.
//
// Implementation in details/oci_client.h.
#include "binding/oci_connection.h"
#include "binding/oci_datetime.h"
#include "binding/oci_fixed_string.h"
#include "binding/reflect.h"

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>

namespace binding {

// ----------------------------------------------------------------------------
// Compile-time OCI external type code for a scalar field. std::optional<U>
// takes U's type code -- the indicator, not the type code, is what tells OCI
// a value is NULL.
// ----------------------------------------------------------------------------
template <typename T> struct OciTypeBinder {
    static_assert(sizeof(T) == 0,
                  "binding: no OCI type code for this field type -- oci_client.h only supports "
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

// FixedString<N>: SQLT_CHR, an explicit-length VARCHAR2 with no null
// terminator needed -- N is the buffer size OCI defines into (what makes it
// usable as a select() output column, unlike std::string), and the field's
// own length_ref() carries the actual content length on both bind and
// define (alenp/rlenp below).
template <std::size_t N> struct OciTypeBinder<FixedString<N>> { static constexpr ub2 type_code = SQLT_CHR; };

// OciDate: SQLT_ODT, directly through the real ::OCIDate struct it wraps.
// No descriptor, no allocation, a fixed 7-byte value -- exactly like an
// arithmetic field for bind/define purposes, so (unlike FixedString<N>) it
// needs no special-casing anywhere below beyond this type-code entry.
template <> struct OciTypeBinder<OciDate> { static constexpr ub2 type_code = SQLT_ODT; };

template <typename T> struct oci_type_code_of { static constexpr ub2 value = OciTypeBinder<T>::type_code; };
template <typename U> struct oci_type_code_of<std::optional<U>> { static constexpr ub2 value = OciTypeBinder<U>::type_code; };
template <typename T> inline constexpr ub2 oci_type_code_v = oci_type_code_of<std::remove_cv_t<T>>::value;

// ----------------------------------------------------------------------------
// A struct usable as a bind-parameter or result-row type here: every field
// is arithmetic, or std::optional<arithmetic> to mark it nullable. Reuses
// reflect.h's struct_field_auditor engine (the same MSVC-safe field-walker
// flat_schema/config_schema use) with a predicate narrower than
// is_bindable_leaf_v -- that one also allows string-convertible fields,
// which this file has no way to bind (see the file comment above).
// ----------------------------------------------------------------------------
struct scalar_field_predicate {
    template <typename U>
    static constexpr bool check() {
        using V = optional_value_t<U>; // void if U isn't std::optional<something>
        return std::is_arithmetic_v<V> || std::is_arithmetic_v<U> ||
               is_fixed_string_v<V> || is_fixed_string_v<U> ||
               is_oci_date_v<V> || is_oci_date_v<U>;
    }
};
template <typename T>
concept scalar_bindable = struct_field_auditor<T, scalar_field_predicate>::value;

// ----------------------------------------------------------------------------
// execute() -- no bind parameters. DDL, or DML that's fully literal in the
// text. No fetch: a statement of this shape returns no rows.
// ----------------------------------------------------------------------------
ExecResult execute(OciConnection& conn, const std::string& sql);

// execute() with a bind-parameter struct -- DML with named parameters
// (":field_name", bound by the field's own compiler-derived name, any order,
// any number of times it appears in the text). A field declared
// std::optional<U> binds SQL NULL when empty.
template <scalar_bindable T>
ExecResult execute(OciConnection& conn, const std::string& sql, T& params);

// ----------------------------------------------------------------------------
// select_rows() -- runs a query and fetches its rows in batches, calling
// `on_batch` once per batch with a pointer to (up to) fetch_batch_size rows
// and how many of them are actually valid (the last batch of a result set is
// usually partial). Column order in `sql`'s SELECT list must match OutT's
// declared field order -- OCIDefineByPos is the only column-output bind API
// in raw OCI, so this is positional regardless of the IN side binding by
// name.
//
// prefetch_rows and fetch_batch_size are deliberately two separate numbers,
// not one: prefetch_rows controls Oracle's own client-side round-trip
// batching (OCI_ATTR_PREFETCH_ROWS) and is what actually keeps network round
// trips low; fetch_batch_size controls how many rows this code processes
// per OCIStmtFetch2/OCIAttrGet call and how big the batch buffer (and any
// optional field's staging/indicator arrays, sized to fetch_batch_size) is.
// They don't have to match -- a large prefetch with a small fetch_batch_size
// is a reasonable choice when the batch's ultimate destination doesn't
// benefit from being handed large chunks at once (inserting into a
// std::map, say, where each element is its own O(log n) insertion
// regardless of batch size, unlike a vector's amortized bulk insert).
//
// A NULL landing on a field that isn't std::optional is not detected: that
// field defines with no indicator at all (see define_one_column in
// details/oci_client.h), so it silently keeps whatever the batch buffer
// already held. Closing that -- giving every field a real, if throwaway,
// indicator -- is a small, separate change, not attempted here.
template <scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT* rows, std::size_t count)>& on_batch);

// Same as above, but also binds `input`'s fields as named IN parameters
// first (e.g. a WHERE clause) -- the read-side counterpart to
// execute(conn, sql, params). `input` only ever supplies parameters; OutT's
// column-order/type rules are unchanged from the no-input overload above.
template <scalar_bindable InT, scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql, InT& input,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT* rows, std::size_t count)>& on_batch);

} // namespace binding

#include "binding/details/oci_client.h"
