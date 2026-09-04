#pragma once
// Implementation of binding::OciClient and its supporting free functions --
// see binding/oci_client.h for the interface and behavioral contract. Not
// meant to be included directly.
#include "binding/oci_client.h"

#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

#include <boost/pfr.hpp>

namespace binding {

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
        // +1 for the trailing null: OciTypeBinder<std::string> binds as
        // SQLT_STR, which requires the null terminator to fall *within*
        // the declared buffer size, or OCI rejects it with ORA-01480
        // ("trailing null character is missing from SQLT_STR type bind
        // data") -- confirmed against a real database; the mock doesn't
        // model this, so a plain std::string field bound via
        // execute()/select()'s input side (or a std::set<std::string>
        // container field, which reuses this same helper) never
        // exercised this until then. Safe because ValueType here is a
        // real std::string (the only string-convertible leaf type in
        // this library's bindable_predicate), and std::string guarantees
        // data()[size()] == '\0' since C++11 -- sv.size() bytes of
        // content plus that guaranteed null both fall inside the
        // buffer this reports.
        return { reinterpret_cast<dvoid*>(const_cast<char*>(sv.data())), static_cast<sb4>(sv.size() + 1) };
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

// ---- bind (insert(vector<T>&): a real Oracle array bind) -------------------

// Binds field I of every row in `rows` at once: OCIBindByName against
// rows[0]'s field, then OCIBindArrayOfStruct telling OCI the byte stride
// to the same field in the next row (sizeof(T), since rows is one
// contiguous std::vector<T>) and to the next row's indicator (sizeof(sb2),
// since field_indicators[I] is its own tightly-packed std::vector<sb2>
// sized rows.size()). No staging, no copying -- OCI reads each value
// directly out of `rows`' own storage during OCIStmtExecute.
//
// See the doc comment on OciClient::insert(vector<T>&) in oci_client.h
// for exactly which field kinds this does and doesn't support and why.
template <std::size_t I, bindable T>
void bind_one_field_array(OCIStmt* stmt, OciConnection& conn, std::vector<T>& rows,
                           std::vector<std::vector<sb2>>& field_indicators, std::string_view field_name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    static_assert(!is_oci_lob_v<FieldType>,
                  "LOB fields are not supported in bulk insert(vector<T>&) yet -- array-binding "
                  "a LOB column needs a per-row array of LOB locators, not implemented here; "
                  "insert(conn, query_text, T&) in a loop for a LOB-bearing row type instead.");
    static_assert(!std::is_convertible_v<FieldType, std::string_view> &&
                  !(is_optional_v<FieldType> && std::is_convertible_v<optional_value_t<FieldType>, std::string_view>),
                  "std::string fields are not supported in bulk insert(vector<T>&) yet -- a "
                  "string's characters live in its own heap/SSO storage, not inline in T at a "
                  "fixed stride, which a real Oracle array bind needs; a fixed-capacity string "
                  "type (see oci_fixed_string.h) would work here once it's wired in.");
    static_assert(!is_multi_bind_container_v<FieldType>,
                  "a vector/set/valarray field has no per-row bulk-insert meaning -- it's a "
                  "single query's dynamic IN-list, not a per-row column value.");
    static_assert(!is_optional_v<FieldType>,
                  "std::optional fields are not supported in bulk insert(vector<T>&) yet -- "
                  "every row's optional would need to be engaged (an empty one has no address "
                  "to bind through), relying on an implementation-defined payload offset being "
                  "consistent across rows; use insert(conn, query_text, T&) in a loop for a "
                  "row type with a nullable field instead.");

    auto& indicators = field_indicators[I];
    indicators.assign(rows.size(), OCI_IND_NOTNULL);

    const std::string placeholder = ":" + std::string(field_name);
    OCIBind* bind_handle = nullptr;
    OCIBindByName(stmt, &bind_handle, conn.err(),
                reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                reinterpret_cast<dvoid*>(&boost::pfr::get<I>(rows[0])), static_cast<sb4>(sizeof(FieldType)),
                oci_type_code_v<FieldType>, indicators.data(), nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    OCIBindArrayOfStruct(bind_handle, conn.err(),
                          static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)), 0, 0);
}

template <bindable T, std::size_t... Is>
void bind_fields_array_impl(OCIStmt* stmt, OciConnection& conn, std::vector<T>& rows,
                             std::vector<std::vector<sb2>>& field_indicators, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_field_array<Is>(stmt, conn, rows, field_indicators, names[Is]), ...);
}

template <bindable T>
void bind_fields_array(OCIStmt* stmt, OciConnection& conn, std::vector<T>& rows,
                        std::vector<std::vector<sb2>>& field_indicators) {
    bind_fields_array_impl(stmt, conn, rows, field_indicators,
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

// ---- define (select(): SELECT columns -> struct, as an array-of-struct
// fetch -- see docs/in_list_binding.md's sibling discussion for why this
// batches by rows rather than fetching one row per OCIStmtFetch2 call) ----

// Batch size for one OCIStmtFetch2 call in select(). This is about C++-side
// overhead, not network round trips -- Oracle's own client-side prefetch
// cache (OCI_ATTR_PREFETCH_ROWS, set in run_select_once below) already
// batches round trips independently of how many rows any single
// OCIStmtFetch2 call asks for (confirmed empirically: fetching 500 rows
// one at a time with prefetch_rows=500 took 2 round trips total, same as
// fetching them 500-at-a-time would). What this constant actually buys:
// results.insert()-ing kSelectBatchRows rows at once instead of push_back
// one row at a time, and kSelectBatchRows/(fields) fewer OCIStmtFetch2
// calls -- real savings for a huge result set, unrelated to network cost.
inline constexpr std::size_t kSelectBatchRows = 100;

// Per-field batch staging: like staging_slot_t (scalar bind side), but a
// std::vector<U> sized to the batch instead of a single U -- an array of
// std::optional<U> has no OCI-compatible fixed-stride representation to
// define directly into, same reason a single std::optional<U> doesn't
// either, so each optional field still needs its own staging storage,
// just one slot per row in the batch instead of one slot total.
template <typename FieldType>
using batch_staging_slot_t = std::conditional_t<is_optional_v<FieldType>, std::vector<optional_value_t<FieldType>>, std::monostate>;

template <typename T, std::size_t... Is>
constexpr auto batch_staging_tuple_type(std::index_sequence<Is...>)
    -> std::tuple<batch_staging_slot_t<boost::pfr::tuple_element_t<Is, T>>...>;

template <typename T>
using batch_staging_tuple_t = decltype(batch_staging_tuple_type<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

// Defines field I of every row in `batch` at once: OCIDefineByPos against
// batch[0]'s field (or, for std::optional<U>, that field's own staging
// array), then OCIDefineArrayOfStruct telling OCI the byte stride to the
// same field in the next row (sizeof(T), since batch is one contiguous
// std::vector<T>) and to the next row's indicator (sizeof(sb2), since
// field_indicators[I] is its own tightly-packed std::vector<sb2> sized to
// the batch). Same field-kind restrictions as the bind side's array
// helper, for the same reasons -- see oci_client.h's select() doc comment.
template <std::size_t I, bindable T>
void define_one_field_array(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                             std::vector<std::vector<sb2>>& field_indicators,
                             batch_staging_tuple_t<T>& batch_staging) {
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

    constexpr ub4 position = I + 1;
    auto& indicators = field_indicators[I];
    indicators.assign(batch.size(), OCI_IND_NOTNULL);
    OCIDefine* define_handle = nullptr;

    if constexpr (is_optional_v<FieldType>) {
        using ElemType = optional_value_t<FieldType>;
        auto& stage = std::get<I>(batch_staging);
        stage.assign(batch.size(), ElemType{});
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                      reinterpret_cast<dvoid*>(stage.data()), static_cast<sb4>(sizeof(ElemType)),
                      oci_type_code_v<FieldType>, indicators.data(), nullptr, nullptr, OCI_DEFAULT);
        OCIDefineArrayOfStruct(define_handle, conn.err(),
                                static_cast<ub4>(sizeof(ElemType)), static_cast<ub4>(sizeof(sb2)), 0, 0);
    } else {
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                      reinterpret_cast<dvoid*>(&boost::pfr::get<I>(batch[0])), static_cast<sb4>(sizeof(FieldType)),
                      oci_type_code_v<FieldType>, indicators.data(), nullptr, nullptr, OCI_DEFAULT);
        OCIDefineArrayOfStruct(define_handle, conn.err(),
                                static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)), 0, 0);
    }
}

template <bindable T, std::size_t... Is>
void define_fields_array_impl(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                               std::vector<std::vector<sb2>>& field_indicators,
                               batch_staging_tuple_t<T>& batch_staging, std::index_sequence<Is...>) {
    (define_one_field_array<Is>(stmt, conn, batch, field_indicators, batch_staging), ...);
}

template <bindable T>
void define_fields_array(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                          std::vector<std::vector<sb2>>& field_indicators,
                          batch_staging_tuple_t<T>& batch_staging) {
    define_fields_array_impl(stmt, conn, batch, field_indicators, batch_staging,
                              std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

// After a fetch, applies what the indicator said for field I of row
// `row_idx` within the batch:
//   - std::optional<U>: the field was defined into that field's own
//     per-row staging slot, not into the struct directly -- copy it in,
//     or reset to nullopt, depending on the indicator.
//   - anything else (a "required" field): a NULL indicator is an error.
//     There's no schema/DESCRIBE metadata available to check ahead of time
//     whether a column can be NULL -- this is the only point a NULL landing
//     on a non-optional field can be caught at all. Throwing here, rather
//     than returning a retriable failure, is deliberate: no reconnect/retry
//     can ever fix a real data/schema mismatch like this, so retrying it
//     would just waste a retry budget reproducing the same throw.
template <std::size_t I, bindable T>
void apply_field_null_semantics_array(T& row, std::size_t row_idx,
                                       const std::vector<std::vector<sb2>>& field_indicators,
                                       const batch_staging_tuple_t<T>& batch_staging, std::string_view field_name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    const sb2 indicator = field_indicators[I][row_idx];
    if constexpr (is_optional_v<FieldType>) {
        auto& field = boost::pfr::get<I>(row);
        field = (indicator == OCI_IND_NULL) ? std::nullopt
                                             : std::make_optional(std::get<I>(batch_staging)[row_idx]);
    } else if (indicator == OCI_IND_NULL) {
        throw std::runtime_error(
            "binding: column bound to field '" + std::string(field_name) +
            "' returned SQL NULL, but that field is not declared std::optional<T> -- "
            "wrap it in std::optional<T> if this column can be NULL");
    }
}

template <bindable T, std::size_t... Is>
void apply_null_semantics_after_fetch_array(T& row, std::size_t row_idx,
                                             const std::vector<std::vector<sb2>>& field_indicators,
                                             const batch_staging_tuple_t<T>& batch_staging, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (apply_field_null_semantics_array<Is>(row, row_idx, field_indicators, batch_staging, names[Is]), ...);
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

// Shared by both run_select_once overloads (with and without an input
// bind struct): defines OutputT's columns as one array-of-struct batch,
// sets OCI_ATTR_PREFETCH_ROWS (Oracle's own client-side round-trip
// batching -- see kSelectBatchRows's comment for why this and the array
// fetch below are solving two different problems, not the same one),
// executes, then repeatedly fetches up to kSelectBatchRows rows per
// OCIStmtFetch2 call and appends however many actually came back
// (OCI_ATTR_ROWS_FETCHED) to `results` in one bulk insert. Does not free
// `stmt` -- that (and, for the with-input overload, its own input
// locators) stays the caller's responsibility, since what needs freeing
// alongside it differs between the two overloads.
template <bindable OutputT>
OciOutcome run_select_fetch_loop(OciConnection& conn, OCIStmt* stmt, std::vector<OutputT>& results) {
    ub4 prefetch_rows = static_cast<ub4>(kSelectBatchRows);
    OCIAttrSet(stmt, OCI_HTYPE_STMT, &prefetch_rows, 0, OCI_ATTR_PREFETCH_ROWS, conn.err());

    std::vector<OutputT> batch(kSelectBatchRows);
    std::vector<std::vector<sb2>> field_indicators(boost::pfr::tuple_size_v<OutputT>);
    batch_staging_tuple_t<OutputT> batch_staging{};
    define_fields_array(stmt, conn, batch, field_indicators, batch_staging);

    sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 0, 0, nullptr, nullptr, OCI_DEFAULT);
    if (status != OCI_SUCCESS) {
        return {false, status};
    }

    for (;;) {
        status = OCIStmtFetch2(stmt, conn.err(), static_cast<ub4>(kSelectBatchRows), OCI_FETCH_NEXT, 0, OCI_DEFAULT);
        if (status != OCI_SUCCESS && status != OCI_NO_DATA) {
            return {false, status};
        }

        // The call that returns the last (possibly partial) batch reports
        // OCI_NO_DATA directly, with OCI_ATTR_ROWS_FETCHED still holding
        // however many valid rows it wrote -- confirmed against a real
        // database, not assumed from documentation. Handling both
        // statuses the same way here (read the count, process that many,
        // then check status to decide whether to loop again) covers both
        // that case and the exact-multiple-of-batch-size case, where a
        // final all-zero OCI_NO_DATA call follows a full OCI_SUCCESS batch.
        ub4 rows_fetched = 0;
        ub4 attr_size = sizeof(rows_fetched);
        OCIAttrGet(stmt, OCI_HTYPE_STMT, &rows_fetched, &attr_size, OCI_ATTR_ROWS_FETCHED, conn.err());

        for (ub4 i = 0; i < rows_fetched; ++i) {
            apply_null_semantics_after_fetch_array(batch[i], i, field_indicators, batch_staging,
                                                    std::make_index_sequence<boost::pfr::tuple_size_v<OutputT>>{});
        }
        results.insert(results.end(), batch.begin(), batch.begin() + rows_fetched);

        if (status == OCI_NO_DATA) break;
    }
    return {true, OCI_SUCCESS};
}

} // namespace detail

inline bool OciClient::execute(OciConnection& conn, const std::string& query_text) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_execute_once(conn, query_text);
    });
}

template <bindable T>
bool OciClient::execute(OciConnection& conn, const std::string& query_text, T& bind_struct) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_execute_once(conn, query_text, bind_struct);
    });
}

template <bindable T>
bool OciClient::insert(OciConnection& conn, const std::string& query_text, T& row) {
    return execute(conn, query_text, row);
}

template <bindable T>
bool OciClient::insert(OciConnection& conn, const std::string& query_text, std::vector<T>& rows) {
    if (rows.empty()) return true; // nothing to bind rows[0]'s address through
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_insert_array_once(conn, query_text, rows);
    });
}

template <bindable OutputT>
bool OciClient::select(OciConnection& conn, const std::string& query_text, std::vector<OutputT>& results) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_select_once(conn, query_text, results);
    });
}

template <bindable InputT, bindable OutputT>
bool OciClient::select(OciConnection& conn, const std::string& query_text,
                        InputT& input, std::vector<OutputT>& results) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_select_once(conn, query_text, input, results);
    });
}

inline OciOutcome OciClient::run_execute_once(OciConnection& conn, const std::string& query_text) {
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
OciOutcome OciClient::run_execute_once(OciConnection& conn, const std::string& query_text, T& bind_struct) {
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
OciOutcome OciClient::run_insert_array_once(OciConnection& conn, const std::string& query_text,
                                             std::vector<T>& rows) {
    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(),
                   reinterpret_cast<const text*>(query_text.c_str()),
                   static_cast<ub4>(query_text.size()),
                   OCI_NTV_SYNTAX, OCI_DEFAULT);

    std::vector<std::vector<sb2>> field_indicators(boost::pfr::tuple_size_v<T>);
    detail::bind_fields_array(stmt, conn, rows, field_indicators);

    const sword status = OCIStmtExecute(conn.svc(), stmt, conn.err(),
                                         static_cast<ub4>(rows.size()), 0, nullptr, nullptr, OCI_DEFAULT);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return {status == OCI_SUCCESS, status};
}

template <bindable OutputT>
OciOutcome OciClient::run_select_once(OciConnection& conn, const std::string& query_text,
                                       std::vector<OutputT>& results) {
    results.clear();

    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(),
                   reinterpret_cast<const text*>(query_text.c_str()),
                   static_cast<ub4>(query_text.size()),
                   OCI_NTV_SYNTAX, OCI_DEFAULT);

    const OciOutcome outcome = detail::run_select_fetch_loop(conn, stmt, results);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return outcome;
}

// Same as the no-input overload above, but also binds `input`'s fields as
// named parameters (see detail::bind_fields) before defining the output
// columns and executing -- the read-side counterpart to
// run_execute_once(conn, query_text, bind_struct). `input`'s own
// {field_name} container markers (a vector/set/valarray field) are
// substituted exactly as on the execute() side.
template <bindable InputT, bindable OutputT>
OciOutcome OciClient::run_select_once(OciConnection& conn, const std::string& query_text,
                                       InputT& input, std::vector<OutputT>& results) {
    results.clear();

    const std::string sql = detail::substitute_container_markers(query_text, input);

    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(),
                   reinterpret_cast<const text*>(sql.c_str()),
                   static_cast<ub4>(sql.size()),
                   OCI_NTV_SYNTAX, OCI_DEFAULT);

    std::vector<OCILobLocator**> input_locators;
    std::vector<sb2> input_indicators(boost::pfr::tuple_size_v<InputT>, OCI_IND_NOTNULL);
    std::vector<std::vector<sb2>> input_container_indicators(boost::pfr::tuple_size_v<InputT>);
    detail::staging_tuple_t<InputT> input_staging{};
    detail::bind_fields(stmt, conn, input, input_locators, input_indicators, input_container_indicators, input_staging);

    const OciOutcome outcome = detail::run_select_fetch_loop(conn, stmt, results);

    detail::free_locators(input_locators);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return outcome;
}

} // namespace binding
