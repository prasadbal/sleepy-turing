#pragma once
// Oracle collection-type bind for a dynamic IN (...) list: bind one Oracle
// collection *object* holding every value, instead of generating
// ":1,:2,...,:N" placeholders sized to the collection at prepare time (the
// way oci_client.h's detail::bind_named_container does for a struct
// field's own multi-value IN-list). A generated placeholder list is capped
// at 1000 elements (ORA-01795, a parser-level limit on any syntactic
// IN (...) list) and, since each distinct list size is different SQL
// text, fragments Oracle's shared-pool cursor cache as sizes vary. Binding
// one collection object keeps the SQL text completely fixed regardless of
// list size, with neither limitation:
//
//   WHERE id IN (SELECT column_value FROM TABLE(:1))
//
// See docs/in_list_binding.md for the full writeup, including the
// OCI_OBJECT environment-mode requirement (OciConnection::connect() sets
// this) and the live-database verification of this code path.
//
// KNOWN BROKEN: an ElemType of std::string (SYS.ODCIVARCHAR2LIST) --
// unlike int/double (SYS.ODCINUMBERLIST), which is live-verified up to
// 10,000 elements. Appending a std::string element crashes (SIGSEGV)
// inside OCICollAppend's own internals (kolcapp/kolcecpy/kolvats) against
// a real database, confirmed via a minimal, fully status-checked raw
// repro -- not a mock artifact. This reproduces the same way whether
// `elem` is a raw (null-terminated) char* or a properly constructed
// OCIString* built via OCIStringAssignText (the documented way to
// represent a VARCHAR2 collection element); switching to the latter
// didn't change where or whether it crashes. Root cause not identified --
// don't use select_with_in_collection()/execute_with_in_collection()
// with a std::string element type against a real database until this is
// resolved.
//
// Implementation in details/oci_collection_bind.h.

#include <set>
#include <string>
#include <string_view>
#include <valarray>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_object_mock.h"

namespace binding {

// Maps ElemType to the Oracle SQL collection type to bind through. Defaults
// to Oracle's built-in ODCI schema types (SYS.ODCINUMBERLIST for numbers,
// SYS.ODCIVARCHAR2LIST for strings) so the common case needs no schema
// setup; specialize this for a custom collection type (e.g. a
// user-created "TABLE OF NUMBER" type) if you have one.
template <typename ElemType> struct OciCollectionTypeBinder;
template <> struct OciCollectionTypeBinder<int> {
    static constexpr std::string_view schema = "SYS";
    static constexpr std::string_view type_name = "ODCINUMBERLIST";
};
template <> struct OciCollectionTypeBinder<double> {
    static constexpr std::string_view schema = "SYS";
    static constexpr std::string_view type_name = "ODCINUMBERLIST";
};
template <> struct OciCollectionTypeBinder<std::string> {
    static constexpr std::string_view schema = "SYS";
    static constexpr std::string_view type_name = "ODCIVARCHAR2LIST";
};

// SELECT with a dynamic IN (...) list bound as a single Oracle collection
// object. `query_text` is fixed, ordinary SQL with exactly one positional
// bind for the collection -- e.g. "SELECT trade_id, notional FROM trades
// WHERE trade_id IN (SELECT column_value FROM TABLE(:1))" -- no marker/
// substitution needed, since the whole collection is one bind regardless
// of how many elements are in it. Not subject to the 1000-element
// ORA-01795 cap a generated placeholder list would be, and reuses the
// same SQL text (and so the same cached cursor) no matter how the
// collection's size varies between calls.
template <typename ElemType, bindable RowT>
bool select_with_in_collection(OciConnection& conn, const std::string& query_text,
                                const std::set<ElemType>& ids, std::vector<RowT>& results);

// Convenience overloads: dedupe/order `ids` into a std::set first (a
// repeated value is never meaningful in an IN-list, only a wasted bind;
// and iterating a set gives a deterministic order, which is what keeps
// two calls over the same logical set of IDs producing the same generated
// element order, useful if the collection type's element order matters
// for a comparison downstream), then delegate. The set is bound as one
// Oracle collection object either way here, so there's no ORA-01795 cap
// to speak of regardless of which of the three container types the
// caller started from.
template <typename ElemType, bindable RowT>
bool select_with_in_collection(OciConnection& conn, const std::string& query_text,
                                const std::vector<ElemType>& ids, std::vector<RowT>& results);

template <typename ElemType, bindable RowT>
bool select_with_in_collection(OciConnection& conn, const std::string& query_text,
                                const std::valarray<ElemType>& ids, std::vector<RowT>& results);

// DML (e.g. "DELETE FROM trades WHERE trade_id IN (SELECT column_value
// FROM TABLE(:1))") with a dynamic IN (...) list bound as a single Oracle
// collection object -- the no-result-rows counterpart to
// select_with_in_collection() above: fixed SQL text regardless of
// collection size, no ORA-01795 cap.
template <typename ElemType>
bool execute_with_in_collection(OciConnection& conn, const std::string& query_text,
                                 const std::set<ElemType>& ids);

template <typename ElemType>
bool execute_with_in_collection(OciConnection& conn, const std::string& query_text,
                                 const std::vector<ElemType>& ids);

template <typename ElemType>
bool execute_with_in_collection(OciConnection& conn, const std::string& query_text,
                                 const std::valarray<ElemType>& ids);

} // namespace binding

#include "binding/details/oci_collection_bind.h"
