#pragma once
#include <string>
#include <type_traits>
#include <vector>

#include "binding/oci_compat.h"

// ============================================================================
// LOB (large object) field types -- CLOB (character data) and BLOB (binary
// data), each backed by a real OCI LOB locator under the hood.
//
// This is a genuinely different shape of problem from every other field
// type in oci_client.h: an int, a FixedString<N>, an OciDate all bind and
// define through a fixed-size buffer whose address OCI reads/writes
// directly. A LOB does not -- OCI hands back (on fetch) or needs (on bind)
// an OCILobLocator*, a descriptor that has to be allocated
// (OCIDescriptorAlloc), populated via OCILobWrite2/read via OCILobRead2,
// and freed, none of which fits the raw-buffer bind/define path every
// other type here uses. See the LOB-specific branches in
// details/oci_client.h (bind_one_param, define_one_column, apply_one_column)
// for that machinery -- it's entirely hidden from callers, who only ever
// see OciClob/OciBlob as plain value types (a std::string or
// std::vector<unsigned char> with no locator exposed).
//
// Scope of what's wired in (see oci_client.h and README.md for the reasons):
//   - execute(conn, sql, params) and select_rows() -- yes, single-row bind
//     and single/batch-row fetch both work, live-verified against a real
//     database.
//   - insert_rows() (chunked array bind) -- no. Each row's LOB needs its
//     own locator lifecycle; there's no fixed-stride raw buffer for
//     OCIBindArrayOfStruct to stride over the way there is for every other
//     field type. bind_array_field static_asserts against a LOB field for
//     exactly this reason.
//   - std::optional<OciClob>/std::optional<OciBlob> (a nullable LOB
//     column) -- no, not yet. Scalar/FixedString/OciDate fields already
//     have a NULL path (an empty optional binds SQL NULL; a NULL column
//     fetches into std::nullopt); doing the same correctly for a LOB bind
//     needs a real, verified answer for "what locator (if any) do you bind
//     for a NULL value", which needs a live database to check rather than
//     guessing -- left out of this pass rather than shipped unverified.
// ============================================================================

namespace binding {

// A CLOB (character large object) field -- SQLT_CLOB, text_data holds the
// value as an ordinary std::string (whatever encoding the session's
// character set implies, same as every other text value in this library).
class OciClob {
public:
    OciClob() = default;
    explicit OciClob(std::string data) : text_data(std::move(data)) {}
    std::string text_data;
};

// A BLOB (binary large object) field -- SQLT_BLOB, identical mechanics to
// OciClob (same locator lifecycle, same OCILobWrite2/OCILobRead2 calls in
// details/oci_client.h), just bytes instead of text and no character-set
// conversion involved.
class OciBlob {
public:
    OciBlob() = default;
    explicit OciBlob(std::vector<unsigned char> data) : binary_data(std::move(data)) {}
    std::vector<unsigned char> binary_data;
};

template <typename T> inline constexpr bool is_oci_clob_v = std::is_same_v<T, OciClob>;
template <typename T> inline constexpr bool is_oci_blob_v = std::is_same_v<T, OciBlob>;
template <typename T> inline constexpr bool is_oci_lob_v = is_oci_clob_v<T> || is_oci_blob_v<T>;

} // namespace binding
