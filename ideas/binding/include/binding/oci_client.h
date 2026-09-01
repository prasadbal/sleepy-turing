#pragma once
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <valarray>
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
struct bindable_predicate {
    template <typename U>
    static constexpr bool check() { return is_bindable_leaf_v<U> || is_oci_lob_v<U>; }
};

// T can be bound as OciClient::execute()/insert()'s parameter struct or
// select()'s result row.
template <typename T>
concept bindable = struct_field_auditor<T, bindable_predicate>::value;

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

// ---- bind (execute()/insert(): struct -> IN parameters) --------------------

template <std::size_t I, bindable T>
void bind_one_field(OCIStmt* stmt, OciConnection& conn, T& row,
                     std::vector<OCILobLocator**>& active_locators,
                     std::vector<sb2>& indicators, staging_tuple_t<T>& staging,
                     std::string_view field_name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(row);
    const std::string placeholder = ":" + std::string(field_name);
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

        OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
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
        OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    value_ptr, bind_size, oci_type_code_v<FieldType>,
                    &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    } else {
        auto [value_ptr, bind_size] = raw_bind_args(field);
        OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    value_ptr, bind_size, oci_type_code_v<FieldType>,
                    &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    }
}

template <bindable T, std::size_t... Is>
void bind_fields_impl(OCIStmt* stmt, OciConnection& conn, T& row,
                       std::vector<OCILobLocator**>& active_locators,
                       std::vector<sb2>& indicators, staging_tuple_t<T>& staging,
                       std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_field<Is>(stmt, conn, row, active_locators, indicators, staging, names[Is]), ...);
}

// Binds every field of `row` as a named IN parameter (OCIBindByName) using
// its own (compiler-derived) field name -- e.g. field `bonus_pct` binds
// `:bonus_pct` wherever that placeholder occurs in the SQL text. Unlike
// OCIDefineByPos on the select() side (see define_one_field below), OCI does
// offer a name-based bind API, and boost::pfr::names_as_array() is
// confirmed available here, so binding by name is used instead of the
// occurrence-order-fragile OCIBindByPos this used before: a struct field
// can now be declared in any order, since matching no longer depends on it
// lining up with wherever the placeholder happens to occur in the SQL text.
// A field that's std::optional and empty binds SQL NULL.
template <bindable T>
void bind_fields(OCIStmt* stmt, OciConnection& conn, T& row,
                  std::vector<OCILobLocator**>& active_locators,
                  std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    bind_fields_impl(stmt, conn, row, active_locators, indicators, staging,
                      std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

// After a successful execute, pulls LOB contents back into the struct and
// frees the descriptors that were allocated for them.
template <bindable T>
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

// ---- define (select(): SELECT columns -> struct) ---------------------------

template <std::size_t I, bindable T>
void define_one_field(OCIStmt* stmt, OciConnection& conn, T& row,
                       std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    static_assert(!is_oci_lob_v<FieldType>,
                  "LOB columns are not supported in select() result rows yet");
    static_assert(!std::is_convertible_v<FieldType, std::string_view> &&
                  !(is_optional_v<FieldType> && std::is_convertible_v<optional_value_t<FieldType>, std::string_view>),
                  "string columns are not supported in select() result rows yet -- OCI needs a "
                  "fixed max output buffer size to define into, which this minimal client "
                  "doesn't manage. Bind side (execute()/insert()) supports std::string fine.");

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

template <bindable T, std::size_t... Is>
void define_fields_impl(OCIStmt* stmt, OciConnection& conn, T& row,
                         std::vector<sb2>& indicators, staging_tuple_t<T>& staging,
                         std::index_sequence<Is...>) {
    (define_one_field<Is>(stmt, conn, row, indicators, staging), ...);
}

template <bindable T>
void define_fields(OCIStmt* stmt, OciConnection& conn, T& row,
                    std::vector<sb2>& indicators, staging_tuple_t<T>& staging) {
    define_fields_impl(stmt, conn, row, indicators, staging,
                        std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

// After each fetched row, applies what the indicator said for one field:
//   - std::optional<U>: the field was defined into a staging U, not into
//     the struct directly (see staging_slot_t) -- copy it in, or reset to
//     nullopt, depending on the indicator.
//   - anything else (a "required" field): a NULL indicator is an error.
//     There's no schema/DESCRIBE metadata available to check ahead of time
//     whether a column can be NULL (see the README) -- this is the only
//     point a NULL landing on a non-optional field can be caught at all.
//     Throwing here, rather than returning a retriable failure, is
//     deliberate: no reconnect/retry can ever fix a real data/schema
//     mismatch like this, so retrying it would just waste a retry budget
//     reproducing the same throw.
template <std::size_t I, bindable T>
void apply_field_null_semantics(T& row, const std::vector<sb2>& indicators,
                                 staging_tuple_t<T>& staging, std::string_view field_name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    if constexpr (is_optional_v<FieldType>) {
        auto& field = boost::pfr::get<I>(row);
        field = (indicators[I] == OCI_IND_NULL) ? std::nullopt
                                                 : std::make_optional(std::get<I>(staging));
    } else if (indicators[I] == OCI_IND_NULL) {
        throw std::runtime_error(
            "binding: column bound to field '" + std::string(field_name) +
            "' returned SQL NULL, but that field is not declared std::optional<T> -- "
            "wrap it in std::optional<T> if this column can be NULL");
    }
}

template <bindable T, std::size_t... Is>
void apply_null_semantics_after_fetch(T& row, const std::vector<sb2>& indicators,
                                       staging_tuple_t<T>& staging, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (apply_field_null_semantics<Is>(row, indicators, staging, names[Is]), ...);
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
//
// Names read as SQL verbs: execute() runs any statement that doesn't return
// rows (DDL, or DML with or without a bound struct); insert() is a
// same-mechanism, intent-naming alias for execute() that also adds a
// std::vector<T> overload for inserting several rows; select() runs a query
// and returns its rows.
// ----------------------------------------------------------------------------
class OciClient {
public:
    // Runs any SQL statement that returns no rows and needs no bind
    // parameters -- DDL (CREATE/ALTER/TRUNCATE), or DML with everything
    // already literal in the text. For a statement with bind parameters,
    // use the execute(conn, query_text, bind_struct) overload below.
    bool execute(OciConnection& conn, const std::string& query_text) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_execute_once(conn, query_text);
        });
    }

    // Runs any SQL statement that returns no rows (INSERT/UPDATE/DELETE/
    // MERGE/a stored-procedure call/..., including RETURNING ... INTO),
    // binding bind_struct's fields as named IN parameters. Each field binds
    // to a `:field_name` placeholder in `query_text` by its own
    // (compiler-derived) name -- e.g. field `bonus_pct` binds `:bonus_pct`
    // wherever that placeholder appears, in any order, any number of times
    // (see bind_fields above). A field declared as std::optional<U> binds
    // SQL NULL when it's empty.
    //
    // conn.run_with_reconnect() reconnects and retries the whole statement
    // only on a disconnect-class error; an ordinary execution error (bad
    // SQL, constraint violation, etc.) is returned immediately, un-retried.
    //
    // Note: passes OCI_DEFAULT (no autocommit) to OCIStmtExecute -- deciding
    // transaction/commit boundaries is left to the caller (OCITransCommit or
    // OCI_COMMIT_ON_SUCCESS), it's out of scope for this reconnect scaffold.
    template <bindable T>
    bool execute(OciConnection& conn, const std::string& query_text, T& bind_struct) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_execute_once(conn, query_text, bind_struct);
        });
    }

    // Same mechanism as execute(conn, query_text, bind_struct) -- an alias
    // that reads as intent for the common case where the statement actually
    // is an INSERT.
    template <bindable T>
    bool insert(OciConnection& conn, const std::string& query_text, T& row) {
        return execute(conn, query_text, row);
    }

    // Inserts several rows by running `query_text` once per element of
    // `rows`, in order. A naive per-row loop for now -- one OCIStmtExecute
    // (with its own reconnect-retry) per row, not a real Oracle array bind
    // (OCIBindByName + OCI_ATTR_BIND_COUNT/an array of values bound as one
    // exec, iters = rows.size()). Stops at the first failed row without
    // rolling back the rows already inserted -- like execute()/insert()
    // above, transaction/commit boundaries are the caller's responsibility.
    // TODO: real bulk-bind support once this matters for throughput; the
    // per-row loop is correct, just not fast for large `rows`.
    template <bindable T>
    bool insert(OciConnection& conn, const std::string& query_text, std::vector<T>& rows) {
        for (T& row : rows) {
            if (!insert(conn, query_text, row)) return false;
        }
        return true;
    }

    // Runs a SELECT and returns its rows -- the "vector<S> as a result set"
    // case. Column order in `query_text`'s SELECT list must match T's
    // declared field order: unlike execute()/insert(), which bind by name,
    // OCIDefineByPos is the *only* way to bind output columns in raw OCI --
    // there is no OCIDefineByName. That's an OCI limitation, not a choice
    // made here, so this side stays positional regardless of boost::pfr's
    // name-reflection support. A field declared as std::optional<U> comes
    // back as std::nullopt when that column is NULL for the row; string and
    // LOB output columns aren't supported here yet (see define_one_field).
    //
    // On a disconnect mid-fetch, results are cleared and the whole SELECT is
    // re-run from scratch on reconnect (there's no cursor to resume from).
    template <bindable T>
    bool select(OciConnection& conn, const std::string& query_text, std::vector<T>& results) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_select_once(conn, query_text, results);
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
    // size becomes its own entry in Oracle's shared-pool cursor cache, and
    // it's capped at 1000 elements (ORA-01795, see make_in_placeholders).
    // For very large or highly variable-sized lists, oci_collection_bind.h's
    // select_with_in_collection() keeps the SQL text fixed regardless of
    // size instead.
    template <typename ElemType, bindable T>
    bool select_with_in_list(OciConnection& conn, const std::string& query_template,
                              const std::set<ElemType>& ids, std::vector<T>& results) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_select_with_in_list_once(conn, query_template, ids, results);
        });
    }

    // Convenience overload: dedupes and orders `ids` into a std::set first
    // (see bind_in_list for why that matters), then delegates.
    template <typename ElemType, bindable T>
    bool select_with_in_list(OciConnection& conn, const std::string& query_template,
                              const std::vector<ElemType>& ids, std::vector<T>& results) {
        return select_with_in_list(conn, query_template,
                                    std::set<ElemType>(ids.begin(), ids.end()), results);
    }

    // Same convenience, for a std::valarray<ElemType> collection of ids.
    template <typename ElemType, bindable T>
    bool select_with_in_list(OciConnection& conn, const std::string& query_template,
                              const std::valarray<ElemType>& ids, std::vector<T>& results) {
        return select_with_in_list(conn, query_template,
                                    std::set<ElemType>(std::begin(ids), std::end(ids)), results);
    }

    // DML (e.g. "DELETE FROM trades WHERE trade_id IN ({IN})") with a
    // dynamic IN (...) list as the statement's only bind parameters. Same
    // reconnect-on-disconnect-only policy and the same {IN}-marker/std::set
    // rules as select_with_in_list.
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

    // Same convenience, for a std::valarray<ElemType> collection of ids.
    template <typename ElemType>
    bool execute_with_in_list(OciConnection& conn, const std::string& query_template,
                               const std::valarray<ElemType>& ids) {
        return execute_with_in_list(conn, query_template, std::set<ElemType>(std::begin(ids), std::end(ids)));
    }

private:
    OciOutcome run_execute_once(OciConnection& conn, const std::string& query_text) {
        OCIStmt* stmt = nullptr;
        OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
        OCIStmtPrepare(stmt, conn.err(),
                       reinterpret_cast<const text*>(query_text.c_str()),
                       static_cast<ub4>(query_text.size()),
                       OCI_NTV_SYNTAX, OCI_DEFAULT);

        const sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {status == OCI_SUCCESS, status};
    }

    template <bindable T>
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

    template <bindable T>
    OciOutcome run_select_once(OciConnection& conn, const std::string& query_text, std::vector<T>& results) {
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
            detail::apply_null_semantics_after_fetch(row_buffer, indicators, staging,
                                                      std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
            results.push_back(row_buffer);
        }

        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return {true, OCI_SUCCESS};
    }

    template <typename ElemType, bindable T>
    OciOutcome run_select_with_in_list_once(OciConnection& conn, const std::string& query_template,
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
            detail::apply_null_semantics_after_fetch(row_buffer, row_indicators, staging,
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
