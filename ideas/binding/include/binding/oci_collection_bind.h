#pragma once
// Oracle collection-type bind for IN (...): the alternative to
// oci_client.h's select_with_in_list()/execute_with_in_list(), which
// generate ":1,:2,...,:N" placeholders sized to the collection at prepare
// time. That approach is capped at 1000 elements (ORA-01795) and, since
// each distinct list size is different SQL text, fragments Oracle's
// shared-pool cursor cache as sizes vary. Binding one Oracle collection
// *object* instead keeps the SQL text completely fixed regardless of list
// size:
//
//   WHERE id IN (SELECT column_value FROM TABLE(:1))
//
// IMPORTANT -- lower confidence than the rest of this directory: this uses
// Oracle's object/collection OCI API (OCIType, OCIObjectNew, OCICollAppend,
// OCIBindObject), which is a genuinely more obscure corner of OCI than the
// scalar bind/define path everything else here is built on, and there's no
// real oci.h on this machine to check the signatures in oci_object_mock.h
// against -- only documentation recall. What IS verified: the C++ template
// plumbing below compiles and runs correctly against its own mock (see
// examples/collection_demo.cpp). What is NOT verified: that
// oci_object_mock.h's signatures faithfully match the real
// OCITypeByName/OCIObjectNew/OCICollAppend/OCIBindObject/OCIObjectFree in
// an actual Oracle client's headers. Diff this file's declarations against
// the real oci.h/ociap.h before trusting it against a live database.

#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <boost/pfr.hpp>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_object_mock.h"
#include "binding/reflect.h"

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

namespace detail {

// Appends one element into an already-created collection instance,
// converting it to the representation Oracle's collection element type
// expects (OCINumber for a NUMBER-typed collection, raw text for a
// VARCHAR2-typed one).
template <typename ElemType>
void append_collection_element(OciConnection& conn, OCIColl* coll, const ElemType& value) {
    if constexpr (std::is_arithmetic_v<ElemType>) {
        OCINumber num{};
        auto raw = static_cast<int>(value); // ODCINUMBERLIST elements are NUMBER; int covers the common id/key case
        OCINumberFromInt(conn.err(), &raw, sizeof(raw), OCI_NUMBER_SIGNED, &num);
        OCICollAppend(conn.env(), conn.err(), &num, nullptr, coll);
    } else if constexpr (std::is_convertible_v<ElemType, std::string_view>) {
        std::string_view sv = value;
        OCICollAppend(conn.env(), conn.err(), sv.data(), nullptr, coll);
    } else {
        static_assert(sizeof(ElemType) == 0, "no collection element encoder for this ElemType");
    }
}

// Looks up ElemType's collection type, creates a new empty instance of it,
// and appends every element of `values` -- the object-API equivalent of
// bind_in_list() in oci_client.h.
template <typename ElemType>
bool build_in_collection(OciConnection& conn, const std::set<ElemType>& values,
                          OCIType** out_tdo, dvoid** out_instance) {
    using Binder = OciCollectionTypeBinder<ElemType>;

    OCIType* tdo = nullptr;
    OCITypeByName(conn.env(), conn.err(), conn.svc(),
                  reinterpret_cast<const text*>(Binder::schema.data()), static_cast<ub4>(Binder::schema.size()),
                  reinterpret_cast<const text*>(Binder::type_name.data()), static_cast<ub4>(Binder::type_name.size()),
                  nullptr, 0, OCI_DURATION_SESSION, OCI_TYPEGET_ALL, &tdo);
    if (!tdo) return false;

    dvoid* instance = nullptr;
    OCIObjectNew(conn.env(), conn.err(), conn.svc(), OCI_TYPECODE_NAMEDCOLLECTION, tdo,
                 nullptr, OCI_DURATION_SESSION, /*value=*/1, &instance);
    if (!instance) return false;

    for (const ElemType& v : values) {
        append_collection_element(conn, reinterpret_cast<OCIColl*>(instance), v);
    }

    *out_tdo = tdo;
    *out_instance = instance;
    return true;
}

} // namespace detail

// SELECT with a dynamic IN (...) list bound as a single Oracle collection
// object, instead of a generated placeholder list. `query_text` is fixed,
// ordinary SQL with exactly one positional bind for the collection -- e.g.
// "SELECT trade_id, notional FROM trades WHERE trade_id IN (SELECT
// column_value FROM TABLE(:1))" -- no {IN} marker/substitution needed,
// since the whole collection is one bind regardless of how many elements
// are in it. Not subject to the 1000-element ORA-01795 cap
// select_with_in_list() has, and reuses the same SQL text (and so the same
// cached cursor) no matter how the collection's size varies between calls.
template <typename ElemType, bindable RowT>
bool select_with_in_collection(OciConnection& conn, const std::string& query_text,
                                const std::set<ElemType>& ids, std::vector<RowT>& results) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        results.clear();

        OCIType* tdo = nullptr;
        dvoid* instance = nullptr;
        if (!detail::build_in_collection(conn, ids, &tdo, &instance)) {
            return {false, OCI_ERROR};
        }

        OCIStmt* stmt = nullptr;
        OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
        OCIStmtPrepare(stmt, conn.err(),
                       reinterpret_cast<const text*>(query_text.c_str()),
                       static_cast<ub4>(query_text.size()),
                       OCI_NTV_SYNTAX, OCI_DEFAULT);

        OCIBind* bind_handle = nullptr;
        OCIBindByPos(stmt, &bind_handle, conn.err(), 1, &instance, 0, SQLT_NTY,
                    nullptr, nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        OCIBindObject(bind_handle, conn.err(), tdo, &instance, nullptr, nullptr, nullptr);

        RowT row_buffer{};
        std::vector<sb2> row_indicators(boost::pfr::tuple_size_v<RowT>, OCI_IND_NOTNULL);
        detail::staging_tuple_t<RowT> staging{};
        detail::define_fields(stmt, conn, row_buffer, row_indicators, staging);

        sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 0, 0, nullptr, nullptr, OCI_DEFAULT);
        if (status != OCI_SUCCESS) {
            OCIObjectFree(conn.env(), conn.err(), instance, OCI_OBJECTFREE_FORCE);
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            return {false, status};
        }

        for (;;) {
            status = OCIStmtFetch2(stmt, conn.err(), 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
            if (status == OCI_NO_DATA) break;
            if (status != OCI_SUCCESS) {
                OCIObjectFree(conn.env(), conn.err(), instance, OCI_OBJECTFREE_FORCE);
                OCIHandleFree(stmt, OCI_HTYPE_STMT);
                return {false, status};
            }
            detail::apply_null_semantics_after_fetch(row_buffer, row_indicators, staging,
                                                std::make_index_sequence<boost::pfr::tuple_size_v<RowT>>{});
            results.push_back(row_buffer);
        }

        OCIObjectFree(conn.env(), conn.err(), instance, OCI_OBJECTFREE_FORCE);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {true, OCI_SUCCESS};
    });
}

} // namespace binding
