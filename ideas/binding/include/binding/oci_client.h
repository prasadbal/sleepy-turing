#pragma once
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <valarray>
#include <vector>

#include "binding/oci_connection.h"
#include "binding/oci_fixed_string.h"
#include "binding/oci_lob.h"
#include "binding/reflect.h"

namespace binding {

// ----------------------------------------------------------------------------
// Compile-time OCI external type code mapping. Add a specialization for any
// new leaf/LOB type this client should know how to bind or define.
// ----------------------------------------------------------------------------
// The primary template is deliberately a hard error rather than being left
// undeclared: `bindable` accepts any arithmetic field (see reflect.h's
// is_bindable_leaf), which is a strictly larger set than the types this map
// knows how to bind. Without this static_assert, a field type with no entry
// here failed as "incomplete type OciTypeBinder<X> used in nested name
// specifier" from deep inside oci_type_code_of, naming neither the field nor
// the struct it came from.
template <typename T>
struct OciTypeBinder {
    static_assert(sizeof(T) == 0,
                  "binding: no OCI external type code is defined for this field type. "
                  "Supported: the integer types, float/double, std::string (bind side only), "
                  "FixedString<N> (binding/oci_fixed_string.h) for a CHAR/VARCHAR2 column, and "
                  "OciClob/OciXml. Note bool has no Oracle SQL counterpart -- use a numeric "
                  "flag or a FixedString<1>. Specialize OciTypeBinder for anything else.");
    static constexpr ub2 type_code = 0; // never reached; keeps the assert above the only error
};

// Integer widths all bind through SQLT_INT/SQLT_UIN -- OCI takes the actual
// width from the bind/define value_sz, so one type code covers every size.
// These were missing before, which meant an ordinary std::int64_t or
// std::uint32_t id field satisfied `bindable` and then failed to compile.
template <> struct OciTypeBinder<short>              { static constexpr ub2 type_code = SQLT_INT;     };
template <> struct OciTypeBinder<int>                { static constexpr ub2 type_code = SQLT_INT;     };
template <> struct OciTypeBinder<long>               { static constexpr ub2 type_code = SQLT_INT;     };
template <> struct OciTypeBinder<long long>          { static constexpr ub2 type_code = SQLT_INT;     };
template <> struct OciTypeBinder<unsigned short>     { static constexpr ub2 type_code = SQLT_UIN;     };
template <> struct OciTypeBinder<unsigned int>       { static constexpr ub2 type_code = SQLT_UIN;     };
template <> struct OciTypeBinder<unsigned long>      { static constexpr ub2 type_code = SQLT_UIN;     };
template <> struct OciTypeBinder<unsigned long long> { static constexpr ub2 type_code = SQLT_UIN;     };

// float is kept for compatibility but is a poor fit for anything money- or
// sensitivity-shaped: SQLT_BFLOAT is a binary single, ~7 significant digits.
template <> struct OciTypeBinder<float>       { static constexpr ub2 type_code = SQLT_BFLOAT;  };
template <> struct OciTypeBinder<double>      { static constexpr ub2 type_code = SQLT_BDOUBLE; };
template <> struct OciTypeBinder<std::string> { static constexpr ub2 type_code = SQLT_STR;     };
template <> struct OciTypeBinder<OciClob>     { static constexpr ub2 type_code = SQLT_CLOB;    };
template <> struct OciTypeBinder<OciXml>      { static constexpr ub2 type_code = SQLT_CLOB;    };

// FixedString<N> binds and defines as SQLT_CHR: an explicit-length VARCHAR2
// with no null terminator required, which is what lets it work as a select()
// output column (the buffer size OCI needs up front is N) as well as an
// input. A CHAR(N) column comes back blank-padded to N, as it would through
// any OCI client.
template <std::size_t N> struct OciTypeBinder<FixedString<N>> { static constexpr ub2 type_code = SQLT_CHR; };

// std::optional<U> takes U's type code -- the indicator, not the type code,
// is what tells OCI a value is NULL.
template <typename T> struct oci_type_code_of { static constexpr ub2 value = OciTypeBinder<std::remove_cv_t<T>>::type_code; };
template <typename U> struct oci_type_code_of<std::optional<U>> { static constexpr ub2 value = OciTypeBinder<U>::type_code; };
template <typename T> inline constexpr ub2 oci_type_code_v = oci_type_code_of<T>::value;

// ----------------------------------------------------------------------------
// std::vector<U>/std::set<U>/std::valarray<U> recognition, for a struct
// field meant to bind as a dynamic multi-value IN-list parameter (see
// docs/in_list_binding.md) rather than a single scalar. Distinct from
// reflect.h's is_vector_v/vector_value_t, which is about config's
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
// side only -- select()'s output side static_asserts against this) a
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
// docs/in_list_binding.md), but named after the field itself
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
// no cap. See docs/in_list_binding.md for the tradeoffs between the two.
std::string make_named_placeholders(std::string_view field_name, std::size_t count);

// ----------------------------------------------------------------------------
// Core database execution client.
//
// Names read as SQL verbs: execute() runs any statement that doesn't return
// rows (DDL, or DML with or without a bound struct); insert() is a
// same-mechanism, intent-naming alias for execute() that also adds a
// std::vector<T> overload for inserting several rows; select() runs a query
// and returns its rows.
//
// Implementation in details/oci_client.h.
// ----------------------------------------------------------------------------
class OciClient {
public:
    // Runs any SQL statement that returns no rows and needs no bind
    // parameters -- DDL (CREATE/ALTER/TRUNCATE), or DML with everything
    // already literal in the text. For a statement with bind parameters,
    // use the execute(conn, query_text, bind_struct) overload below.
    bool execute(OciConnection& conn, const std::string& query_text);

    // Runs any SQL statement that returns no rows (INSERT/UPDATE/DELETE/
    // MERGE/a stored-procedure call/..., including RETURNING ... INTO),
    // binding bind_struct's fields as named IN parameters. Each field binds
    // to a `:field_name` placeholder in `query_text` by its own
    // (compiler-derived) name -- e.g. field `bonus_pct` binds `:bonus_pct`
    // wherever that placeholder appears, in any order, any number of times.
    // A field declared as std::optional<U> binds SQL NULL when it's empty.
    // A field declared as std::vector/std::set/std::valarray<U> binds as a
    // dynamic multi-value IN-list -- its own `{field_name}` marker in
    // `query_text` (not `:field_name`) gets replaced with a placeholder
    // list sized to that field's element count before preparing (see
    // docs/in_list_binding.md).
    //
    // conn.run_with_reconnect() reconnects and retries the whole statement
    // only on a disconnect-class error; an ordinary execution error (bad
    // SQL, constraint violation, etc.) is returned immediately, un-retried.
    //
    // Note: passes OCI_DEFAULT (no autocommit) to OCIStmtExecute -- deciding
    // transaction/commit boundaries is left to the caller (OCITransCommit or
    // OCI_COMMIT_ON_SUCCESS), it's out of scope for this reconnect scaffold.
    template <bindable T>
    bool execute(OciConnection& conn, const std::string& query_text, T& bind_struct);

    // Same mechanism as execute(conn, query_text, bind_struct) -- an alias
    // that reads as intent for the common case where the statement actually
    // is an INSERT.
    template <bindable T>
    bool insert(OciConnection& conn, const std::string& query_text, T& row);

    // Inserts several rows as a real Oracle array bind: one OCIBindByName
    // per field (pointing at rows[0]'s field) followed by
    // OCIBindArrayOfStruct (stride sizeof(T) to the next row's value,
    // sizeof(sb2) to the next row's indicator), then a single
    // OCIStmtExecute with iters = rows.size() -- one network round trip
    // for the whole batch, reading each field's values directly out of
    // `rows`' own contiguous storage rather than staging/copying them
    // anywhere first.
    //
    // Scope: only a plain (non-optional) arithmetic leaf field binds this
    // way today -- its bytes already sit inline in T at a fixed offset,
    // which is exactly what a fixed-stride array bind needs. std::string,
    // std::optional<U>, LOB, and vector/set/valarray fields all
    // static_assert here instead of silently doing the wrong thing:
    //   - std::string's characters live in the string object's own
    //     heap/SSO storage, not inline in T, so there's no fixed-width
    //     value at a fixed stride to bind directly -- needs a
    //     fixed-capacity string type (see oci_fixed_string.h, not yet
    //     wired in here) before this can work.
    //   - std::optional<U> would need every row's optional engaged (an
    //     empty one has no address to bind through) and relies on an
    //     implementation-defined payload offset being consistent across
    //     rows -- fragile enough to defer rather than support today.
    //   - LOB needs a per-row array of locators, not implemented.
    //   - vector/set/valarray is a single query's dynamic IN-list, not a
    //     per-row column value -- it has no bulk-insert meaning at all.
    // For a row type with any of those fields, insert each row in a loop
    // via insert(conn, query_text, T&) instead.
    //
    // On failure the whole batch is un-retried-as-a-batch by
    // run_with_reconnect the same way every other method here is:
    // reconnect-and-redo-the-whole-call only on a disconnect-class error.
    // Transaction/commit boundaries remain the caller's responsibility.
    template <bindable T>
    bool insert(OciConnection& conn, const std::string& query_text, std::vector<T>& rows);

    // Runs a SELECT with no bind parameters and returns its rows -- the
    // "vector<S> as a result set" case, for a query that's fully literal in
    // the text (or has no WHERE clause at all). For a query that also needs
    // bind parameters, use the select(conn, query_text, input, results)
    // overload below.
    //
    // Column order in `query_text`'s SELECT list must match OutputT's
    // declared field order: unlike execute()/insert(), which bind by name,
    // OCIDefineByPos is the *only* way to bind output columns in raw OCI --
    // there is no OCIDefineByName. That's an OCI limitation, not a choice
    // made here, so this side stays positional regardless of boost::pfr's
    // name-reflection support. A field declared as std::optional<U> comes
    // back as std::nullopt when that column is NULL for the row; string,
    // LOB, and vector/set/valarray output columns aren't supported here --
    // for a dynamic-sized result set, see select_with_in_collection() in
    // oci_collection_bind.h, which still returns std::vector<RowT>, just
    // via a bound IN-list rather than a container-typed column.
    //
    // On a disconnect mid-fetch, results are cleared and the whole SELECT is
    // re-run from scratch on reconnect (there's no cursor to resume from).
    template <bindable OutputT>
    bool select(OciConnection& conn, const std::string& query_text, std::vector<OutputT>& results);

    // Runs a SELECT that also binds parameters from `input`'s fields --
    // e.g. "SELECT trade_id, notional FROM trades WHERE status = :status",
    // with `input.status` bound by name exactly as execute(conn, query_text,
    // bind_struct) binds its struct (same by-name rules, same {field_name}
    // container-marker mechanism for a vector/set/valarray field in
    // `input`). `results`' column-order/type rules are exactly the
    // no-input overload above -- `input` only ever supplies parameters,
    // never result columns.
    template <bindable InputT, bindable OutputT>
    bool select(OciConnection& conn, const std::string& query_text, InputT& input, std::vector<OutputT>& results);

private:
    OciOutcome run_execute_once(OciConnection& conn, const std::string& query_text);

    template <bindable T>
    OciOutcome run_execute_once(OciConnection& conn, const std::string& query_text, T& bind_struct);

    template <bindable T>
    OciOutcome run_insert_array_once(OciConnection& conn, const std::string& query_text, std::vector<T>& rows);

    template <bindable OutputT>
    OciOutcome run_select_once(OciConnection& conn, const std::string& query_text, std::vector<OutputT>& results);

    template <bindable InputT, bindable OutputT>
    OciOutcome run_select_once(OciConnection& conn, const std::string& query_text,
                                InputT& input, std::vector<OutputT>& results);
};

} // namespace binding

#include "binding/details/oci_client.h"
