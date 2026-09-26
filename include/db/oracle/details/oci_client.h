#pragma once
// Implementation of oci_client.h's free functions -- see that file for the
// interface and behavioral contract, and its own file comment for how this
// differs from ideas/binding's version of the same file (drives
// OciStatement instead of raw OCI calls; LOB staging is OCILob objects,
// not raw OCILobLocator* plus hand-rolled alloc/free). Not meant to be
// included directly.
#include <db/oracle/oci_client.h>

#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

namespace marketlib::db::oracle {
namespace detail {

// ---- IN: bind a struct's fields by name ------------------------------------
//
// A plain field binds straight through its own address -- `params` is the
// caller's and has to live until execute() runs anyway. An optional<T>
// field can't: an empty optional has no address to bind through, so it
// needs real storage that outlives this whole function -- `staging`,
// owned by the caller (see bind_params below), is that storage. A LOB
// field's storage is an OCILob itself: constructing it, calling
// create_temporary()/write() on it, and letting it free its own locator
// when `staging` goes out of scope replaces ideas/binding's separate
// make_temp_lob/free_temp_lob free functions and its own
// std::vector<OCILobLocator*> entirely.
template <typename T>
using bind_slot_t = std::conditional_t<is_oci_lob_v<T>, std::optional<OCILob>,
                        std::conditional_t<is_optional_v<T>, optional_value_t<T>, std::monostate>>;

template <typename T, std::size_t... I>
auto bind_tuple(std::index_sequence<I...>)
    -> std::tuple<bind_slot_t<boost::pfr::tuple_element_t<I, T>>...>;
template <typename T>
using bind_t = decltype(bind_tuple<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

template <std::size_t I, typename T>
void bind_one_param(OciStatement& stmt, OciConnection& conn, T& params, std::string_view name,
                     std::vector<sb2>& indicators, bind_t<T>& staging) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(params);

    if constexpr (is_oci_lob_v<FieldT>) {
        // Never optional here -- see oci_client.h's is_scalar_bindable_field_v
        // comment for why std::optional<OciClob/OciBlob> doesn't satisfy
        // scalar_bindable yet.
        auto& slot = std::get<I>(staging);
        slot.emplace(conn);
        if constexpr (is_oci_clob_v<FieldT>) {
            slot->create_temporary(OCI_TEMP_CLOB);
            slot->write(field.text_data.data(), field.text_data.size());
        } else {
            slot->create_temporary(OCI_TEMP_BLOB);
            slot->write(field.binary_data.data(), field.binary_data.size());
        }
        stmt.bindName(std::string(name), oci_type_code_v<FieldT>, slot->locator_address(),
                      static_cast<sb4>(sizeof(OCILobLocator*)));
    } else if constexpr (is_optional_v<FieldT>) {
        // Never a FixedString<N> element here -- is_scalar_bindable_field_v
        // (oci_client.h) excludes std::optional<FixedString<N>> entirely,
        // so ElemT is always arithmetic or OciDate, both plain
        // fixed-size values with no length/indicator distinction of
        // their own to worry about.
        auto& stage = std::get<I>(staging);
        if (field) { stage = *field; indicators[I] = OCI_IND_NOTNULL; }
        else       { stage = {};     indicators[I] = OCI_IND_NULL; }
        stmt.bindName(std::string(name), oci_type_code_v<FieldT>, &stage, sizeof(stage), &indicators[I]);
    } else if constexpr (is_fixed_string_v<FieldT>) {
        // Oracle itself can't store an empty VARCHAR2/CHAR value --
        // inserting '' always lands as NULL, regardless of what the
        // client bound. Binding a zero-length FixedString<N> as an
        // explicit NULL makes that visible in the type instead of
        // relying on the engine's own implicit conversion: a plain,
        // non-optional FixedString<N> already means "may be empty" to
        // a caller, so there's no separate "may be absent" state left
        // for std::optional<FixedString<N>> to add.
        indicators[I] = (field.length() == 0) ? OCI_IND_NULL : OCI_IND_NOTNULL;
        stmt.bindName(std::string(name), oci_type_code_v<FieldT>, field.data(),
                      static_cast<sb4>(field.length()), &indicators[I]);
    } else {
        indicators[I] = OCI_IND_NOTNULL;
        stmt.bindName(std::string(name), oci_type_code_v<FieldT>, &field, sizeof(field));
    }
}

template <typename T, std::size_t... I>
void bind_params_impl(OciStatement& stmt, OciConnection& conn, T& params, std::vector<sb2>& indicators,
                       bind_t<T>& staging, std::index_sequence<I...>) {
    constexpr auto names = field_names_of<T>();
    (bind_one_param<I>(stmt, conn, params, names[I], indicators, staging), ...);
}

// indicators/staging are owned by the caller (see execute() below), not
// hidden as locals here, because they must stay alive from this call
// through OciStatement::execute() -- the same lifetime rule the
// field-level comment above explains, just at the whole-struct level.
// staging's OCILob entries free their own locators when it's destroyed;
// there is no separate free step the caller has to remember to run,
// unlike ideas/binding's lob_locators vector.
template <typename T>
void bind_params(OciStatement& stmt, OciConnection& conn, T& params, std::vector<sb2>& indicators, bind_t<T>& staging) {
    indicators.assign(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
    bind_params_impl(stmt, conn, params, indicators, staging,
                      std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

// ---- Column-name resolution: which OCI column position does field I of
// OutT actually correspond to? Delegates the per-name lookup to
// OciStatement::describeColumnPosition() (oci_statement.h), which already
// does the OCIParamGet/OCIAttrGet(OCI_ATTR_NAME) walk; this only adds the
// "backend can't describe at all" fallback (position = I + 1) that
// describeColumnPosition() itself deliberately leaves to its caller (see
// its own comment) -- true of the mock, never true of a real Oracle
// result set with at least one column.
template <typename T>
std::vector<ub4> resolve_column_positions(OciStatement& stmt, OciConnection& conn) {
    ub4 column_count = 0;
    ub4 attr_size = sizeof(column_count);
    OCIAttrGet(stmt.handle(), OCI_HTYPE_STMT, &column_count, &attr_size, OCI_ATTR_PARAM_COUNT, conn.err());

    constexpr std::size_t field_count = boost::pfr::tuple_size_v<T>;
    std::vector<ub4> positions(field_count);

    if (column_count == 0) {
        for (std::size_t i = 0; i < field_count; ++i) positions[i] = static_cast<ub4>(i + 1);
        return positions;
    }

    constexpr auto field_names = field_names_of<T>();
    for (std::size_t i = 0; i < field_count; ++i) {
        positions[i] = stmt.describeColumnPosition(std::string(field_names[i]));
    }
    return positions;
}

// ---- OUT: define a batch of result rows ------------------------------------
//
// One OciStatement::bindOutput() per column, with elemSize telling it the
// row-to-row stride for an array-of-struct batch fetch. A plain column
// defines straight into batch[0]'s own address; an optional<T> column
// needs its own batch-sized staging vector (same reason as the bind
// side) plus a batch-sized indicator array. A LOB column's staging is a
// std::vector<OCILob>, one per row: OCILob's own locator_address() is
// the per-row define target, and its own layout (not a raw
// OCILobLocator*) is what elemSize strides over -- sizeof(OCILob), not
// sizeof(OCILobLocator*), since every element of a std::vector<OCILob>
// is the same size and lays its own locator_ member out at the same
// relative offset.
template <typename T>
using define_slot_t = std::conditional_t<is_oci_lob_v<T>, std::vector<OCILob>,
                        std::conditional_t<is_optional_v<T>, std::vector<optional_value_t<T>>, std::monostate>>;

template <typename T, std::size_t... I>
auto define_tuple(std::index_sequence<I...>)
    -> std::tuple<define_slot_t<boost::pfr::tuple_element_t<I, T>>...>;
template <typename T>
using define_t = decltype(define_tuple<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

template <std::size_t I, typename T>
void define_one_column(OciStatement& stmt, OciConnection& conn, std::vector<T>& batch,
                        std::vector<std::vector<sb2>>& indicators, define_t<T>& staging, ub4 position) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;

    if constexpr (is_optional_v<FieldT>) {
        // Never a FixedString<N> element here -- same exclusion as
        // bind_one_param's matching comment above.
        using ElemT = optional_value_t<FieldT>;
        auto& ind = indicators[I];
        ind.assign(batch.size(), OCI_IND_NOTNULL);
        auto& stage = std::get<I>(staging);
        stage.assign(batch.size(), ElemT{});
        stmt.bindOutput(position, oci_type_code_v<FieldT>, stage.data(), sizeof(ElemT),
                        nullptr, ind.data(), static_cast<sb4>(sizeof(ElemT)));
    } else if constexpr (is_oci_lob_v<FieldT>) {
        auto& ind = indicators[I];
        ind.assign(batch.size(), OCI_IND_NOTNULL);
        auto& locators = std::get<I>(staging);
        locators.clear();
        locators.reserve(batch.size());
        for (std::size_t i = 0; i < batch.size(); ++i) locators.emplace_back(conn);
        stmt.bindOutput(position, oci_type_code_v<FieldT>, locators[0].locator_address(),
                        static_cast<sb4>(sizeof(OCILobLocator*)), nullptr, ind.data(),
                        static_cast<sb4>(sizeof(OCILob)));
    } else if constexpr (is_fixed_string_v<FieldT>) {
        // A real indicator, not nullptr, even though this field isn't
        // std::optional<FixedString<N>> -- Oracle can't store an empty
        // VARCHAR2/CHAR value (it's always NULL on the way back too), so
        // a plain FixedString<N> column can genuinely come back NULL with
        // no optional<> wrapper involved at all. Without a real
        // indicator here, a NULL row would silently keep whatever the
        // batch buffer already held from a previous row -- see
        // apply_one_column's matching branch below, which is what
        // actually clears it back to empty.
        auto& ind = indicators[I];
        ind.assign(batch.size(), OCI_IND_NOTNULL);
        auto& first = boost::pfr::get<I>(batch[0]);
        stmt.bindOutput(position, oci_type_code_v<FieldT>, first.data(),
                        static_cast<sb4>(FieldT::capacity), &first.length_ref(), ind.data(),
                        static_cast<sb4>(sizeof(T)));
    } else {
        stmt.bindOutput(position, oci_type_code_v<FieldT>, &boost::pfr::get<I>(batch[0]),
                        sizeof(FieldT), nullptr, nullptr, static_cast<sb4>(sizeof(T)));
    }
}

template <typename T, std::size_t... I>
void define_columns(OciStatement& stmt, OciConnection& conn, std::vector<T>& batch,
                     std::vector<std::vector<sb2>>& indicators, define_t<T>& staging,
                     const std::vector<ub4>& positions, std::index_sequence<I...>) {
    (define_one_column<I>(stmt, conn, batch, indicators, staging, positions[I]), ...);
}

// After a fetch, copies each optional/LOB field's per-row staging value
// (or nullopt/default, per its indicator) into batch[row]. A no-op for
// every other field -- its value already landed directly in batch[row]
// via the plain bindOutput() path above.
template <std::size_t I, typename T>
void apply_one_column(std::vector<T>& batch, std::size_t row,
                       const std::vector<std::vector<sb2>>& indicators, const define_t<T>& staging) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    if constexpr (is_optional_v<FieldT>) {
        auto& field = boost::pfr::get<I>(batch[row]);
        field = (indicators[I][row] == OCI_IND_NULL)
                    ? std::nullopt
                    : std::make_optional(std::get<I>(staging)[row]);
    } else if constexpr (is_oci_lob_v<FieldT>) {
        auto& field = boost::pfr::get<I>(batch[row]);
        if (indicators[I][row] == OCI_IND_NULL) {
            field = FieldT{};
        } else {
            const std::string bytes = std::get<I>(staging)[row].read(is_oci_clob_v<FieldT>);
            if constexpr (is_oci_clob_v<FieldT>) field.text_data = bytes;
            else field.binary_data.assign(bytes.begin(), bytes.end());
        }
    } else if constexpr (is_fixed_string_v<FieldT>) {
        // A plain (non-optional) FixedString<N> field never has a
        // staging slot -- its value already landed directly in
        // batch[row] via bindOutput(), same as before. What's new is
        // clearing it back to empty on NULL: without this, a NULL row
        // would keep whatever a previous row (or the buffer's initial
        // zero-fill) left in the same reused batch slot. Explicit
        // clear(), not relying on Oracle to have zeroed length_ref()
        // itself on a NULL fetch.
        if (indicators[I][row] == OCI_IND_NULL) {
            boost::pfr::get<I>(batch[row]).clear();
        }
    }
}

// Skips entirely -- not just a no-op per field, the whole index_sequence
// fold -- when T has no field apply_one_column<I> would actually do
// anything for: a plain-scalar row type (the common case: no optional,
// no LOB, no FixedString<N> field at all) never needs a post-fetch copy,
// since every field already landed directly in batch[row] via
// bindOutput(). Checked once at compile time per field type rather than
// inferred from define_t<T>'s own staging shape -- a plain FixedString<N>
// field needs apply_one_column's NULL-clearing branch above but has no
// staging slot of its own (its define_slot_t is std::monostate, same as
// a plain scalar), so "does define_t<T> have a real slot for this field"
// stopped being the same question as "does this field need apply()."
template <typename FieldT>
inline constexpr bool field_needs_apply_v = is_optional_v<FieldT> || is_oci_lob_v<FieldT> || is_fixed_string_v<FieldT>;

template <typename T, std::size_t... I>
constexpr bool needs_apply_loop_impl(std::index_sequence<I...>) {
    return (field_needs_apply_v<boost::pfr::tuple_element_t<I, T>> || ...);
}
template <typename T>
inline constexpr bool needs_apply_loop_v =
    needs_apply_loop_impl<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});

template <typename T, std::size_t... I>
void apply_columns(std::vector<T>& batch, std::size_t row,
                    const std::vector<std::vector<sb2>>& indicators, const define_t<T>& staging,
                    std::index_sequence<I...>) {
    (apply_one_column<I>(batch, row, indicators, staging), ...);
}

// ---- select()'s positional, struct-free single-row define -----------------
template <std::size_t I, typename... T>
void define_one_positional_output(OciStatement& stmt, std::tuple<T&...>& outs, std::vector<sb2>& indicators) {
    using Arg = std::remove_reference_t<std::tuple_element_t<I, std::tuple<T&...>>>;
    auto& out = std::get<I>(outs);
    constexpr ub4 position = I + 1;
    if constexpr (is_fixed_string_v<Arg>) {
        // A real indicator here too, same reason as define_one_column's
        // plain FixedString<N> branch: Oracle can't store/return an
        // empty VARCHAR2/CHAR distinct from NULL, so this can come back
        // NULL with no std::optional<FixedString<N>> involved at all --
        // apply_positional_outputs below clears it back to empty rather
        // than leaving whatever the caller's own buffer held before the
        // call.
        stmt.bindOutput(position, oci_type_code_v<Arg>, out.data(), static_cast<sb4>(Arg::capacity),
                        &out.length_ref(), &indicators[I]);
    } else {
        stmt.bindOutput(position, oci_type_code_v<Arg>, &out, sizeof(Arg), nullptr, nullptr);
    }
}

template <typename... T, std::size_t... I>
void define_positional_outputs(OciStatement& stmt, std::tuple<T&...>& outs, std::vector<sb2>& indicators,
                               std::index_sequence<I...>) {
    (define_one_positional_output<I, T...>(stmt, outs, indicators), ...);
}

// After execute(1), clears any FixedString<N> positional output whose
// indicator came back NULL -- see define_one_positional_output's comment.
// A no-op for every other argument type (arithmetic/OciDate can't be
// NULL through this path -- see positional_bindable's own restriction).
template <std::size_t I, typename... T>
void apply_one_positional_output(std::tuple<T&...>& outs, const std::vector<sb2>& indicators) {
    using Arg = std::remove_reference_t<std::tuple_element_t<I, std::tuple<T&...>>>;
    if constexpr (is_fixed_string_v<Arg>) {
        if (indicators[I] == OCI_IND_NULL) std::get<I>(outs).clear();
    }
}

template <typename... T, std::size_t... I>
void apply_positional_outputs(std::tuple<T&...>& outs, const std::vector<sb2>& indicators, std::index_sequence<I...>) {
    (apply_one_positional_output<I, T...>(outs, indicators), ...);
}

// Shared by both select_rows() overloads: defines OutT's columns as one
// array-of-struct batch, sets prefetch_rows, executes, then repeatedly
// fetches up to fetch_batch_size rows per fetch() call and hands each
// batch to on_batch.
template <typename OutT>
ExecResult run_select_fetch_loop(OciConnection& conn, OciStatement& stmt,
                                  std::size_t prefetch_rows, std::size_t fetch_batch_size,
                                  const std::function<void(const OutT*, std::size_t)>& on_batch) {
    std::vector<OutT> batch(fetch_batch_size);
    std::vector<std::vector<sb2>> indicators(boost::pfr::tuple_size_v<OutT>);
    define_t<OutT> staging{};

    stmt.set_prefetch_rows(static_cast<ub4>(prefetch_rows));

    // iters=0: nothing to fetch up front -- but this is also what
    // actually resolves the result set's column names against a real
    // database (see docs/oci_statement_lifecycle_notes.md).
    ExecResult result = stmt.execute(0);
    if (result.status == ExecStatus::Success) {
        const std::vector<ub4> positions = resolve_column_positions<OutT>(stmt, conn);
        bool all_matched = true;
        for (ub4 pos : positions) {
            if (pos == 0) { all_matched = false; break; }
        }

        if (!all_matched) {
            result = {ExecStatus::QueryError, OciCallResult{OCI_ERROR, 0, {}}};
        } else {
            define_columns(stmt, conn, batch, indicators, staging, positions,
                           std::make_index_sequence<boost::pfr::tuple_size_v<OutT>>{});

            for (;;) {
                const ExecResult fetch_result = stmt.fetch(static_cast<ub4>(fetch_batch_size));
                if (fetch_result.status != ExecStatus::Success) { result = fetch_result; break; }

                const ub4 rows_fetched = stmt.rows_fetched();
                if constexpr (needs_apply_loop_v<OutT>) {
                    for (ub4 row = 0; row < rows_fetched; ++row) {
                        apply_columns(batch, row, indicators, staging,
                                      std::make_index_sequence<boost::pfr::tuple_size_v<OutT>>{});
                    }
                }
                on_batch(batch.data(), rows_fetched);

                // The fetch CALL's own status is the real stopping signal,
                // not stmt.state() -- see docs/oci_statement_lifecycle_notes.md
                // for why (prefetch can make state() read EndOfFetch before
                // every buffered row is actually drained).
                if (fetch_result.call.status == OCI_NO_DATA) { result = fetch_result; break; }
            }
        }
    }
    return result;
}

// ---- array bind: one bindNameArray() per field against rows[offset], with
// its own OCIBindArrayOfStruct stride. Rebound fresh for every chunk
// (insert_rows below), pointed at that chunk's own starting row -- see
// oci_statement.h's own bindNameArray comment for why (a real crash
// against a real database, confirmed while building ideas/binding, from
// reusing a single bind across chunks via OCIStmtExecute's own rowoff
// instead).
template <std::size_t I, typename T>
void bind_array_field(OciStatement& stmt, std::vector<T>& rows,
                       std::vector<std::vector<sb2>>& indicators,
                       std::size_t offset, std::size_t count, std::string_view name) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    static_assert(!is_optional_v<FieldT>,
                  "insert_rows: optional<T> fields are not supported in the array-bind path -- "
                  "every row's optional would need to be engaged (an empty one has no address to "
                  "bind through), and there is no NULL semantics tracked for it here. Use "
                  "execute(conn, sql, row) in a loop for a row type with a nullable field instead.");
    static_assert(!is_oci_lob_v<FieldT>,
                  "insert_rows: LOB fields (OciClob/OciBlob) are not supported in the array-bind "
                  "path -- each row's LOB needs its own locator allocated and written, not a "
                  "fixed-stride raw buffer bindNameArray() can stride over. Use "
                  "execute(conn, sql, row) in a loop for a row type with a LOB field instead.");
    // Every field gets a real, always-OCI_IND_NOTNULL indicator array here
    // -- a null indicator crashed against a real database for an array
    // bind specifically, confirmed while building ideas/binding (see
    // oci_statement.h's bindNameArray comment).
    auto& ind = indicators[I];
    ind.assign(count, OCI_IND_NOTNULL);

    if constexpr (is_fixed_string_v<FieldT>) {
        auto& first = boost::pfr::get<I>(rows[offset]);
        stmt.bindNameArray(std::string(name), oci_type_code_v<FieldT>, first.data(),
                           static_cast<sb4>(FieldT::capacity), static_cast<sb4>(sizeof(T)), ind.data(),
                           &first.length_ref(), static_cast<ub4>(sizeof(T)));
    } else {
        stmt.bindNameArray(std::string(name), oci_type_code_v<FieldT>, &boost::pfr::get<I>(rows[offset]),
                           sizeof(FieldT), static_cast<sb4>(sizeof(T)), ind.data());
    }
}

template <typename T, std::size_t... I>
void bind_array_fields_impl(OciStatement& stmt, std::vector<T>& rows,
                             std::vector<std::vector<sb2>>& indicators,
                             std::size_t offset, std::size_t count, std::index_sequence<I...>) {
    constexpr auto names = field_names_of<T>();
    (bind_array_field<I>(stmt, rows, indicators, offset, count, names[I]), ...);
}

template <typename T>
void bind_array_fields(OciStatement& stmt, std::vector<T>& rows,
                        std::vector<std::vector<sb2>>& indicators, std::size_t offset, std::size_t count) {
    bind_array_fields_impl(stmt, rows, indicators, offset, count,
                            std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

} // namespace detail

inline ExecResult execute(OciConnection& conn, const std::string& sql) {
    OciStatement stmt(conn);
    stmt.prepare(sql);
    return stmt.execute(1);
}

inline ExecResult select_generic(OciConnection& conn, const std::string& sql,
                                 std::size_t prefetch_rows, std::size_t fetch_batch_size,
                                 const GenericBatchCallback& on_batch) {
    OciStatement stmt(conn);
    stmt.prepare(sql);
    stmt.set_prefetch_rows(static_cast<ub4>(prefetch_rows));

    // iters=0: nothing to fetch up front, but this is also what resolves
    // describeColumns()'s own information -- see
    // docs/oci_statement_lifecycle_notes.md for why that has to happen
    // after execute(), not before it.
    ExecResult result = stmt.execute(0);
    if (result.status != ExecStatus::Success) return result;

    const std::vector<ColumnInfo> columns = stmt.describeColumns();
    for (const auto& col : columns) {
        if (col.oracle_type == SQLT_CLOB || col.oracle_type == SQLT_BLOB) {
            return {ExecStatus::QueryError,
                    OciCallResult{OCI_ERROR, 0,
                        "select_generic: column '" + col.name + "' is a LOB -- a locator isn't a "
                        "flat byte buffer the way every other described type here is; use "
                        "select_rows<T>() with an OciClob/OciBlob field for a LOB column instead"}};
        }
    }

    std::vector<std::vector<unsigned char>> column_data(columns.size());
    std::vector<std::vector<sb2>> indicators(columns.size());
    std::vector<std::vector<ub2>> lengths(columns.size());

    for (std::size_t i = 0; i < columns.size(); ++i) {
        const ColumnInfo& col = columns[i];
        const bool is_text = (col.oracle_type == SQLT_CHR || col.oracle_type == SQLT_AFC);
        const ub4 size = col.data_size > 0 ? col.data_size : 1;
        column_data[i].assign(fetch_batch_size * size, 0);
        indicators[i].assign(fetch_batch_size, OCI_IND_NOTNULL);
        lengths[i].assign(fetch_batch_size, 0);
        // rlskip = sizeof(ub2): lengths[i] is its own tightly-packed
        // vector<ub2>, one entry per row -- a different stride from
        // pvskip's `size` (the column's own per-row byte width), which
        // is exactly why bindOutput() needed a real, separate rlskip
        // parameter rather than reusing elemSize for it (see its own
        // comment in oci_statement.h).
        stmt.bindOutput(col.position, col.oracle_type, column_data[i].data(), static_cast<sb4>(size),
                        is_text ? lengths[i].data() : nullptr, indicators[i].data(),
                        static_cast<sb4>(size), static_cast<ub4>(sizeof(ub2)));
    }

    for (;;) {
        const ExecResult fetch_result = stmt.fetch(static_cast<ub4>(fetch_batch_size));
        if (fetch_result.status != ExecStatus::Success) { result = fetch_result; break; }

        const ub4 rows_fetched = stmt.rows_fetched();
        const GenericBatch batch{columns, rows_fetched, column_data, indicators, lengths};
        on_batch(batch);

        // The fetch call's own status is the real stopping signal, not
        // stmt.state() -- see docs/oci_statement_lifecycle_notes.md.
        if (fetch_result.call.status == OCI_NO_DATA) { result = fetch_result; break; }
    }
    return result;
}

template <scalar_bindable T>
ExecResult execute(OciConnection& conn, const std::string& sql, T& params) {
    OciStatement stmt(conn);
    stmt.prepare(sql);

    std::vector<sb2> indicators;
    detail::bind_t<T> staging{};
    detail::bind_params(stmt, conn, params, indicators, staging);

    return stmt.execute(1);
}

template <scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT*, std::size_t)>& on_batch) {
    OciStatement stmt(conn);
    stmt.prepare(sql);
    return detail::run_select_fetch_loop<OutT>(conn, stmt, prefetch_rows, fetch_batch_size, on_batch);
}

template <scalar_bindable InT, scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql, InT& input,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT*, std::size_t)>& on_batch) {
    OciStatement stmt(conn);
    stmt.prepare(sql);

    std::vector<sb2> in_indicators;
    detail::bind_t<InT> in_staging{};
    detail::bind_params(stmt, conn, input, in_indicators, in_staging);

    return detail::run_select_fetch_loop<OutT>(conn, stmt, prefetch_rows, fetch_batch_size, on_batch);
}

template <positional_bindable... T>
ExecResult select(OciConnection& conn, const std::string& sql, T&... outputs) {
    OciStatement stmt(conn);
    stmt.prepare(sql);

    std::tuple<T&...> outs(outputs...);
    std::vector<sb2> indicators(sizeof...(T), OCI_IND_NOTNULL);
    detail::define_positional_outputs(stmt, outs, indicators, std::index_sequence_for<T...>{});

    // iters=1: for a SELECT, execute() itself fetches that many rows as
    // part of the same call -- no separate fetch() needed for exactly one
    // row. Zero matching rows comes back as OCI_NO_DATA directly from
    // this call, classified Success (see docs/oci_statement_lifecycle_notes.md)
    // -- and with no row actually fetched, indicators is never written to
    // by OCI at all, so apply_positional_outputs only runs when a row
    // genuinely came back (oci_status != OCI_NO_DATA), matching this
    // function's own documented "arguments left untouched on zero rows"
    // contract.
    const ExecResult result = stmt.execute(1);
    if (result.call.status != OCI_NO_DATA) {
        detail::apply_positional_outputs(outs, indicators, std::index_sequence_for<T...>{});
    }
    return result;
}

template <scalar_bindable T>
ExecResult insert_rows(OciConnection& conn, const std::string& sql, std::vector<T>& rows, std::size_t chunk_size) {
    if (rows.empty()) return {ExecStatus::Success, OciCallResult{OCI_SUCCESS, 0, {}}};

    OciStatement stmt(conn);
    stmt.prepare(sql);

    // Owned here, not inside bind_array_fields: each chunk's indicator
    // arrays must stay alive from that chunk's bind through its own
    // execute() call.
    std::vector<std::vector<sb2>> indicators(boost::pfr::tuple_size_v<T>);

    ExecResult result{ExecStatus::Success, OciCallResult{OCI_SUCCESS, 0, {}}};
    for (std::size_t offset = 0; offset < rows.size(); offset += chunk_size) {
        const std::size_t this_chunk = std::min(chunk_size, rows.size() - offset);
        detail::bind_array_fields(stmt, rows, indicators, offset, this_chunk);
        result = stmt.execute(static_cast<ub4>(this_chunk));
        if (result.status != ExecStatus::Success) break;
    }
    return result;
}

} // namespace marketlib::db::oracle
