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
inline void record_collection_status(sword& accumulated, sword status) noexcept {
    if (accumulated == OCI_SUCCESS && status != OCI_SUCCESS) accumulated = status;
}

template <typename ElemType>
void append_collection_element(sword& status, OciConnection& conn, OCIColl* coll, const ElemType& value) {
    if constexpr (std::is_floating_point_v<ElemType>) {
        // Oracle NUMBER is decimal and holds a double's full range; going
        // through OCINumberFromInt truncated every fractional value to an int
        // before it was ever appended, so a collection built from doubles
        // silently matched on truncated integers -- wrong rows, no error
        // anywhere. OciCollectionTypeBinder<double> exists, so this path is
        // reachable by design.
        OCINumber num{};
        double raw = static_cast<double>(value);
        record_collection_status(status, OCINumberFromReal(conn.err(), &raw, sizeof(raw), &num));
        record_collection_status(status, OCICollAppend(conn.env(), conn.err(), &num, nullptr, coll));
    } else if constexpr (std::is_integral_v<ElemType>) {
        // The element's own width and signedness, not int: a 64-bit id
        // narrowed to int is the same silent-truncation bug as above.
        OCINumber num{};
        ElemType raw = value;
        record_collection_status(status, OCINumberFromInt(conn.err(), &raw, sizeof(raw),
                                 std::is_signed_v<ElemType> ? OCI_NUMBER_SIGNED : OCI_NUMBER_UNSIGNED, &num));
        record_collection_status(status, OCICollAppend(conn.env(), conn.err(), &num, nullptr, coll));
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
        record_collection_status(status, OCIStringAssignText(conn.env(), conn.err(),
                             reinterpret_cast<const text*>(sv.data()), static_cast<ub4>(sv.size()),
                             &ocistr));
        record_collection_status(status, OCICollAppend(conn.env(), conn.err(), &ocistr, nullptr, coll));
        // OCICollAppend copies the string's content into the collection's
        // own storage, so the temporary descriptor is released right away
        // rather than held until build_in_collection's caller frees the
        // whole collection.
        record_collection_status(status, OCIStringResize(conn.env(), conn.err(), 0, &ocistr));
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
    sword status = OCITypeByName(conn.env(), conn.err(), conn.svc(),
                  reinterpret_cast<const text*>(Binder::schema.data()), static_cast<ub4>(Binder::schema.size()),
                  reinterpret_cast<const text*>(Binder::type_name.data()), static_cast<ub4>(Binder::type_name.size()),
                  nullptr, 0, OCI_DURATION_SESSION, OCI_TYPEGET_ALL, &tdo);
    if (status != OCI_SUCCESS || !tdo) return false;

    dvoid* instance = nullptr;
    status = OCIObjectNew(conn.env(), conn.err(), conn.svc(), OCI_TYPECODE_NAMEDCOLLECTION, tdo,
                 nullptr, OCI_DURATION_SESSION, /*value=*/1, &instance);
    if (status != OCI_SUCCESS || !instance) return false;

    for (const ElemType& v : values) {
        append_collection_element(status, conn, reinterpret_cast<OCIColl*>(instance), v);
        if (status != OCI_SUCCESS) {
            // Do not hand back a partially built collection: binding it would
            // silently query against a subset of the requested ids.
            OCIObjectFree(conn.env(), conn.err(), instance, OCI_OBJECTFREE_FORCE);
            return false;
        }
    }

    *out_tdo = tdo;
    *out_instance = instance;
    return true;
}

// Frees a built collection instance on every exit path, including a throw out
// of the fetch loop (apply_field_null_semantics throws on an unexpected NULL).
class ObjectInstanceGuard {
public:
    ObjectInstanceGuard(OciConnection& conn, dvoid* instance) : conn_(conn), instance_(instance) {}
    ~ObjectInstanceGuard() {
        if (instance_) OCIObjectFree(conn_.env(), conn_.err(), instance_, OCI_OBJECTFREE_FORCE);
    }

    ObjectInstanceGuard(const ObjectInstanceGuard&) = delete;
    ObjectInstanceGuard& operator=(const ObjectInstanceGuard&) = delete;

private:
    OciConnection& conn_;
    dvoid* instance_;
};

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

        detail::ObjectInstanceGuard instance_guard(conn, instance);

        detail::StmtHandle stmt;
        const sword prepare_status = detail::prepare_statement(conn, stmt, query_text);
        if (prepare_status != OCI_SUCCESS) {
            return {false, prepare_status};
        }

        OCIBind* bind_handle = nullptr;
        sword bind_status = OCIBindByPos(stmt.get(), &bind_handle, conn.err(), 1, &instance, 0, SQLT_NTY,
                    nullptr, nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        if (bind_status == OCI_SUCCESS) {
            bind_status = OCIBindObject(bind_handle, conn.err(), tdo, &instance, nullptr, nullptr, nullptr);
        }
        if (bind_status != OCI_SUCCESS) {
            return {false, bind_status};
        }

        return detail::run_select_fetch_loop(conn, stmt.get(), results);
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

        detail::ObjectInstanceGuard instance_guard(conn, instance);

        detail::StmtHandle stmt;
        const sword prepare_status = detail::prepare_statement(conn, stmt, query_text);
        if (prepare_status != OCI_SUCCESS) {
            return {false, prepare_status};
        }

        OCIBind* bind_handle = nullptr;
        sword bind_status = OCIBindByPos(stmt.get(), &bind_handle, conn.err(), 1, &instance, 0, SQLT_NTY,
                    nullptr, nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        if (bind_status == OCI_SUCCESS) {
            bind_status = OCIBindObject(bind_handle, conn.err(), tdo, &instance, nullptr, nullptr, nullptr);
        }
        if (bind_status != OCI_SUCCESS) {
            return {false, bind_status};
        }

        const sword status = OCIStmtExecute(conn.svc(), stmt.get(), conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);
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
