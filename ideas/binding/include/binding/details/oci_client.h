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

// Replaces *every* occurrence of "{marker_name}" in `query_template` with
// `placeholders` -- used for the per-field container marker (that field's
// own name, see detail::substitute_container_markers).
//
// All occurrences, not just the first: a container field can legitimately be
// matched against more than one column ("WHERE a IN ({ids}) OR b IN ({ids})"),
// and replacing only the first left a literal "{ids}" in the SQL handed to
// OCIStmtPrepare, surfacing as an opaque ORA-00911. Every expansion generates
// the same placeholder names, and a placeholder repeated in one statement is
// a single bind (see "on placeholder reuse" in README.md), so the repeated
// list binds correctly against one set of values.
inline std::string substitute_marker(const std::string& query_template, std::string_view marker_name,
                                      const std::string& placeholders) {
    const std::string marker = "{" + std::string(marker_name) + "}";
    if (query_template.find(marker) == std::string::npos) {
        throw std::runtime_error("binding: query template is missing the " + marker + " placeholder marker");
    }
    std::string result;
    result.reserve(query_template.size());
    std::size_t pos = 0;
    for (;;) {
        const auto hit = query_template.find(marker, pos);
        if (hit == std::string::npos) {
            result.append(query_template, pos, std::string::npos);
            return result;
        }
        result.append(query_template, pos, hit - pos);
        result += placeholders;
        pos = hit + marker.size();
    }
}

inline std::string make_in_placeholders(std::size_t count, ub4 start_position) {
    if (count == 0) return "NULL";

    constexpr std::size_t oracle_max_in_list_size = 1000;
    if (count > oracle_max_in_list_size) {
        throw std::runtime_error(
            "binding: IN list has " + std::to_string(count) +
            " elements, but Oracle limits a plain IN (...) list to " +
            std::to_string(oracle_max_in_list_size) +
            " (ORA-01795) -- use select_with_in_collection() for larger lists");
    }

    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
        if (i) result += ',';
        result += ':' + std::to_string(start_position + i);
    }
    return result;
}

// Replaces the "{IN}" marker in `query_template` -- the fixed marker name
// for select_with_in_list()/execute_with_in_list()'s standalone id
// collection, as opposed to substitute_container_markers' per-field
// "{field_name}" markers (a struct has no single field here to name it
// after).
inline std::string substitute_in_marker(const std::string& query_template, const std::string& placeholders) {
    return substitute_marker(query_template, "IN", placeholders);
}

namespace detail {

// Keeps the first failing status seen while walking a struct's fields, so a
// fold over every field still reports which call actually failed. Every OCI
// call in this file is now checked: an unchecked failure leaves a
// half-configured statement that later calls then run against, the same class
// of bug the connect() rewrite (see README.md) removed from the setup path.
inline void record_status(sword& accumulated, sword status) noexcept {
    if (accumulated == OCI_SUCCESS && status != OCI_SUCCESS) accumulated = status;
}

// Owns an OCI statement handle for the duration of one attempt. Needed
// because several paths through this file throw -- apply_field_null_semantics
// on an unexpected NULL, make_named_placeholders past the ORA-01795 cap,
// substitute_marker on a missing marker -- and an explicit OCIHandleFree at
// the end of a method is skipped entirely when the stack unwinds through it,
// leaking one statement handle per throw over a long reporting batch.
class StmtHandle {
public:
    StmtHandle() = default;
    ~StmtHandle() { if (stmt_) OCIHandleFree(stmt_, OCI_HTYPE_STMT); }

    StmtHandle(const StmtHandle&) = delete;
    StmtHandle& operator=(const StmtHandle&) = delete;

    sword alloc(OCIEnv* env) {
        return OCIHandleAlloc(env, reinterpret_cast<void**>(&stmt_), OCI_HTYPE_STMT, 0, nullptr);
    }

    OCIStmt* get() const noexcept { return stmt_; }

private:
    OCIStmt* stmt_ = nullptr;
};

// Allocates and prepares in one checked step. A failure here (a null env
// after a failed reconnect, a handle alloc that ran out, a statement the
// server rejects at prepare) previously went unnoticed, and execution
// continued on to OCIStmtExecute against a null or unprepared handle.
inline sword prepare_statement(OciConnection& conn, StmtHandle& stmt, const std::string& sql) {
    const sword alloc_status = stmt.alloc(conn.env());
    if (alloc_status != OCI_SUCCESS) return alloc_status;
    return OCIStmtPrepare(stmt.get(), conn.err(),
                          reinterpret_cast<const text*>(sql.c_str()),
                          static_cast<ub4>(sql.size()),
                          OCI_NTV_SYNTAX, OCI_DEFAULT);
}

// Reads the raw bytes to bind/define for `value`: for a string-convertible
// leaf, its content (pointer + length); for anything else, its own address
// and size. Shared by the plain-field and the std::optional<value> cases.
template <typename ValueType>
std::pair<dvoid*, sb4> raw_bind_args(ValueType& value) {
    if constexpr (is_fixed_string_v<ValueType>) {
        // SQLT_CHR: explicit length, no null terminator, so the bound size is
        // the content length, not the buffer capacity. Checked before the
        // string_view branch below, which FixedString would otherwise match
        // (it converts to string_view) and be bound as SQLT_STR with a
        // trailing null it does not guarantee.
        return { reinterpret_cast<dvoid*>(value.data()), static_cast<sb4>(value.length()) };
    } else if constexpr (std::is_convertible_v<ValueType, std::string_view>) {
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
sword bind_named_container(OCIStmt* stmt, OciConnection& conn, Container& container,
                           std::string_view field_name, std::vector<sb2>& elem_indicators) {
    using ElemType = multi_bind_value_t<Container>;
    elem_indicators.assign(container.size(), OCI_IND_NOTNULL);
    sword status = OCI_SUCCESS;
    std::size_t idx = 0;
    for (auto& elem : container) {
        const std::string placeholder = ":" + std::string(field_name) + "_" + std::to_string(idx);
        OCIBind* bind_handle = nullptr;
        // set<T>'s elements are always const when iterated (mutating one
        // could violate the set's ordering invariant); const_cast matches
        // the same read-only-through-a-mutable-pointer pattern raw_bind_args
        // already uses for std::string_view content.
        auto [value_ptr, bind_size] = raw_bind_args(const_cast<ElemType&>(elem));
        record_status(status, OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    value_ptr, bind_size, oci_type_code_v<ElemType>,
                    &elem_indicators[idx], nullptr, nullptr, 0, nullptr, OCI_DEFAULT));
        if (status != OCI_SUCCESS) return status;
        ++idx;
    }
    return status;
}

// Binds every element of `values` as a positional IN parameter, starting
// at start_position -- the standalone-collection counterpart to
// bind_named_container above (which binds a struct field's own container
// by name instead of a bare id collection by position). `values` is a
// std::set for the same reason bind_named_container's caller-facing
// vector/valarray overloads dedupe into one first: a repeated value is
// never meaningful in an IN-list, and iterating a set gives a
// deterministic order, so the same logical id set always produces the
// same generated SQL text (and so hits the same cached cursor) regardless
// of what order the caller collected the values in.
template <typename ElemType>
sword bind_in_list(OCIStmt* stmt, OciConnection& conn, const std::set<ElemType>& values,
                    ub4 start_position, std::vector<sb2>& indicators) {
    sword status = OCI_SUCCESS;
    ub4 position = start_position;
    for (const ElemType& value : values) {
        const std::size_t idx = position - start_position;
        indicators[idx] = OCI_IND_NOTNULL;
        OCIBind* bind_handle = nullptr;
        // OCIBindByPos's valuep is non-const even for an IN-only bind (OCI
        // never writes through it here); const_cast matches the same
        // read-only-through-a-mutable-pointer pattern raw_bind_args
        // already uses for std::string_view content.
        auto [value_ptr, bind_size] = raw_bind_args(const_cast<ElemType&>(value));
        record_status(status, OCIBindByPos(stmt, &bind_handle, conn.err(), position,
                    value_ptr, bind_size, oci_type_code_v<ElemType>,
                    &indicators[idx], nullptr, nullptr, 0, nullptr, OCI_DEFAULT));
        if (status != OCI_SUCCESS) return status;
        ++position;
    }
    return status;
}

// Binds a FixedString<N> (or an optional's FixedString staging value): the
// buffer capacity as the bound size, with the value's own length_ref() as
// OCI's actual-length pointer. Capacity rather than content length so the
// same bind also works as an OUT parameter (RETURNING ... INTO), where OCI
// writes the returned value back through this pointer and treats the bound
// size as the buffer it may fill. That is exactly what a raw std::string
// cannot safely do, which is why an output bind needs this type.
template <typename FixedStringType>
sword bind_fixed_string(OCIStmt* stmt, OciConnection& conn, FixedStringType& value,
                         const std::string& placeholder, sb2* indicator) {
    OCIBind* bind_handle = nullptr;
    return OCIBindByName(stmt, &bind_handle, conn.err(),
                reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                reinterpret_cast<dvoid*>(value.data()), static_cast<sb4>(FixedStringType::capacity),
                oci_type_code_v<FixedStringType>, indicator,
                &value.length_ref(), nullptr, 0, nullptr, OCI_DEFAULT);
}

template <std::size_t I, bindable T>
void bind_one_field(sword& status, OCIStmt* stmt, OciConnection& conn, T& row,
                     std::vector<OCILobLocator**>& active_locators,
                     std::vector<OCIDateTime**>& active_datetime_locators,
                     std::vector<sb2>& indicators, std::vector<std::vector<sb2>>& container_indicators,
                     staging_tuple_t<T>& staging, std::string_view field_name) {
    if (status != OCI_SUCCESS) return; // an earlier field already failed
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(row);
    const std::string placeholder = ":" + std::string(field_name);
    OCIBind* bind_handle = nullptr;
    indicators[I] = OCI_IND_NOTNULL;

    if constexpr (is_multi_bind_container_v<FieldType>) {
        record_status(status, bind_named_container(stmt, conn, field, field_name, container_indicators[I]));
    } else if constexpr (is_oci_lob_v<FieldType>) {
        record_status(status, OCIDescriptorAlloc(conn.env(), reinterpret_cast<void**>(&field.locator),
                                                 OCI_DTYPE_LOB, 0, nullptr));
        if (status != OCI_SUCCESS) return;
        active_locators.push_back(&field.locator);

        // A descriptor straight out of OCIDescriptorAlloc is not yet a usable
        // LOB -- it has no underlying storage, and OCILobWrite2 against one
        // fails on a real database (ORA-22275, invalid LOB locator). Writing
        // an inbound LOB needs either a locator fetched from the row
        // (EMPTY_CLOB() + RETURNING ... INTO, or SELECT ... FOR UPDATE) or a
        // temporary LOB, which is what this creates and binds. Released in
        // free_locators / drain_lobs below.
        record_status(status, OCILobCreateTemporary(conn.svc(), conn.err(), field.locator,
                                                    OCI_DEFAULT, SQLCS_IMPLICIT, OCI_TEMP_CLOB,
                                                    0, OCI_DURATION_SESSION));
        if (status != OCI_SUCCESS) return;

        std::string& buffer = [&]() -> std::string& {
            if constexpr (std::is_same_v<FieldType, OciClob>) return field.text_data;
            else return field.xml_data;
        }();

        if (!buffer.empty()) {
            oraub8 bytes_written = 0;
            oraub8 chars_written = 0;
            record_status(status,
                OCILobWrite2(conn.svc(), conn.err(), field.locator, &bytes_written, &chars_written, 1,
                             reinterpret_cast<dvoid*>(buffer.data()), buffer.size(), OCI_ONE_PIECE,
                             nullptr, nullptr, 0, 0));
            if (status != OCI_SUCCESS) return;
        }

        record_status(status, OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    reinterpret_cast<dvoid*>(&field.locator), sizeof(OCILobLocator*),
                    oci_type_code_v<FieldType>, &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT));
    } else if constexpr (is_oci_datetime_v<FieldType>) {
        // OciTimestamp is bind-side only (like OciClob/OciXml) -- its
        // OCIDateTime* is an opaque descriptor allocated per value, not a
        // fixed-size inline value, so define_one_field_array rejects it
        // from select() output entirely (see that function's static_asserts).
        record_status(status, OCIDescriptorAlloc(conn.env(), reinterpret_cast<void**>(&field.locator),
                                                 OCI_DTYPE_TIMESTAMP, 0, nullptr));
        if (status != OCI_SUCCESS) return;
        active_datetime_locators.push_back(&field.locator);

        record_status(status, OCIDateTimeConstruct(conn.env(), conn.err(), field.locator,
                                                   field.year(), field.month(), field.day(),
                                                   field.hour(), field.minute(), field.second(), 0,
                                                   nullptr, 0));
        if (status != OCI_SUCCESS) return;

        record_status(status, OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    reinterpret_cast<dvoid*>(&field.locator), sizeof(OCIDateTime*),
                    oci_type_code_v<FieldType>, &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT));
    } else if constexpr (is_fixed_string_v<FieldType>) {
        record_status(status, bind_fixed_string(stmt, conn, field, placeholder, &indicators[I]));
    } else if constexpr (is_optional_v<FieldType>) {
        auto& stage = std::get<I>(staging);
        if (field.has_value()) {
            stage = *field;
            indicators[I] = OCI_IND_NOTNULL;
        } else {
            stage = {};
            indicators[I] = OCI_IND_NULL;
        }
        if constexpr (is_fixed_string_v<optional_value_t<FieldType>>) {
            record_status(status, bind_fixed_string(stmt, conn, stage, placeholder, &indicators[I]));
        } else {
            auto [value_ptr, bind_size] = raw_bind_args(stage);
            record_status(status, OCIBindByName(stmt, &bind_handle, conn.err(),
                        reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                        value_ptr, bind_size, oci_type_code_v<FieldType>,
                        &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT));
        }
    } else {
        auto [value_ptr, bind_size] = raw_bind_args(field);
        record_status(status, OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    value_ptr, bind_size, oci_type_code_v<FieldType>,
                    &indicators[I], nullptr, nullptr, 0, nullptr, OCI_DEFAULT));
    }
}

template <bindable T, std::size_t... Is>
sword bind_fields_impl(OCIStmt* stmt, OciConnection& conn, T& row,
                       std::vector<OCILobLocator**>& active_locators,
                       std::vector<OCIDateTime**>& active_datetime_locators,
                       std::vector<sb2>& indicators, std::vector<std::vector<sb2>>& container_indicators,
                       staging_tuple_t<T>& staging, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    sword status = OCI_SUCCESS;
    (bind_one_field<Is>(status, stmt, conn, row, active_locators, active_datetime_locators, indicators,
                        container_indicators, staging, names[Is]), ...);
    return status;
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
sword bind_fields(OCIStmt* stmt, OciConnection& conn, T& row,
                  std::vector<OCILobLocator**>& active_locators,
                  std::vector<OCIDateTime**>& active_datetime_locators,
                  std::vector<sb2>& indicators, std::vector<std::vector<sb2>>& container_indicators,
                  staging_tuple_t<T>& staging) {
    return bind_fields_impl(stmt, conn, row, active_locators, active_datetime_locators, indicators,
                      container_indicators, staging,
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
template <std::size_t I, bindable T, typename Alloc>
void bind_one_field_array(sword& status, OCIStmt* stmt, OciConnection& conn, std::vector<T, Alloc>& rows,
                           std::vector<std::vector<sb2>>& field_indicators, std::string_view field_name) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    static_assert(!is_oci_lob_v<FieldType>,
                  "LOB fields are not supported in bulk insert(vector<T>&) yet -- array-binding "
                  "a LOB column needs a per-row array of LOB locators, not implemented here; "
                  "insert(conn, query_text, T&) in a loop for a LOB-bearing row type instead.");
    static_assert(!is_oci_datetime_v<FieldType>,
                  "OciTimestamp fields are not supported in bulk insert(vector<T>&) -- like a LOB, "
                  "its OCIDateTime* is a per-value descriptor, not a fixed-size inline value; use "
                  "OciDate for a COB/business date (that one IS bulk-bindable), or "
                  "insert(conn, query_text, T&) in a loop for an OciTimestamp-bearing row type.");
    // FixedString<N> is exempt from the string restriction below: unlike
    // std::string, its characters live inline in T at a fixed offset, which
    // is exactly what a fixed-stride array bind needs. Its per-row length
    // travels through OCIBindArrayOfStruct's alskip the same way.
    static_assert(is_fixed_string_v<FieldType> ||
                  (!std::is_convertible_v<FieldType, std::string_view> &&
                   !(is_optional_v<FieldType> && std::is_convertible_v<optional_value_t<FieldType>, std::string_view>)),
                  "std::string fields are not supported in bulk insert(vector<T>&) -- a "
                  "string's characters live in its own heap/SSO storage, not inline in T at a "
                  "fixed stride, which a real Oracle array bind needs. Declare the field as "
                  "FixedString<N> (binding/oci_fixed_string.h) to bulk-insert a CHAR/VARCHAR2 "
                  "column, or insert(conn, query_text, T&) in a loop.");
    static_assert(!is_multi_bind_container_v<FieldType>,
                  "a vector/set/valarray field has no per-row bulk-insert meaning -- it's a "
                  "single query's dynamic IN-list, not a per-row column value.");
    static_assert(!is_optional_v<FieldType>,
                  "std::optional fields are not supported in bulk insert(vector<T>&) yet -- "
                  "every row's optional would need to be engaged (an empty one has no address "
                  "to bind through), relying on an implementation-defined payload offset being "
                  "consistent across rows; use insert(conn, query_text, T&) in a loop for a "
                  "row type with a nullable field instead.");

    if (status != OCI_SUCCESS) return; // an earlier field already failed

    auto& indicators = field_indicators[I];
    indicators.assign(rows.size(), OCI_IND_NOTNULL);

    const std::string placeholder = ":" + std::string(field_name);
    OCIBind* bind_handle = nullptr;

    if constexpr (is_fixed_string_v<FieldType>) {
        auto& first = boost::pfr::get<I>(rows[0]);
        record_status(status, OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    reinterpret_cast<dvoid*>(first.data()), static_cast<sb4>(FieldType::capacity),
                    oci_type_code_v<FieldType>, indicators.data(),
                    &first.length_ref(), nullptr, 0, nullptr, OCI_DEFAULT));
        // alskip = sizeof(T): each row's own length_ field sits inside its own
        // FixedString, one whole row apart from the previous row's.
        record_status(status, OCIBindArrayOfStruct(bind_handle, conn.err(),
                              static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)),
                              static_cast<ub4>(sizeof(T)), 0));
    } else {
        record_status(status, OCIBindByName(stmt, &bind_handle, conn.err(),
                    reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                    reinterpret_cast<dvoid*>(&boost::pfr::get<I>(rows[0])), static_cast<sb4>(sizeof(FieldType)),
                    oci_type_code_v<FieldType>, indicators.data(), nullptr, nullptr, 0, nullptr, OCI_DEFAULT));
        record_status(status, OCIBindArrayOfStruct(bind_handle, conn.err(),
                              static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)), 0, 0));
    }
}

template <bindable T, typename Alloc, std::size_t... Is>
sword bind_fields_array_impl(OCIStmt* stmt, OciConnection& conn, std::vector<T, Alloc>& rows,
                             std::vector<std::vector<sb2>>& field_indicators, std::index_sequence<Is...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    sword status = OCI_SUCCESS;
    (bind_one_field_array<Is>(status, stmt, conn, rows, field_indicators, names[Is]), ...);
    return status;
}

template <bindable T, typename Alloc>
sword bind_fields_array(OCIStmt* stmt, OciConnection& conn, std::vector<T, Alloc>& rows,
                        std::vector<std::vector<sb2>>& field_indicators) {
    return bind_fields_array_impl(stmt, conn, rows, field_indicators,
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
                // Bound LOBs are temporary LOBs (see bind_one_field), so the
                // server-side temporary has to be released before the
                // descriptor itself is freed -- otherwise it lives until the
                // session ends, which for a long batch means the temporary
                // LOB tablespace grows for the whole run.
                OCILobFreeTemporary(conn.svc(), conn.err(), field.locator);
                OCIDescriptorFree(reinterpret_cast<void*>(field.locator), OCI_DTYPE_LOB);
                field.locator = nullptr;
            }
        }
    });
}

inline void free_locators(OciConnection& conn, std::vector<OCILobLocator**>& active_locators) {
    for (auto* loc : active_locators) {
        if (*loc) {
            OCILobFreeTemporary(conn.svc(), conn.err(), *loc); // see drain_lobs
            OCIDescriptorFree(reinterpret_cast<void*>(*loc), OCI_DTYPE_LOB);
            *loc = nullptr;
        }
    }
    active_locators.clear();
}

// Releases whatever an attempt allocated for a bind struct's LOB fields on
// any exit path, including a throw out of the middle of a bind or fetch.
class LocatorGuard {
public:
    LocatorGuard(OciConnection& conn, std::vector<OCILobLocator**>& locators)
        : conn_(conn), locators_(locators) {}
    ~LocatorGuard() { free_locators(conn_, locators_); }

    LocatorGuard(const LocatorGuard&) = delete;
    LocatorGuard& operator=(const LocatorGuard&) = delete;

    // Hands ownership back when the caller has already drained the LOBs
    // itself (drain_lobs frees each descriptor as it reads it back).
    void release() { locators_.clear(); }

private:
    OciConnection& conn_;
    std::vector<OCILobLocator**>& locators_;
};

// OciTimestamp's counterpart to free_locators -- a plain OCIDateTime
// descriptor, not a temporary LOB, so this skips OCILobFreeTemporary
// entirely (calling it on a non-LOB descriptor would be invalid) and just
// frees the descriptor itself.
inline void free_datetime_locators(std::vector<OCIDateTime**>& active_locators) {
    for (auto* loc : active_locators) {
        if (*loc) {
            OCIDescriptorFree(reinterpret_cast<void*>(*loc), OCI_DTYPE_TIMESTAMP);
            *loc = nullptr;
        }
    }
    active_locators.clear();
}

// OciTimestamp's counterpart to LocatorGuard -- see free_datetime_locators.
class DateTimeLocatorGuard {
public:
    explicit DateTimeLocatorGuard(std::vector<OCIDateTime**>& locators) : locators_(locators) {}
    ~DateTimeLocatorGuard() { free_datetime_locators(locators_); }

    DateTimeLocatorGuard(const DateTimeLocatorGuard&) = delete;
    DateTimeLocatorGuard& operator=(const DateTimeLocatorGuard&) = delete;

private:
    std::vector<OCIDateTime**>& locators_;
};

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
void define_one_field_array(sword& status, OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                             std::vector<std::vector<sb2>>& field_indicators,
                             batch_staging_tuple_t<T>& batch_staging) {
    using FieldType = boost::pfr::tuple_element_t<I, T>;
    // A FixedString<N> column (optionally wrapped in std::optional) is the
    // supported way to fetch a CHAR/VARCHAR2 -- N is the fixed maximum output
    // buffer size OCI needs up front, which is precisely what std::string
    // cannot supply.
    static constexpr bool fetches_as_fixed_string =
        is_fixed_string_v<FieldType> ||
        (is_optional_v<FieldType> && is_fixed_string_v<optional_value_t<FieldType>>);

    static_assert(!is_oci_lob_v<FieldType>,
                  "LOB columns are not supported in select() result rows yet");
    static_assert(!is_oci_datetime_v<FieldType> &&
                  !(is_optional_v<FieldType> && is_oci_datetime_v<optional_value_t<FieldType>>),
                  "OciTimestamp columns are not supported in select() result rows -- its "
                  "OCIDateTime* is a per-value descriptor, not a fixed-size inline value the "
                  "array fetch can define directly into. Declare a DATE/COB-date-shaped column "
                  "as OciDate instead (that one IS select()-able); OciTimestamp is bind-side only "
                  "for now (see execute()/insert()).");
    static_assert(fetches_as_fixed_string ||
                  (!std::is_convertible_v<FieldType, std::string_view> &&
                   !(is_optional_v<FieldType> && std::is_convertible_v<optional_value_t<FieldType>, std::string_view>)),
                  "std::string columns are not supported in select() result rows -- OCI needs a "
                  "fixed max output buffer size to define into before it knows how long the "
                  "value is. Declare the field as FixedString<N> (binding/oci_fixed_string.h), "
                  "or std::optional<FixedString<N>> for a nullable column. The bind side "
                  "(execute()/insert()) still takes std::string fine.");
    static_assert(!is_multi_bind_container_v<FieldType>,
                  "vector/set/valarray fields are an execute()/insert() input-side concept only "
                  "(a dynamic multi-value IN-list parameter) -- a single result column can't "
                  "fetch into a variable-length container; that's multiple rows, not one field.");

    if (status != OCI_SUCCESS) return; // an earlier column already failed

    constexpr ub4 position = I + 1;
    auto& indicators = field_indicators[I];
    indicators.assign(batch.size(), OCI_IND_NOTNULL);
    OCIDefine* define_handle = nullptr;

    if constexpr (is_optional_v<FieldType>) {
        using ElemType = optional_value_t<FieldType>;
        auto& stage = std::get<I>(batch_staging);
        stage.assign(batch.size(), ElemType{});
        if constexpr (is_fixed_string_v<ElemType>) {
            record_status(status, OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                          reinterpret_cast<dvoid*>(stage[0].data()), static_cast<sb4>(ElemType::capacity),
                          oci_type_code_v<FieldType>, indicators.data(),
                          &stage[0].length_ref(), nullptr, OCI_DEFAULT));
            record_status(status, OCIDefineArrayOfStruct(define_handle, conn.err(),
                                    static_cast<ub4>(sizeof(ElemType)), static_cast<ub4>(sizeof(sb2)),
                                    static_cast<ub4>(sizeof(ElemType)), 0));
        } else {
            record_status(status, OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                          reinterpret_cast<dvoid*>(stage.data()), static_cast<sb4>(sizeof(ElemType)),
                          oci_type_code_v<FieldType>, indicators.data(), nullptr, nullptr, OCI_DEFAULT));
            record_status(status, OCIDefineArrayOfStruct(define_handle, conn.err(),
                                    static_cast<ub4>(sizeof(ElemType)), static_cast<ub4>(sizeof(sb2)), 0, 0));
        }
    } else if constexpr (is_fixed_string_v<FieldType>) {
        auto& first = boost::pfr::get<I>(batch[0]);
        // rlskip = sizeof(T): OCI reports each row's fetched length into that
        // row's own FixedString::length_, one whole row apart.
        record_status(status, OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                      reinterpret_cast<dvoid*>(first.data()), static_cast<sb4>(FieldType::capacity),
                      oci_type_code_v<FieldType>, indicators.data(),
                      &first.length_ref(), nullptr, OCI_DEFAULT));
        record_status(status, OCIDefineArrayOfStruct(define_handle, conn.err(),
                                static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)),
                                static_cast<ub4>(sizeof(T)), 0));
    } else {
        record_status(status, OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                      reinterpret_cast<dvoid*>(&boost::pfr::get<I>(batch[0])), static_cast<sb4>(sizeof(FieldType)),
                      oci_type_code_v<FieldType>, indicators.data(), nullptr, nullptr, OCI_DEFAULT));
        record_status(status, OCIDefineArrayOfStruct(define_handle, conn.err(),
                                static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)), 0, 0));
    }
}

template <bindable T, std::size_t... Is>
sword define_fields_array_impl(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                               std::vector<std::vector<sb2>>& field_indicators,
                               batch_staging_tuple_t<T>& batch_staging, std::index_sequence<Is...>) {
    sword status = OCI_SUCCESS;
    (define_one_field_array<Is>(status, stmt, conn, batch, field_indicators, batch_staging), ...);
    return status;
}

template <bindable T>
sword define_fields_array(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                          std::vector<std::vector<sb2>>& field_indicators,
                          batch_staging_tuple_t<T>& batch_staging) {
    return define_fields_array_impl(stmt, conn, batch, field_indicators, batch_staging,
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
// `results` may have any allocator (see OciClient::select()'s doc comment);
// `batch` below is this function's own internal scratch buffer, never
// exposed to the caller, so it deliberately stays plain std::vector<OutputT>
// regardless of what allocator `results` uses.
template <bindable OutputT, typename Alloc>
OciOutcome run_select_fetch_loop(OciConnection& conn, OCIStmt* stmt, std::vector<OutputT, Alloc>& results) {
    ub4 prefetch_rows = static_cast<ub4>(kSelectBatchRows);
    sword status = OCIAttrSet(stmt, OCI_HTYPE_STMT, &prefetch_rows, 0, OCI_ATTR_PREFETCH_ROWS, conn.err());
    if (status != OCI_SUCCESS) {
        return {false, status};
    }

    std::vector<OutputT> batch(kSelectBatchRows);
    std::vector<std::vector<sb2>> field_indicators(boost::pfr::tuple_size_v<OutputT>);
    batch_staging_tuple_t<OutputT> batch_staging{};
    // A failed define leaves a column pointing nowhere; fetching into it
    // afterwards is what turns a recoverable error into a corrupt row.
    status = define_fields_array(stmt, conn, batch, field_indicators, batch_staging);
    if (status != OCI_SUCCESS) {
        return {false, status};
    }

    status = OCIStmtExecute(conn.svc(), stmt, conn.err(), 0, 0, nullptr, nullptr, OCI_DEFAULT);
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
        const sword attr_status =
            OCIAttrGet(stmt, OCI_HTYPE_STMT, &rows_fetched, &attr_size, OCI_ATTR_ROWS_FETCHED, conn.err());
        if (attr_status != OCI_SUCCESS) {
            // Without a trustworthy count there is no way to know how much of
            // `batch` OCI actually wrote, so appending any of it would append
            // stale rows from the previous batch.
            return {false, attr_status};
        }

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

template <bindable T, typename Alloc>
bool OciClient::insert(OciConnection& conn, const std::string& query_text, std::vector<T, Alloc>& rows) {
    if (rows.empty()) return true; // nothing to bind rows[0]'s address through
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_insert_array_once(conn, query_text, rows);
    });
}

template <bindable OutputT, typename Alloc>
bool OciClient::select(OciConnection& conn, const std::string& query_text, std::vector<OutputT, Alloc>& results) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_select_once(conn, query_text, results);
    });
}

template <bindable InputT, bindable OutputT, typename Alloc>
bool OciClient::select(OciConnection& conn, const std::string& query_text,
                        InputT& input, std::vector<OutputT, Alloc>& results) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        return run_select_once(conn, query_text, input, results);
    });
}

inline OciOutcome OciClient::run_execute_once(OciConnection& conn, const std::string& query_text) {
    detail::StmtHandle stmt;
    const sword prepare_status = detail::prepare_statement(conn, stmt, query_text);
    if (prepare_status != OCI_SUCCESS) {
        return {false, prepare_status};
    }

    const sword status = OCIStmtExecute(conn.svc(), stmt.get(), conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);
    return {status == OCI_SUCCESS, status};
}

template <bindable T>
OciOutcome OciClient::run_execute_once(OciConnection& conn, const std::string& query_text, T& bind_struct) {
    // A vector/set/valarray field's own "{field_name}" marker gets
    // replaced with a placeholder list sized to that field's current
    // element count -- a no-op if T has no such field, so this is safe
    // (and cheap) to call unconditionally.
    const std::string sql = detail::substitute_container_markers(query_text, bind_struct);

    detail::StmtHandle stmt;
    const sword prepare_status = detail::prepare_statement(conn, stmt, sql);
    if (prepare_status != OCI_SUCCESS) {
        return {false, prepare_status};
    }

    std::vector<OCILobLocator**> active_locators;
    detail::LocatorGuard locator_guard(conn, active_locators);
    std::vector<OCIDateTime**> active_datetime_locators;
    detail::DateTimeLocatorGuard datetime_guard(active_datetime_locators);
    std::vector<sb2> indicators(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
    std::vector<std::vector<sb2>> container_indicators(boost::pfr::tuple_size_v<T>);
    detail::staging_tuple_t<T> staging{};
    const sword bind_status = detail::bind_fields(stmt.get(), conn, bind_struct, active_locators,
                                                  active_datetime_locators, indicators, container_indicators, staging);
    if (bind_status != OCI_SUCCESS) {
        return {false, bind_status};
    }

    const sword status = OCIStmtExecute(conn.svc(), stmt.get(), conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);

    if (status == OCI_SUCCESS) {
        detail::drain_lobs(conn, bind_struct); // frees each locator as it reads it back
        locator_guard.release();
        return {true, status};
    }

    return {false, status};
}

template <bindable T, typename Alloc>
OciOutcome OciClient::run_insert_array_once(OciConnection& conn, const std::string& query_text,
                                             std::vector<T, Alloc>& rows) {
    detail::StmtHandle stmt;
    const sword prepare_status = detail::prepare_statement(conn, stmt, query_text);
    if (prepare_status != OCI_SUCCESS) {
        return {false, prepare_status};
    }

    std::vector<std::vector<sb2>> field_indicators(boost::pfr::tuple_size_v<T>);
    const sword bind_status = detail::bind_fields_array(stmt.get(), conn, rows, field_indicators);
    if (bind_status != OCI_SUCCESS) {
        return {false, bind_status};
    }

    const sword status = OCIStmtExecute(conn.svc(), stmt.get(), conn.err(),
                                         static_cast<ub4>(rows.size()), 0, nullptr, nullptr, OCI_DEFAULT);
    return {status == OCI_SUCCESS, status};
}

template <bindable OutputT, typename Alloc>
OciOutcome OciClient::run_select_once(OciConnection& conn, const std::string& query_text,
                                       std::vector<OutputT, Alloc>& results) {
    results.clear();

    detail::StmtHandle stmt;
    const sword prepare_status = detail::prepare_statement(conn, stmt, query_text);
    if (prepare_status != OCI_SUCCESS) {
        return {false, prepare_status};
    }

    return detail::run_select_fetch_loop(conn, stmt.get(), results);
}

// Same as the no-input overload above, but also binds `input`'s fields as
// named parameters (see detail::bind_fields) before defining the output
// columns and executing -- the read-side counterpart to
// run_execute_once(conn, query_text, bind_struct). `input`'s own
// {field_name} container markers (a vector/set/valarray field) are
// substituted exactly as on the execute() side.
template <bindable InputT, bindable OutputT, typename Alloc>
OciOutcome OciClient::run_select_once(OciConnection& conn, const std::string& query_text,
                                       InputT& input, std::vector<OutputT, Alloc>& results) {
    results.clear();

    const std::string sql = detail::substitute_container_markers(query_text, input);

    detail::StmtHandle stmt;
    const sword prepare_status = detail::prepare_statement(conn, stmt, sql);
    if (prepare_status != OCI_SUCCESS) {
        return {false, prepare_status};
    }

    std::vector<OCILobLocator**> input_locators;
    detail::LocatorGuard locator_guard(conn, input_locators);
    std::vector<OCIDateTime**> input_datetime_locators;
    detail::DateTimeLocatorGuard datetime_guard(input_datetime_locators);
    std::vector<sb2> input_indicators(boost::pfr::tuple_size_v<InputT>, OCI_IND_NOTNULL);
    std::vector<std::vector<sb2>> input_container_indicators(boost::pfr::tuple_size_v<InputT>);
    detail::staging_tuple_t<InputT> input_staging{};
    const sword bind_status = detail::bind_fields(stmt.get(), conn, input, input_locators, input_datetime_locators,
                                                  input_indicators, input_container_indicators, input_staging);
    if (bind_status != OCI_SUCCESS) {
        return {false, bind_status};
    }

    return detail::run_select_fetch_loop(conn, stmt.get(), results);
}

template <typename ElemType, bindable RowT, typename Alloc>
bool select_with_in_list(OciConnection& conn, const std::string& query_template,
                          const std::set<ElemType>& ids, std::vector<RowT, Alloc>& results) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        results.clear();
        const std::string sql = substitute_in_marker(query_template, make_in_placeholders(ids.size(), 1));

        detail::StmtHandle stmt;
        const sword prepare_status = detail::prepare_statement(conn, stmt, sql);
        if (prepare_status != OCI_SUCCESS) {
            return {false, prepare_status};
        }

        std::vector<sb2> in_indicators(ids.size(), OCI_IND_NOTNULL);
        const sword bind_status = detail::bind_in_list(stmt.get(), conn, ids, 1, in_indicators);
        if (bind_status != OCI_SUCCESS) {
            return {false, bind_status};
        }

        return detail::run_select_fetch_loop(conn, stmt.get(), results);
    });
}

template <typename ElemType, bindable RowT, typename Alloc>
bool select_with_in_list(OciConnection& conn, const std::string& query_template,
                          const std::vector<ElemType>& ids, std::vector<RowT, Alloc>& results) {
    return select_with_in_list(conn, query_template, std::set<ElemType>(ids.begin(), ids.end()), results);
}

template <typename ElemType, bindable RowT, typename Alloc>
bool select_with_in_list(OciConnection& conn, const std::string& query_template,
                          const std::valarray<ElemType>& ids, std::vector<RowT, Alloc>& results) {
    return select_with_in_list(conn, query_template, std::set<ElemType>(std::begin(ids), std::end(ids)), results);
}

template <typename ElemType>
bool execute_with_in_list(OciConnection& conn, const std::string& query_template,
                           const std::set<ElemType>& ids) {
    return conn.run_with_reconnect([&]() -> OciOutcome {
        const std::string sql = substitute_in_marker(query_template, make_in_placeholders(ids.size(), 1));

        detail::StmtHandle stmt;
        const sword prepare_status = detail::prepare_statement(conn, stmt, sql);
        if (prepare_status != OCI_SUCCESS) {
            return {false, prepare_status};
        }

        std::vector<sb2> indicators(ids.size(), OCI_IND_NOTNULL);
        const sword bind_status = detail::bind_in_list(stmt.get(), conn, ids, 1, indicators);
        if (bind_status != OCI_SUCCESS) {
            return {false, bind_status};
        }

        const sword status = OCIStmtExecute(conn.svc(), stmt.get(), conn.err(), 1, 0, nullptr, nullptr, OCI_DEFAULT);
        return {status == OCI_SUCCESS, status};
    });
}

template <typename ElemType>
bool execute_with_in_list(OciConnection& conn, const std::string& query_template,
                           const std::vector<ElemType>& ids) {
    return execute_with_in_list(conn, query_template, std::set<ElemType>(ids.begin(), ids.end()));
}

template <typename ElemType>
bool execute_with_in_list(OciConnection& conn, const std::string& query_template,
                           const std::valarray<ElemType>& ids) {
    return execute_with_in_list(conn, query_template, std::set<ElemType>(std::begin(ids), std::end(ids)));
}

} // namespace binding
