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
// std::vector<U>/std::set<U>/std::valarray<U> recognition, for a struct
// field meant to bind as a dynamic multi-value IN-list parameter (see
// "input variables" further down) rather than a single scalar. Distinct
// from reflect.h's is_vector_v/vector_value_t, which is about config's
// "repeated nested struct" concept -- an entirely different thing sharing
// only the word "vector".
// ----------------------------------------------------------------------------
template <typename T> struct is_multi_bind_container_impl : std::false_type { using value_type = void; };
template <typename U> struct is_multi_bind_container_impl<std::vector<U>>   : std::true_type  { using value_type = U; };
template <typename U> struct is_multi_bind_container_impl<std::set<U>>     : std::true_type  { using value_type = U; };
template <typename U> struct is_multi_bind_container_impl<std::valarray<U>> : std::true_type  { using value_type = U; };

template <typename T>
inline constexpr bool is_multi_bind_container_v = is_multi_bind_container_impl<std::remove_cv_t<T>>::value;
template <typename T>
using multi_bind_value_t = typename is_multi_bind_container_impl<std::remove_cv_t<T>>::value_type;

// ----------------------------------------------------------------------------
// Predicate + concept: a struct usable as an OCI bind/row type has fields
// that are each a flat leaf (arithmetic/string, optionally wrapped in
// std::optional to mark it nullable), a recognized LOB wrapper, or (bind
// side only -- see define_one_field's static_assert) a
// vector/set/valarray<U> of a leaf U, for a dynamic multi-value IN-list
// parameter. Reuses the same struct_field_auditor engine as flat_schema
// (binding/reflect.h) -- this is the "config schema" and "SQL row schema"
// ideas sharing one MSVC-safe core.
// ----------------------------------------------------------------------------
struct bindable_predicate {
    template <typename U>
    static constexpr bool check() {
        if constexpr (is_multi_bind_container_v<U>) {
            return is_bindable_leaf_v<multi_bind_value_t<U>>;
        } else {
            return is_bindable_leaf_v<U> || is_oci_lob_v<U>;
        }
    }
};

// T can be bound as OciClient::execute()/insert()'s parameter struct or
// select()'s result row.
template <typename T>
concept bindable = struct_field_auditor<T, bindable_predicate>::value;

// Like a struct field bound as a dynamic multi-value IN-list (see
// bind_named_container below), but named after the field itself
// (":field_name_0,:field_name_1,..."), for OCIBindByName. Subject to
// ORA-01795 ("maximum number of expressions in a list is 1000"), a
// parser-level cap on a syntactic IN (...) list that applies whether its
// elements are literals or bind placeholders -- caught here with a clear
// message instead of surfacing as an opaque ORA-01795 from OCIStmtExecute.
// A count of 0 produces "NULL": "IN ()" is a SQL syntax error, but
// "IN (NULL)" is valid and, since x = NULL is never true in SQL's
// three-valued logic, correctly matches nothing -- no special-casing
// needed at the call site for an empty collection.
//
// For a collection with more than 1000 elements, or one whose size varies
// enough between calls to fragment Oracle's shared-pool cursor cache (each
// distinct generated placeholder-list size is different SQL text, hence a
// different cached cursor), oci_collection_bind.h's
// select_with_in_collection()/execute_with_in_collection() bind a single
// Oracle collection object instead -- fixed SQL text regardless of size,
// no cap. This client only builds the placeholder-list version, since the
// collection-bind alternative already exists and dominates it in every
// case except needing SYS.ODCINUMBERLIST/SYS.ODCIVARCHAR2LIST (or a custom
// collection type) to be usable in your schema.
inline std::string make_named_placeholders(std::string_view field_name, std::size_t count) {
    if (count == 0) return "NULL";

    constexpr std::size_t oracle_max_in_list_size = 1000;
    if (count > oracle_max_in_list_size) {
        throw std::runtime_error(
            "binding: field '" + std::string(field_name) + "' has " + std::to_string(count) +
            " elements, but Oracle limits a plain IN (...) list to " +
            std::to_string(oracle_max_in_list_size) + " (ORA-01795) -- use a collection bind "
            "(oci_collection_bind.h) for larger lists");
    }

    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
        if (i) result += ',';
        result += ':' + std::string(field_name) + '_' + std::to_string(i);
    }
    return result;
}

// Replaces the first occurrence of "{marker_name}" in `query_template` with
// `placeholders` -- used for the per-field container marker (that field's
// own name, see detail::substitute_container_markers).
inline std::string substitute_marker(const std::string& query_template, std::string_view marker_name,
                                      const std::string& placeholders) {
    const std::string marker = "{" + std::string(marker_name) + "}";
    const auto pos = query_template.find(marker);
    if (pos == std::string::npos) {
        throw std::runtime_error("binding: query template is missing the " + marker + " placeholder marker");
    }
    std::string result = query_template;
    result.replace(pos, marker.size(), placeholders);
    return result;
}

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

// Binds every element of `container` as its own named IN parameter,
// `:field_name_0`, `:field_name_1`, ... in iteration order. Does NOT
// dedupe/sort into a set first: a struct field's std::vector/std::valarray
// is bound exactly as the caller populated it, in order, duplicates
// included -- that would be a surprising thing for a general-purpose
// field bind to silently do to a caller's own data (it's the right
// default for the standalone IN-list convenience functions in
// oci_collection_bind.h, where the collection only ever means "a set of
// IDs to match," but a struct field is just a field). A std::set<U> field
// is naturally already ordered/deduped by virtue of being a set, so
// behaves the same either way.
template <typename Container>
void bind_named_container(OCIStmt* stmt, OciConnection& conn, Container& container,
                           std::string_view field_name, std::vector<sb2>& elem_indicators) {
    using ElemType = multi_bind_value_t<Container>;
    elem_indicators.assign(container.size(), OCI_IND_NOTNULL);
    std::size_t idx = 0;
    for (auto& elem : container) {
        const std::string placeholder = ":" + std::string(field_name) + "_" + std::to_string(idx);
        OCIBind* bind_handle = nullptr;
        // set<T>'s elements are always const when iterated (mutating one
        // could violate the set's ordering invariant); const_cast matches
        // the same read-only-through-a-mutable-pointer pattern raw_bind_args
        // already uses for std::string_view content.
        auto [value_ptr, bind_size] = raw_bind_args(const_cast<ElemType&>(elem));
        OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    value_ptr, bind_size, oci_type_code_v<ElemType>,
                    &elem_indicators[idx], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        ++idx;
    }
}

template <std::size_t I, bindable T>
void bind_one_field(OCIStmt* stmt, OciConnection& conn, T& row,
                     std::vector<OCILobLocator**>& active_locators,
                     std::vector<sb2>& indicators, std::vector<std::vector<sb2>>& container_indicators,
                     staging_tuple_t<T>& staging, std::string_view field_name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(row);
    const std::string placeholder = ":" + std::string(field_name);
    OCIBind* bind_handle = nullptr;
    indicators[I] = OCI_IND_NOTNULL;

    if constexpr (is_multi_bind_container_v<FieldType>) {
        bind_named_container(stmt, conn, field, field_name, container_indicators[I]);
    } else if constexpr (is_oci_lob_v<FieldType>) {
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
                       std::vector<sb2>& indicators, std::vector<std::vector<sb2>>& container_indicators,
                       staging_tuple_t<T>& staging, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_field<Is>(stmt, conn, row, active_locators, indicators, container_indicators, staging, names[Is]), ...);
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
// A field that's std::optional and empty binds SQL NULL. A field that's a
// std::vector/std::set/std::valarray<U> binds as a dynamic multi-value
// IN-list instead of a single value -- see bind_named_container and
// substitute_container_markers.
template <bindable T>
void bind_fields(OCIStmt* stmt, OciConnection& conn, T& row,
                  std::vector<OCILobLocator**>& active_locators,
                  std::vector<sb2>& indicators, std::vector<std::vector<sb2>>& container_indicators,
                  staging_tuple_t<T>& staging) {
    bind_fields_impl(stmt, conn, row, active_locators, indicators, container_indicators, staging,
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
    static_assert(!is_multi_bind_container_v<FieldType>,
                  "vector/set/valarray fields are an execute()/insert() input-side concept only "
                  "(a dynamic multi-value IN-list parameter) -- a single result column can't "
                  "fetch into a variable-length container; that's multiple rows, not one field.");

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

// If field I is a vector/set/valarray<U>, replaces that field's own
// "{field_name}" marker in `sql` with a named placeholder list sized to
// the field's current element count (see make_named_placeholders) --
// e.g. field `trade_ids` with 3 elements turns
// "... IN ({trade_ids})" into "... IN (:trade_ids_0,:trade_ids_1,:trade_ids_2)"
// before OCIStmtPrepare, since Oracle needs the exact placeholder count
// fixed in the SQL text itself. A missing marker for a container field is
// a caller mistake (forgot to write "{field_name}" where the values
// should go) and throws, rather than silently preparing a statement whose
// binds have nowhere to land. No-op for every other field type.
template <std::size_t I, bindable T>
void substitute_one_container_marker(std::string& sql, const T& row, std::string_view field_name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    if constexpr (is_multi_bind_container_v<FieldType>) {
        const auto& container = boost::pfr::get<I>(row);
        sql = substitute_marker(sql, field_name, make_named_placeholders(field_name, container.size()));
    }
}

template <bindable T, std::size_t... Is>
std::string substitute_container_markers_impl(const std::string& query_text, const T& row, std::index_sequence<Is...>) {
    std::string sql = query_text;
    constexpr auto names = boost::pfr::names_as_array<T>();
    (substitute_one_container_marker<Is>(sql, row, names[Is]), ...);
    return sql;
}

// Returns `query_text` with every vector/set/valarray field's own
// "{field_name}" marker replaced by a placeholder list sized to that
// field's current contents -- a no-op (returns query_text unchanged) for a
// T with no such fields.
template <bindable T>
std::string substitute_container_markers(const std::string& query_text, const T& row) {
    return substitute_container_markers_impl(query_text, row, std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

} // namespace detail

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
    // SQL NULL when it's empty. A field declared as
    // std::vector/std::set/std::valarray<U> binds as a dynamic multi-value
    // IN-list -- its own `{field_name}` marker in `query_text` (not
    // `:field_name`) gets replaced with a placeholder list sized to that
    // field's element count before preparing (see
    // detail::substitute_container_markers/bind_named_container).
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
    // back as std::nullopt when that column is NULL for the row; string,
    // LOB, and vector/set/valarray output columns aren't supported here
    // (see define_one_field) -- for a dynamic-sized result set, see
    // select_with_in_collection() in oci_collection_bind.h, which still
    // returns std::vector<RowT>, just via a bound IN-list rather than a
    // container-typed column.
    //
    // On a disconnect mid-fetch, results are cleared and the whole SELECT is
    // re-run from scratch on reconnect (there's no cursor to resume from).
    template <bindable T>
    bool select(OciConnection& conn, const std::string& query_text, std::vector<T>& results) {
        return conn.run_with_reconnect([&]() -> OciOutcome {
            return run_select_once(conn, query_text, results);
        });
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
        // A vector/set/valarray field's own "{field_name}" marker gets
        // replaced with a placeholder list sized to that field's current
        // element count -- a no-op if T has no such field, so this is safe
        // (and cheap) to call unconditionally.
        const std::string sql = detail::substitute_container_markers(query_text, bind_struct);

        OCIStmt* stmt = nullptr;
        OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);

        OCIStmtPrepare(stmt, conn.err(),
                       reinterpret_cast<const text*>(sql.c_str()),
                       static_cast<ub4>(sql.size()),
                       OCI_NTV_SYNTAX, OCI_DEFAULT);

        std::vector<OCILobLocator**> active_locators;
        std::vector<sb2> indicators(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
        std::vector<std::vector<sb2>> container_indicators(boost::pfr::tuple_size_v<T>);
        detail::staging_tuple_t<T> staging{};
        detail::bind_fields(stmt, conn, bind_struct, active_locators, indicators, container_indicators, staging);

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
};

} // namespace binding
