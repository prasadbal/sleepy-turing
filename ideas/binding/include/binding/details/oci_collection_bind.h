#pragma once
// Implementation of oci_collection_bind.h's free functions -- see that file
// for the interface, the OCI_OBJECT environment-mode requirement, and the
// live-database verification. Not meant to be included directly.
//
// IMPORTANT -- lower confidence than the rest of this directory in one
// respect: oci_object_mock.h's mock signatures for OCIType/OCIObjectNew/
// OCICollAppend/OCIBindObject were originally written from documentation
// recall with no real oci.h to check against. They have since been
// compiled (-fsyntax-only) against real oci.h/ociap.h and exercised in a
// live test (see docs/in_list_binding.md) with zero issues, but that mock
// file itself is still worth a second look if this ever needs to run
// against an OCI client meaningfully older or newer than what's been
// tested.
#include "binding/oci_collection_bind.h"

#include <stdexcept>
#include <type_traits>

namespace binding {
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
        // A VARCHAR2 collection element's OCI representation is an
        // OCIString* descriptor, not a raw char* -- OCICollAppend expects
        // `elem` to point at *that* (an OCIString**) for a VARCHAR2-typed
        // collection, the same way a NUMBER element needs an OCINumber
        // above. Passing sv.data() directly used to compile (it's just a
        // const void*) but crashed inside OCICollAppend on a real
        // database (SIGSEGV in kolcapp/kolcecpy, which expect to
        // dereference an OCIString descriptor, not read raw text through
        // that pointer) -- confirmed via gdb backtrace, not caught by the
        // mock or by the earlier collection-bind demos, which only ever
        // exercised an int element type live.
        OCIString* ocistr = nullptr;
        OCIStringAssignText(conn.env(), conn.err(),
                             reinterpret_cast<const text*>(sv.data()), static_cast<ub4>(sv.size()),
                             &ocistr);
        OCICollAppend(conn.env(), conn.err(), &ocistr, nullptr, coll);
        // OCICollAppend copies the string's content into the collection's
        // own storage, so the temporary descriptor is released right away
        // rather than held until build_in_collection's caller frees the
        // whole collection.
        OCIStringResize(conn.env(), conn.err(), 0, &ocistr);
    } else {
        static_assert(sizeof(ElemType) == 0, "no collection element encoder for this ElemType");
    }
}

// Looks up ElemType's collection type, creates a new empty instance of it,
// and appends every element of `values`.
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

        const OciOutcome outcome = detail::run_select_fetch_loop(conn, stmt, results);

        OCIObjectFree(conn.env(), conn.err(), instance, OCI_OBJECTFREE_FORCE);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return outcome;
    });
}

template <typename ElemType, bindable RowT>
bool select_with_in_collection(OciConnection& conn, const std::string& query_text,
                                const std::vector<ElemType>& ids, std::vector<RowT>& results) {
    return select_with_in_collection(conn, query_text, std::set<ElemType>(ids.begin(), ids.end()), results);
}

template <typename ElemType, bindable RowT>
bool select_with_in_collection(OciConnection& conn, const std::string& query_text,
                                const std::valarray<ElemType>& ids, std::vector<RowT>& results) {
    return select_with_in_collection(conn, query_text, std::set<ElemType>(std::begin(ids), std::end(ids)), results);
}

template <typename ElemType>
bool execute_with_in_collection(OciConnection& conn, const std::string& query_text,
                                 const std::set<ElemType>& ids) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
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

        const sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);
        OCIObjectFree(conn.env(), conn.err(), instance, OCI_OBJECTFREE_FORCE);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {status == OCI_SUCCESS, status};
    });
}

template <typename ElemType>
bool execute_with_in_collection(OciConnection& conn, const std::string& query_text,
                                 const std::vector<ElemType>& ids) {
    return execute_with_in_collection(conn, query_text, std::set<ElemType>(ids.begin(), ids.end()));
}

template <typename ElemType>
bool execute_with_in_collection(OciConnection& conn, const std::string& query_text,
                                 const std::valarray<ElemType>& ids) {
    return execute_with_in_collection(conn, query_text, std::set<ElemType>(std::begin(ids), std::end(ids)));
}

} // namespace binding
