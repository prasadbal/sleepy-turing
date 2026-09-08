#pragma once
// Implementation of oci_client.h's free functions -- see that file for the
// interface and behavioral contract. Not meant to be included directly.
#include "binding/oci_client.h"

#include <boost/pfr.hpp>
#include <optional>
#include <tuple>
#include <vector>

namespace binding {
namespace detail {

// ---- IN: bind a struct's fields by name ------------------------------------
//
// A plain field binds straight through its own address -- `params` is the
// caller's and has to live until execute() runs anyway, same as it always
// has. An optional<T> field can't: an empty optional has no address to bind
// through, and OCI holds onto whatever pointer it's given until execute()
// actually runs, not just for the duration of this call -- so it needs real
// storage that outlives this whole function. `staging`, owned by the caller
// (see bind_params below), is that storage.
template <typename T>
using in_staging_slot_t = std::conditional_t<is_optional_v<T>, optional_value_t<T>, std::monostate>;

template <typename T, std::size_t... I>
auto in_staging_tuple(std::index_sequence<I...>)
    -> std::tuple<in_staging_slot_t<boost::pfr::tuple_element_t<I, T>>...>;
template <typename T>
using in_staging_t = decltype(in_staging_tuple<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

template <std::size_t I, typename T>
void bind_one_param(OCIStmt* stmt, OciConnection& conn, T& params, std::string_view name,
                     std::vector<sb2>& indicators, in_staging_t<T>& staging) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(params);
    const std::string placeholder = ":" + std::string(name);
    OCIBind* bind_handle = nullptr;

    if constexpr (is_optional_v<FieldT>) {
        auto& stage = std::get<I>(staging);
        if (field) { stage = *field; indicators[I] = OCI_IND_NOTNULL; }
        else       { stage = {};     indicators[I] = OCI_IND_NULL; }
        OCIBindByName(stmt, &bind_handle, conn.err(),
                      reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                      &stage, sizeof(stage), oci_type_code_v<FieldT>, &indicators[I],
                      nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    } else {
        indicators[I] = OCI_IND_NOTNULL;
        OCIBindByName(stmt, &bind_handle, conn.err(),
                      reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                      &field, sizeof(field), oci_type_code_v<FieldT>, nullptr,
                      nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    }
}

template <typename T, std::size_t... I>
void bind_params_impl(OCIStmt* stmt, OciConnection& conn, T& params, std::vector<sb2>& indicators,
                       in_staging_t<T>& staging, std::index_sequence<I...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_param<I>(stmt, conn, params, names[I], indicators, staging), ...);
}

// indicators/staging are owned by the caller (see run_execute_once below)
// rather than hidden as locals in here, because they must stay alive from
// this call through OciConnection::execute() -- the same lifetime rule the
// field-level comment above explains, just at the whole-struct level.
template <typename T>
void bind_params(OCIStmt* stmt, OciConnection& conn, T& params,
                  std::vector<sb2>& indicators, in_staging_t<T>& staging) {
    indicators.assign(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
    bind_params_impl(stmt, conn, params, indicators, staging,
                      std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
}

// ---- OUT: define a batch of result rows ------------------------------------
//
// One OCIDefineByPos per column, then OCIDefineArrayOfStruct telling OCI the
// byte stride from one row's value to the next (batch is one contiguous
// vector<T>) -- the array-fetch mechanism, mirrored from the bind side's own
// array-insert equivalent. A plain column defines straight into batch[0]'s
// own address; an optional<T> column needs its own batch-sized staging
// array (same reason as the bind side) plus a batch-sized indicator array.
template <typename T>
using out_staging_slot_t = std::conditional_t<is_optional_v<T>, std::vector<optional_value_t<T>>, std::monostate>;

template <typename T, std::size_t... I>
auto out_staging_tuple(std::index_sequence<I...>)
    -> std::tuple<out_staging_slot_t<boost::pfr::tuple_element_t<I, T>>...>;
template <typename T>
using out_staging_t = decltype(out_staging_tuple<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

// One indicator array per field, sized to the batch -- but only ever
// populated for an optional<T> field. A plain field defines with no
// indicator at all: an unexpected NULL there silently leaves that row's
// slot holding whatever the batch buffer already had. See oci_client.h's
// select_rows doc comment for why that's a deliberate trade for this pass,
// not an oversight.
template <std::size_t I, typename T>
void define_one_column(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                        std::vector<std::vector<sb2>>& indicators, out_staging_t<T>& staging) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    constexpr ub4 position = I + 1;
    OCIDefine* define_handle = nullptr;

    if constexpr (is_optional_v<FieldT>) {
        using ElemT = optional_value_t<FieldT>;
        auto& ind = indicators[I];
        ind.assign(batch.size(), OCI_IND_NOTNULL);
        auto& stage = std::get<I>(staging);
        stage.assign(batch.size(), ElemT{});
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                       stage.data(), sizeof(ElemT), oci_type_code_v<FieldT>,
                       ind.data(), nullptr, nullptr, OCI_DEFAULT);
        OCIDefineArrayOfStruct(define_handle, conn.err(), sizeof(ElemT), sizeof(sb2), 0, 0);
    } else {
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                       &boost::pfr::get<I>(batch[0]), sizeof(FieldT), oci_type_code_v<FieldT>,
                       nullptr, nullptr, nullptr, OCI_DEFAULT);
        OCIDefineArrayOfStruct(define_handle, conn.err(), sizeof(T), sizeof(sb2), 0, 0);
    }
}

template <typename T, std::size_t... I>
void define_columns(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                     std::vector<std::vector<sb2>>& indicators, out_staging_t<T>& staging,
                     std::index_sequence<I...>) {
    (define_one_column<I>(stmt, conn, batch, indicators, staging), ...);
}

// After a fetch, copies each optional field's per-row staging value (or
// nullopt, per its indicator) into batch[row]. A no-op for every other
// field -- its value already landed directly in batch[row] via the plain
// OCIDefineArrayOfStruct path above, nothing further to do.
template <std::size_t I, typename T>
void apply_one_column(std::vector<T>& batch, std::size_t row,
                       const std::vector<std::vector<sb2>>& indicators, const out_staging_t<T>& staging) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    if constexpr (is_optional_v<FieldT>) {
        auto& field = boost::pfr::get<I>(batch[row]);
        field = (indicators[I][row] == OCI_IND_NULL)
                    ? std::nullopt
                    : std::make_optional(std::get<I>(staging)[row]);
    }
}

template <typename T, std::size_t... I>
void apply_columns(std::vector<T>& batch, std::size_t row,
                    const std::vector<std::vector<sb2>>& indicators, const out_staging_t<T>& staging,
                    std::index_sequence<I...>) {
    (apply_one_column<I>(batch, row, indicators, staging), ...);
}

// Shared by both select_rows() overloads: defines OutT's columns as one
// array-of-struct batch, sets prefetch_rows (Oracle's own client-side
// round-trip batching -- see oci_client.h's select_rows doc comment for why
// this is a separate number from fetch_batch_size), executes, then
// repeatedly fetches up to fetch_batch_size rows per OCIStmtFetch2 call and
// hands each batch to on_batch. Does not free `stmt` -- that stays the
// caller's responsibility, since what else needs freeing alongside it
// differs between the two overloads (nothing, on the with-input side, since
// there's no LOB/collection cleanup to do in this scalars-only file).
template <typename OutT>
ExecResult run_select_fetch_loop(OciConnection& conn, OCIStmt* stmt,
                                  std::size_t prefetch_rows, std::size_t fetch_batch_size,
                                  const std::function<void(const OutT*, std::size_t)>& on_batch) {
    std::vector<OutT> batch(fetch_batch_size);
    std::vector<std::vector<sb2>> indicators(boost::pfr::tuple_size_v<OutT>);
    out_staging_t<OutT> staging{};
    define_columns(stmt, conn, batch, indicators, staging,
                   std::make_index_sequence<boost::pfr::tuple_size_v<OutT>>{});

    ub4 prefetch = static_cast<ub4>(prefetch_rows);
    OCIAttrSet(stmt, OCI_HTYPE_STMT, &prefetch, 0, OCI_ATTR_PREFETCH_ROWS, conn.err());

    // iters=0: nothing to fetch up front, the loop below does all of it.
    ExecResult result = conn.execute(stmt, 0);
    if (result.status != ExecStatus::Success) return result;

    for (;;) {
        const sword status = OCIStmtFetch2(stmt, conn.err(), static_cast<ub4>(fetch_batch_size),
                                            OCI_FETCH_NEXT, 0, OCI_DEFAULT);
        if (status != OCI_SUCCESS && status != OCI_NO_DATA) {
            return conn.is_disconnect_error() ? ExecResult{ExecStatus::ConnectionLost, status}
                                               : ExecResult{ExecStatus::QueryError, status};
        }

        ub4 rows_fetched = 0;
        ub4 attr_size = sizeof(rows_fetched);
        OCIAttrGet(stmt, OCI_HTYPE_STMT, &rows_fetched, &attr_size, OCI_ATTR_ROWS_FETCHED, conn.err());

        for (ub4 row = 0; row < rows_fetched; ++row) {
            apply_columns(batch, row, indicators, staging,
                          std::make_index_sequence<boost::pfr::tuple_size_v<OutT>>{});
        }
        on_batch(batch.data(), rows_fetched);

        if (status == OCI_NO_DATA) return {ExecStatus::Success, OCI_SUCCESS};
    }
}

} // namespace detail

inline ExecResult execute(OciConnection& conn, const std::string& sql) {
    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(), reinterpret_cast<const text*>(sql.c_str()),
                   static_cast<ub4>(sql.size()), OCI_NTV_SYNTAX, OCI_DEFAULT);
    const ExecResult result = conn.execute(stmt, 1);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return result;
}

template <scalar_bindable T>
ExecResult execute(OciConnection& conn, const std::string& sql, T& params) {
    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(), reinterpret_cast<const text*>(sql.c_str()),
                   static_cast<ub4>(sql.size()), OCI_NTV_SYNTAX, OCI_DEFAULT);

    std::vector<sb2> indicators;
    detail::in_staging_t<T> staging{};
    detail::bind_params(stmt, conn, params, indicators, staging);

    const ExecResult result = conn.execute(stmt, 1);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return result;
}

template <scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT*, std::size_t)>& on_batch) {
    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(), reinterpret_cast<const text*>(sql.c_str()),
                   static_cast<ub4>(sql.size()), OCI_NTV_SYNTAX, OCI_DEFAULT);

    const ExecResult result = detail::run_select_fetch_loop<OutT>(conn, stmt, prefetch_rows, fetch_batch_size, on_batch);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return result;
}

template <scalar_bindable InT, scalar_bindable OutT>
ExecResult select_rows(OciConnection& conn, const std::string& sql, InT& input,
                        std::size_t prefetch_rows, std::size_t fetch_batch_size,
                        const std::function<void(const OutT*, std::size_t)>& on_batch) {
    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(), reinterpret_cast<const text*>(sql.c_str()),
                   static_cast<ub4>(sql.size()), OCI_NTV_SYNTAX, OCI_DEFAULT);

    std::vector<sb2> in_indicators;
    detail::in_staging_t<InT> in_staging{};
    detail::bind_params(stmt, conn, input, in_indicators, in_staging);

    const ExecResult result = detail::run_select_fetch_loop<OutT>(conn, stmt, prefetch_rows, fetch_batch_size, on_batch);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return result;
}

} // namespace binding
