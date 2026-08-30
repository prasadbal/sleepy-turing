#pragma once
#include <string>
#include <type_traits>
#include <vector>

#include <boost/pfr.hpp>

#include "binding/oci_connection.h"
#include "binding/oci_lob.h"
#include "binding/reflect.h"

namespace binding {

// ----------------------------------------------------------------------------
// Compile-time OCI external type code mapping. Add a specialization for any
// new leaf/LOB type this client should know how to bind or define.
// ----------------------------------------------------------------------------
template <typename T> struct OciTypeBinder;
template <> struct OciTypeBinder<int>     { static constexpr ub2 type_code = SQLT_INT;     };
template <> struct OciTypeBinder<float>   { static constexpr ub2 type_code = SQLT_BFLOAT;  };
template <> struct OciTypeBinder<double>  { static constexpr ub2 type_code = SQLT_BDOUBLE; };
template <> struct OciTypeBinder<OciClob> { static constexpr ub2 type_code = SQLT_CLOB;    };
template <> struct OciTypeBinder<OciXml>  { static constexpr ub2 type_code = SQLT_CLOB;    };

// ----------------------------------------------------------------------------
// Predicate + concept: a struct usable as an OCI bind/row type is either a
// flat leaf (arithmetic/string) or a recognized LOB wrapper. Reuses the same
// struct_field_auditor engine as flat_schema (binding/reflect.h) -- this is
// the "config schema" and "SQL row schema" ideas sharing one MSVC-safe core.
// ----------------------------------------------------------------------------
struct oci_bindable_predicate {
    template <typename U>
    static constexpr bool check() { return is_bindable_leaf_v<U> || is_oci_lob_v<U>; }
};

template <typename T>
concept oci_row_schema = struct_field_auditor<T, oci_bindable_predicate>::value;

namespace detail {

// Binds every field of `row` as a parameter on `stmt`, in field-declaration
// order. Deliberately positional (OCIBindByPos), not by name: name-based
// binding would need boost::pfr's field-name reflection, which relies on
// compiler-specific __FUNCSIG__/__PRETTY_FUNCTION__ parsing and is not
// reliably available on MSVC. tuple_size/for_each_field (used here) are
// stable everywhere and always iterate in declaration order, so position N
// here always means "the Nth field of T" -- your SQL text's bind
// placeholders (by whatever name) must occur left-to-right in that same
// order.
template <oci_row_schema T>
void bind_fields(OCIStmt* stmt, OciConnection& conn, T& row,
                  std::vector<OCILobLocator**>& active_locators) {
    ub4 position = 0;

    boost::pfr::for_each_field(row, [&](auto& field) {
        using FieldType = std::decay_t<decltype(field)>;
        ++position;
        OCIBind* bind_handle = nullptr;

        if constexpr (is_oci_lob_v<FieldType>) {
            OCIDescriptorAlloc(conn.env(), reinterpret_cast<void**>(&field.locator), OCI_DTYPE_LOB, 0, nullptr);
            active_locators.push_back(&field.locator);

            std::string& buffer = [&]() -> std::string& {
                if constexpr (std::is_same_v<FieldType, OciClob>) return field.text_data;
                else return field.xml_data;
            }();

            if (!buffer.empty()) {
                oraub8 bytes_written = 0;
                oraub8 chars_written = 0;
                OCILobWrite2(conn.svc(), conn.err(), field.locator, &bytes_written, &chars_written, 1,
                             reinterpret_cast<dvoid*>(buffer.data()), buffer.size(), OCI_ONE_PIECE,
                             nullptr, nullptr, 0, 0);
            }

            OCIBindByPos(stmt, &bind_handle, conn.err(), position,
                        reinterpret_cast<dvoid*>(&field.locator), sizeof(OCILobLocator*),
                        OciTypeBinder<FieldType>::type_code, nullptr, nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        } else {
            OCIBindByPos(stmt, &bind_handle, conn.err(), position,
                        reinterpret_cast<dvoid*>(&field), static_cast<sb4>(sizeof(field)),
                        OciTypeBinder<FieldType>::type_code, nullptr, nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        }
    });
}

// After a successful execute, pulls LOB contents back into the struct and
// frees the descriptors that were allocated for them.
template <oci_row_schema T>
void drain_lobs(OciConnection& conn, T& row) {
    boost::pfr::for_each_field(row, [&](auto& field) {
        using FieldType = std::decay_t<decltype(field)>;
        if constexpr (is_oci_lob_v<FieldType>) {
            if (field.locator) {
                oraub8 lob_length = 0;
                OCILobGetLength2(conn.svc(), conn.err(), field.locator, &lob_length);

                std::string& target = [&]() -> std::string& {
                    if constexpr (std::is_same_v<FieldType, OciClob>) return field.text_data;
                    else return field.xml_data;
                }();

                if (lob_length > 0) {
                    target.resize(lob_length);
                    oraub8 bytes_read = 0;
                    oraub8 chars_read = 0;
                    OCILobRead2(conn.svc(), conn.err(), field.locator, &bytes_read, &chars_read, 1,
                                reinterpret_cast<dvoid*>(target.data()), lob_length, OCI_ONE_PIECE,
                                nullptr, nullptr, 0, 0);
                } else {
                    target.clear();
                }
                OCIDescriptorFree(reinterpret_cast<void*>(field.locator), OCI_DTYPE_LOB);
                field.locator = nullptr;
            }
        }
    });
}

inline void free_locators(std::vector<OCILobLocator**>& active_locators) {
    for (auto* loc : active_locators) {
        if (*loc) {
            OCIDescriptorFree(reinterpret_cast<void*>(*loc), OCI_DTYPE_LOB);
            *loc = nullptr;
        }
    }
    active_locators.clear();
}

} // namespace detail

// ----------------------------------------------------------------------------
// Core database execution client.
// ----------------------------------------------------------------------------
class OciClient {
public:
    // Single-row DML (INSERT/UPDATE/DELETE, including RETURNING ... INTO).
    // Bind placeholders in `query_text` must occur left-to-right in the same
    // order as T's fields are declared (see bind_fields above).
    //
    // conn.run_with_reconnect() reconnects and retries the whole statement
    // only on a disconnect-class error; an ordinary execution error (bad
    // SQL, constraint violation, etc.) is returned immediately, un-retried.
    //
    // Note: passes OCI_DEFAULT (no autocommit) to OCIStmtExecute -- deciding
    // transaction/commit boundaries is left to the caller (OCITransCommit or
    // OCI_COMMIT_ON_SUCCESS), it's out of scope for this reconnect scaffold.
    template <oci_row_schema T>
    bool execute(OciConnection& conn, const std::string& query_text, T& bind_struct) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_execute_once(conn, query_text, bind_struct);
        });
    }

    // SELECT into a vector of rows -- the "vector<S> as a result set" case.
    // Column order in `query_text`'s SELECT list must match T's declared
    // field order (OCIDefineByPos is positional for the same MSVC-safety
    // reason bind_fields is). LOB output columns aren't supported here yet.
    //
    // On a disconnect mid-fetch, results are cleared and the whole SELECT is
    // re-run from scratch on reconnect (there's no cursor to resume from).
    template <oci_row_schema T>
    bool query(OciConnection& conn, const std::string& query_text, std::vector<T>& results) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_query_once(conn, query_text, results);
        });
    }

private:
    template <oci_row_schema T>
    OciOutcome run_execute_once(OciConnection& conn, const std::string& query_text, T& bind_struct) {
        OCIStmt* stmt = nullptr;
        OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);

        OCIStmtPrepare(stmt, conn.err(),
                       reinterpret_cast<const text*>(query_text.c_str()),
                       static_cast<ub4>(query_text.size()),
                       OCI_NTV_SYNTAX, OCI_DEFAULT);

        std::vector<OCILobLocator**> active_locators;
        detail::bind_fields(stmt, conn, bind_struct, active_locators);

        const sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);

        if (status == OCI_SUCCESS) {
            detail::drain_lobs(conn, bind_struct);
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            return {true, status};
        }

        detail::free_locators(active_locators);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {false, status};
    }

    template <oci_row_schema T>
    OciOutcome run_query_once(OciConnection& conn, const std::string& query_text, std::vector<T>& results) {
        results.clear();

        OCIStmt* stmt = nullptr;
        OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
        OCIStmtPrepare(stmt, conn.err(),
                       reinterpret_cast<const text*>(query_text.c_str()),
                       static_cast<ub4>(query_text.size()),
                       OCI_NTV_SYNTAX, OCI_DEFAULT);

        T row_buffer{};
        ub4 position = 0;
        boost::pfr::for_each_field(row_buffer, [&](auto& field) {
            using FieldType = std::decay_t<decltype(field)>;
            static_assert(!is_oci_lob_v<FieldType>,
                          "LOB columns are not supported in query() result rows yet");
            OCIDefine* define_handle = nullptr;
            OCIDefineByPos(stmt, &define_handle, conn.err(), ++position,
                          reinterpret_cast<dvoid*>(&field), static_cast<sb4>(sizeof(field)),
                          OciTypeBinder<FieldType>::type_code, nullptr, nullptr, nullptr, OCI_DEFAULT);
        });

        sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 0, 0, nullptr, nullptr, OCI_DEFAULT);
        if (status != OCI_SUCCESS) {
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            return {false, status};
        }

        for (;;) {
            status = OCIStmtFetch2(stmt, conn.err(), 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
            if (status == OCI_NO_DATA) break;
            if (status != OCI_SUCCESS) {
                OCIHandleFree(stmt, OCI_HTYPE_STMT);
                return {false, status};
            }
            results.push_back(row_buffer);
        }

        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {true, OCI_SUCCESS};
    }
};

} // namespace binding
