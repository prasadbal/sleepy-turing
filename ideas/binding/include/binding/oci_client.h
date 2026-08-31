#pragma once
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
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
template <> struct OciTypeBinder<int>         { static constexpr ub2 type_code = SQLT_INT;     };
template <> struct OciTypeBinder<float>       { static constexpr ub2 type_code = SQLT_BFLOAT;  };
template <> struct OciTypeBinder<double>      { static constexpr ub2 type_code = SQLT_BDOUBLE; };
template <> struct OciTypeBinder<std::string> { static constexpr ub2 type_code = SQLT_STR;     };
template <> struct OciTypeBinder<OciClob>     { static constexpr ub2 type_code = SQLT_CLOB;    };
template <> struct OciTypeBinder<OciXml>      { static constexpr ub2 type_code = SQLT_CLOB;    };

// std::optional<U> takes U's type code -- the indicator, not the type code,
// is what tells OCI a value is NULL.
template <typename T> struct oci_type_code_of { static constexpr ub2 value = OciTypeBinder<T>::type_code; };
template <typename U> struct oci_type_code_of<std::optional<U>> { static constexpr ub2 value = OciTypeBinder<U>::type_code; };
template <typename T> inline constexpr ub2 oci_type_code_v = oci_type_code_of<T>::value;

// ----------------------------------------------------------------------------
// Predicate + concept: a struct usable as an OCI bind/row type is either a
// flat leaf (arithmetic/string, optionally wrapped in std::optional to mark
// it nullable) or a recognized LOB wrapper. Reuses the same
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

// Reads the raw bytes to bind/define for `value`: for a string-convertible
// leaf, its content (pointer + length); for anything else, its own address
// and size. Shared by the plain-field and the std::optional<value> cases.
template <typename ValueType>
std::pair<dvoid*, sb4> raw_bind_args(ValueType& value) {
    if constexpr (std::is_convertible_v<ValueType, std::string_view>) {
        std::string_view sv = value;
        return { reinterpret_cast<dvoid*>(const_cast<char*>(sv.data())), static_cast<sb4>(sv.size()) };
    } else {
        return { reinterpret_cast<dvoid*>(&value), static_cast<sb4>(sizeof(value)) };
    }
}

// Per-field scratch storage needed only for std::optional<U> fields: OCI
// needs a real, addressable U to read from (bind) or write into (define),
// and an empty std::optional<U> has no such address to hand out (*opt is
// undefined behavior when the optional is disengaged, and there's no
// standard-sanctioned way to get the address of its unset storage either).
// Every other field type binds/defines directly against its own address, so
// its slot here is unused (std::monostate).
template <typename FieldType>
using staging_slot_t = std::conditional_t<is_optional_v<FieldType>, optional_value_t<FieldType>, std::monostate>;

template <typename T, std::size_t... Is>
constexpr auto staging_tuple_type(std::index_sequence<Is...>)
    -> std::tuple<staging_slot_t<boost::pfr::tuple_element_t<Is, T>>...>;

template <typename T>
using staging_tuple_t = decltype(staging_tuple_type<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

// ---- bind (execute(): struct -> IN parameters) -----------------------------

template <std::size_t I, oci_row_schema T>
void bind_one_field(OCIStmt* stmt, OciConnection& conn, T& row,
                     std::vector<OCILobLocator**>& active_locators,
                     std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(row);
    constexpr ub4 position = I + 1;
    OCIBind* bind_handle = nullptr;
    indicators[I] = OCI_IND_NOTNULL;

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
                    oci_type_code_v<FieldType>, &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    } else if constexpr (is_optional_v<FieldType>) {
        auto& stage = std::get<I>(staging);
        if (field.has_value()) {
            stage = *field;
            indicators[I] = OCI_IND_NOTNULL;
        } else {
            stage = {};
            indicators[I] = OCI_IND_NULL;
        }
        auto [value_ptr, bind_size] = raw_bind_args(stage);
        OCIBindByPos(stmt, &bind_handle, conn.err(), position,
                    value_ptr, bind_size, oci_type_code_v<FieldType>,
                    &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    } else {
        auto [value_ptr, bind_size] = raw_bind_args(field);
        OCIBindByPos(stmt, &bind_handle, conn.err(), position,
                    value_ptr, bind_size, oci_type_code_v<FieldType>,
                    &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    }
}

template <oci_row_schema T, std::size_t... Is>
void bind_fields_impl(OCIStmt* stmt, OciConnection& conn, T& row,
                       std::vector<OCILobLocator**>& active_locators,
                       std::vector<sb2>& indicators, staging_tuple_t<T>& staging,
                       std::index_sequence<Is...>) {
    (bind_one_field<Is>(stmt, conn, row, active_locators, indicators, staging), ...);
}

// Binds every field of `row` as a parameter on `stmt`, in field-declaration
// order. Deliberately positional (OCIBindByPos), not by name: name-based
// binding would need boost::pfr's field-name reflection, which relies on
// compiler-specific __FUNCSIG__/__PRETTY_FUNCTION__ parsing and is not
// reliably available on MSVC. tuple_size/tuple_element_t/get (used here) are
// stable everywhere and always iterate in declaration order, so position N
// here always means "the Nth field of T" -- your SQL text's bind
// placeholders (by whatever name) must occur left-to-right in that same
// order. A field that's std::optional and empty binds SQL NULL.
template <oci_row_schema T>
void bind_fields(OCIStmt* stmt, OciConnection& conn, T& row,
                  std::vector<OCILobLocator**>& active_locators,
                  std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    bind_fields_impl(stmt, conn, row, active_locators, indicators, staging,
                      std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
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

// ---- dynamic IN (...) lists -------------------------------------------------

// Binds every element of `values` as a positional IN parameter, starting at
// start_position. Takes a std::set, not a vector: an IN list rarely benefits
// from a repeated value (it only adds a redundant bind), and iterating a set
// gives a deterministic order, so the same logical set of values always
// produces the same generated SQL text regardless of what order the caller
// happened to collect them in -- Oracle's shared-pool cursor cache is keyed
// on SQL text, so that determinism is what keeps a given IN-list size from
// fragmenting into many distinct cached cursors for no reason.
template <typename ElemType>
void bind_in_list(OCIStmt* stmt, OciConnection& conn, const std::set<ElemType>& values,
                   ub4 start_position, std::vector<sb2>& indicators) {
    ub4 position = start_position;
    for (const ElemType& value : values) {
        const std::size_t idx = position - start_position;
        indicators[idx] = OCI_IND_NOTNULL;
        OCIBind* bind_handle = nullptr;
        // OCIBindByPos's valuep is non-const even for an IN-only bind (OCI
        // never writes through it here); const_cast matches the same
        // read-only-through-a-mutable-pointer pattern raw_bind_args already
        // uses for std::string_view content.
        auto [value_ptr, bind_size] = raw_bind_args(const_cast<ElemType&>(value));
        OCIBindByPos(stmt, &bind_handle, conn.err(), position,
                    value_ptr, bind_size, oci_type_code_v<ElemType>,
                    &indicators[idx], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        ++position;
    }
}

// ---- define (query(): SELECT columns -> struct) ----------------------------

template <std::size_t I, oci_row_schema T>
void define_one_field(OCIStmt* stmt, OciConnection& conn, T& row,
                       std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    static_assert(!is_oci_lob_v<FieldType>,
                  "LOB columns are not supported in query() result rows yet");
    static_assert(!std::is_convertible_v<FieldType, std::string_view> &&
                  !(is_optional_v<FieldType> && std::is_convertible_v<optional_value_t<FieldType>, std::string_view>),
                  "string columns are not supported in query() result rows yet -- OCI needs a "
                  "fixed max output buffer size to define into, which this minimal client "
                  "doesn't manage. Bind side (execute()) supports std::string fine.");

    auto& field = boost::pfr::get<I>(row);
    constexpr ub4 position = I + 1;
    OCIDefine* define_handle = nullptr;
    indicators[I] = OCI_IND_NOTNULL;

    if constexpr (is_optional_v<FieldType>) {
        auto& stage = std::get<I>(staging);
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                      reinterpret_cast<dvoid*>(&stage), static_cast<sb4>(sizeof(stage)),
                      oci_type_code_v<FieldType>, &indicators[I], nullptr, nullptr, OCI_DEFAULT);
    } else {
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                      reinterpret_cast<dvoid*>(&field), static_cast<sb4>(sizeof(field)),
                      oci_type_code_v<FieldType>, &indicators[I], nullptr, nullptr, OCI_DEFAULT);
    }
}

template <oci_row_schema T, std::size_t... Is>
void define_fields_impl(OCIStmt* stmt, OciConnection& conn, T& row,
                         std::vector<sb2>& indicators, staging_tuple_t<T>& staging,
                         std::index_sequence<Is...>) {
    (define_one_field<Is>(stmt, conn, row, indicators, staging), ...);
}

template <oci_row_schema T>
void define_fields(OCIStmt* stmt, OciConnection& conn, T& row,
                    std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    define_fields_impl(stmt, conn, row, indicators, staging,
                        std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

// After each fetched row, an std::optional field was defined into a staging
// U, not into the struct directly (see staging_slot_t) -- copy it in, or
// reset to nullopt, depending on what the indicator said this row.
template <std::size_t I, oci_row_schema T>
void sync_one_optional(T& row, const std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    if constexpr (is_optional_v<FieldType>) {
        auto& field = boost::pfr::get<I>(row);
        field = (indicators[I] == OCI_IND_NULL) ? std::nullopt
                                                 : std::make_optional(std::get<I>(staging));
    }
}

template <oci_row_schema T, std::size_t... Is>
void sync_optionals_after_fetch(T& row, const std::vector<sb2>& indicators, staging_tuple_t<T>& staging,
                                 std::index_sequence<Is...>) {
    (sync_one_optional<Is>(row, indicators, staging), ...);
}

} // namespace detail

// Builds "start,start+1,...,start+count-1" as ":N,:N+1,..." for splicing an
// IN (...) clause into a query template -- Oracle needs the exact number of
// bind placeholders fixed in the prepared SQL text itself, so the caller's
// template gets re-instantiated to match the collection's size before every
// prepare. An empty collection produces "NULL": "IN ()" is a SQL syntax
// error, but "IN (NULL)" is valid and, since x = NULL is never true in SQL's
// three-valued logic, correctly matches nothing -- no special-casing needed
// at the call site for an empty ID list.
inline std::string make_in_placeholders(std::size_t count, ub4 start_position = 1) {
    if (count == 0) return "NULL";

    // ORA-01795: "maximum number of expressions in a list is 1000". This is
    // a parser-level cap on a syntactic IN (...) list -- it applies whether
    // the elements are literals or bind placeholders, and has nothing to do
    // with the 64KB max SQL statement text length (which a placeholder list
    // this size comes nowhere near). Caught here with a clear message
    // instead of surfacing as an opaque ORA-01795 from OCIStmtExecute. See
    // "Dynamic IN (...) lists" in the README for the collection-bind
    // alternative, which isn't subject to this cap.
    constexpr std::size_t oracle_max_in_list_size = 1000;
    if (count > oracle_max_in_list_size) {
        throw std::runtime_error(
            "binding: IN list has " + std::to_string(count) +
            " elements, but Oracle limits a plain IN (...) list to " +
            std::to_string(oracle_max_in_list_size) +
            " (ORA-01795) -- use a collection bind for larger lists");
    }

    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
        if (i) result += ',';
        result += ':' + std::to_string(start_position + i);
    }
    return result;
}

// Replaces the one "{IN}" marker in `query_template` with `placeholders`.
inline std::string substitute_in_marker(const std::string& query_template, const std::string& placeholders) {
    constexpr std::string_view marker = "{IN}";
    const auto pos = query_template.find(marker);
    if (pos == std::string::npos) {
        throw std::runtime_error("binding: query template is missing the {IN} placeholder marker");
    }
    std::string result = query_template;
    result.replace(pos, marker.size(), placeholders);
    return result;
}

// ----------------------------------------------------------------------------
// Core database execution client.
// ----------------------------------------------------------------------------
class OciClient {
public:
    // Single-row DML (INSERT/UPDATE/DELETE, including RETURNING ... INTO).
    // Bind placeholders in `query_text` must occur left-to-right in the same
    // order as T's fields are declared (see bind_fields above). A field
    // declared as std::optional<U> binds SQL NULL when it's empty.
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
    // reason bind_fields is). A field declared as std::optional<U> comes
    // back as std::nullopt when that column is NULL for the row; string and
    // LOB output columns aren't supported here yet (see define_one_field).
    //
    // On a disconnect mid-fetch, results are cleared and the whole SELECT is
    // re-run from scratch on reconnect (there's no cursor to resume from).
    template <oci_row_schema T>
    bool query(OciConnection& conn, const std::string& query_text, std::vector<T>& results) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_query_once(conn, query_text, results);
        });
    }

    // SELECT with a dynamic IN (...) list as the query's only bind
    // parameters. `query_template` must contain exactly one "{IN}" marker,
    // e.g. "SELECT trade_id, notional FROM trades WHERE trade_id IN ({IN})",
    // which gets replaced with a placeholder list sized to ids.size() (see
    // make_in_placeholders) before preparing. See bind_in_list for why the
    // list is a std::set, not a std::vector.
    //
    // A different ids.size() re-prepares with different SQL text each call
    // -- fine for occasional or boundedly-sized lists, but each distinct
    // size becomes its own entry in Oracle's shared-pool cursor cache. For
    // very large or highly variable-sized lists, an Oracle collection-type
    // bind (WHERE id IN (SELECT column_value FROM TABLE(:1))) keeps the SQL
    // text fixed regardless of size -- not implemented here yet (see the
    // README: same OCIType/OCIObject* machinery as the nested-column-type
    // gap on the query() side).
    template <typename ElemType, oci_row_schema T>
    bool query_with_in_list(OciConnection& conn, const std::string& query_template,
                             const std::set<ElemType>& ids, std::vector<T>& results) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_query_with_in_list_once(conn, query_template, ids, results);
        });
    }

    // Convenience overload: dedupes and orders `ids` into a std::set first
    // (see bind_in_list for why that matters), then delegates.
    template <typename ElemType, oci_row_schema T>
    bool query_with_in_list(OciConnection& conn, const std::string& query_template,
                             const std::vector<ElemType>& ids, std::vector<T>& results) {
        return query_with_in_list(conn, query_template,
                                   std::set<ElemType>(ids.begin(), ids.end()), results);
    }

    // DML (e.g. "DELETE FROM trades WHERE trade_id IN ({IN})") with a
    // dynamic IN (...) list as the statement's only bind parameters. Same
    // reconnect-on-disconnect-only policy and the same {IN}-marker/std::set
    // rules as query_with_in_list.
    template <typename ElemType>
    bool execute_with_in_list(OciConnection& conn, const std::string& query_template,
                               const std::set<ElemType>& ids) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_execute_with_in_list_once(conn, query_template, ids);
        });
    }

    template <typename ElemType>
    bool execute_with_in_list(OciConnection& conn, const std::string& query_template,
                               const std::vector<ElemType>& ids) {
        return execute_with_in_list(conn, query_template, std::set<ElemType>(ids.begin(), ids.end()));
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
        std::vector<sb2> indicators(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
        detail::staging_tuple_t<T> staging{};
        detail::bind_fields(stmt, conn, bind_struct, active_locators, indicators, staging);

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
        std::vector<sb2> indicators(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
        detail::staging_tuple_t<T> staging{};
        detail::define_fields(stmt, conn, row_buffer, indicators, staging);

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
            detail::sync_optionals_after_fetch(row_buffer, indicators, staging,
                                                std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
            results.push_back(row_buffer);
        }

        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {true, OCI_SUCCESS};
    }

    template <typename ElemType, oci_row_schema T>
    OciOutcome run_query_with_in_list_once(OciConnection& conn, const std::string& query_template,
                                            const std::set<ElemType>& ids, std::vector<T>& results) {
        results.clear();
        const std::string sql = substitute_in_marker(query_template, make_in_placeholders(ids.size(), 1));

        OCIStmt* stmt = nullptr;
        OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
        OCIStmtPrepare(stmt, conn.err(),
                       reinterpret_cast<const text*>(sql.c_str()),
                       static_cast<ub4>(sql.size()),
                       OCI_NTV_SYNTAX, OCI_DEFAULT);

        std::vector<sb2> in_indicators(ids.size(), OCI_IND_NOTNULL);
        detail::bind_in_list(stmt, conn, ids, 1, in_indicators);

        T row_buffer{};
        std::vector<sb2> row_indicators(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
        detail::staging_tuple_t<T> staging{};
        detail::define_fields(stmt, conn, row_buffer, row_indicators, staging);

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
            detail::sync_optionals_after_fetch(row_buffer, row_indicators, staging,
                                                std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
            results.push_back(row_buffer);
        }

        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {true, OCI_SUCCESS};
    }

    template <typename ElemType>
    OciOutcome run_execute_with_in_list_once(OciConnection& conn, const std::string& query_template,
                                              const std::set<ElemType>& ids) {
        const std::string sql = substitute_in_marker(query_template, make_in_placeholders(ids.size(), 1));

        OCIStmt* stmt = nullptr;
        OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
        OCIStmtPrepare(stmt, conn.err(),
                       reinterpret_cast<const text*>(sql.c_str()),
                       static_cast<ub4>(sql.size()),
                       OCI_NTV_SYNTAX, OCI_DEFAULT);

        std::vector<sb2> indicators(ids.size(), OCI_IND_NOTNULL);
        detail::bind_in_list(stmt, conn, ids, 1, indicators);

        const sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {status == OCI_SUCCESS, status};
    }
};

} // namespace binding
