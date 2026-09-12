#pragma once
// Implementation of oci_client.h's free functions -- see that file for the
// interface and behavioral contract. Not meant to be included directly.
#include "binding/oci_client.h"

#include <algorithm>
#include <boost/pfr.hpp>
#include <optional>
#include <tuple>
#include <vector>

namespace binding {
namespace detail {

// ---- LOB locator lifecycle: allocate/write on the way in, allocate/read on
// the way out, free either way. Shared by both the IN (bind_one_param) and
// OUT (define_one_column/apply_one_column) sides below -- everything else
// in this file binds/defines straight through a field's own fixed-size
// address, but a LOB locator is a real OCI-managed descriptor with its own
// allocate/populate/free lifecycle, not a buffer OCI reads/writes in place.
// ---------------------------------------------------------------------------

// Allocates a temporary LOB, writes `data`/`size` into it in one
// OCI_ONE_PIECE call, and returns the locator -- what bind_one_param binds
// for an OciClob/OciBlob IN parameter. Caller frees it via free_temp_lob
// once the execute() that used it has run.
inline OCILobLocator* make_temp_lob(OciConnection& conn, const void* data, std::size_t size, ub1 lob_type) {
    OCILobLocator* locator = nullptr;
    OCIDescriptorAlloc(conn.env(), reinterpret_cast<void**>(&locator), OCI_DTYPE_LOB, 0, nullptr);
    OCILobCreateTemporary(conn.svc(), conn.err(), locator, 0, SQLCS_IMPLICIT, lob_type, 0, OCI_DURATION_SESSION);
    if (size > 0) {
        oraub8 byte_amt = static_cast<oraub8>(size);
        oraub8 char_amt = 0;
        OCILobWrite2(conn.svc(), conn.err(), locator, &byte_amt, &char_amt, 1,
                     const_cast<void*>(data), static_cast<oraub8>(size), OCI_ONE_PIECE,
                     nullptr, nullptr, 0, SQLCS_IMPLICIT);
    }
    return locator;
}

inline void free_temp_lob(OciConnection& conn, OCILobLocator* locator) {
    if (!locator) return;
    OCILobFreeTemporary(conn.svc(), conn.err(), locator);
    OCIDescriptorFree(locator, OCI_DTYPE_LOB);
}

// Reads a fetched LOB's entire contents in one call: OCILobGetLength2 first
// (characters for a CLOB, bytes for a BLOB -- see the Oracle docs for
// OCILobGetLength2), then a single OCI_ONE_PIECE OCILobRead2 sized off
// that. A CLOB's length is in *characters*, not bytes, so the byte buffer
// needs headroom for a multi-byte charset -- AL32UTF8's worst case is 4
// bytes/character -- a BLOB's length is already in bytes, no expansion
// needed. Simpler than a polling FIRST_PIECE/NEXT_PIECE read loop, at the
// cost of one extra round trip (the length call) and holding the whole
// value in memory at once -- the right tradeoff for the report/config-sized
// LOBs this library targets, not a multi-gigabyte streaming case.
inline std::string read_lob_bytes(OciConnection& conn, OCILobLocator* locator, bool is_char_lob) {
    oraub8 length = 0;
    OCILobGetLength2(conn.svc(), conn.err(), locator, &length);
    if (length == 0) return {};
    const std::size_t buffer_bytes = is_char_lob ? static_cast<std::size_t>(length) * 4 + 16
                                                  : static_cast<std::size_t>(length);
    std::string buf(buffer_bytes, '\0');
    oraub8 byte_amt = static_cast<oraub8>(buffer_bytes);
    oraub8 char_amt = 0;
    OCILobRead2(conn.svc(), conn.err(), locator, &byte_amt, &char_amt, 1,
                buf.data(), static_cast<oraub8>(buffer_bytes), OCI_ONE_PIECE,
                nullptr, nullptr, 0, SQLCS_IMPLICIT);
    buf.resize(static_cast<std::size_t>(byte_amt));
    return buf;
}

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
using bind_slot_t = std::conditional_t<is_optional_v<T>, optional_value_t<T>, std::monostate>;

template <typename T, std::size_t... I>
auto bind_tuple(std::index_sequence<I...>)
    -> std::tuple<bind_slot_t<boost::pfr::tuple_element_t<I, T>>...>;
template <typename T>
using bind_t = decltype(bind_tuple<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

template <std::size_t I, typename T>
void bind_one_param(OCIStmt* stmt, OciConnection& conn, T& params, std::string_view name,
                     std::vector<sb2>& indicators, bind_t<T>& staging,
                     std::vector<OCILobLocator*>& lob_locators) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    auto& field = boost::pfr::get<I>(params);
    const std::string placeholder = ":" + std::string(name);
    OCIBind* bind_handle = nullptr;

    if constexpr (is_oci_lob_v<FieldT>) {
        // Never optional here -- see scalar_field_predicate's comment in
        // oci_client.h for why std::optional<OciClob/OciBlob> doesn't even
        // satisfy scalar_bindable yet.
        indicators[I] = OCI_IND_NOTNULL;
        OCILobLocator*& locator = lob_locators[I];
        if constexpr (is_oci_clob_v<FieldT>) {
            locator = make_temp_lob(conn, field.text_data.data(), field.text_data.size(), OCI_TEMP_CLOB);
        } else {
            locator = make_temp_lob(conn, field.binary_data.data(), field.binary_data.size(), OCI_TEMP_BLOB);
        }
        OCIBindByName(stmt, &bind_handle, conn.err(),
                      reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                      &locator, sizeof(OCILobLocator*), oci_type_code_v<FieldT>, nullptr,
                      nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
    } else if constexpr (is_optional_v<FieldT>) {
        using ElemT = optional_value_t<FieldT>;
        auto& stage = std::get<I>(staging);
        if (field) { stage = *field; indicators[I] = OCI_IND_NOTNULL; }
        else       { stage = {};     indicators[I] = OCI_IND_NULL; }
        if constexpr (is_fixed_string_v<ElemT>) {
            // stage.length_ref() as alenp: the *content* length within the
            // Capacity-sized buffer, since bind_size below is the buffer's
            // full capacity (what OCI is told it may read from), not how
            // much of it is meaningful for this particular value.
            OCIBindByName(stmt, &bind_handle, conn.err(),
                          reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                          stage.data(), static_cast<sb4>(ElemT::capacity), oci_type_code_v<FieldT>,
                          &indicators[I], &stage.length_ref(), nullptr, 0, nullptr, OCI_DEFAULT);
        } else {
            OCIBindByName(stmt, &bind_handle, conn.err(),
                          reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                          &stage, sizeof(stage), oci_type_code_v<FieldT>, &indicators[I],
                          nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        }
    } else if constexpr (is_fixed_string_v<FieldT>) {
        indicators[I] = OCI_IND_NOTNULL;
        OCIBindByName(stmt, &bind_handle, conn.err(),
                      reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                      field.data(), static_cast<sb4>(FieldT::capacity), oci_type_code_v<FieldT>, nullptr,
                      &field.length_ref(), nullptr, 0, nullptr, OCI_DEFAULT);
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
                       bind_t<T>& staging, std::vector<OCILobLocator*>& lob_locators,
                       std::index_sequence<I...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_one_param<I>(stmt, conn, params, names[I], indicators, staging, lob_locators), ...);
}

// indicators/staging/lob_locators are owned by the caller (see execute()
// below) rather than hidden as locals in here, because they must stay
// alive from this call through OciConnection::execute() -- the same
// lifetime rule the field-level comment above explains, just at the
// whole-struct level. The caller also owns freeing lob_locators'
// non-null entries (via free_temp_lob) after execute() runs -- this
// function only allocates and writes them, same division of
// responsibility bind vs. free has everywhere else in this file.
template <typename T>
void bind_params(OCIStmt* stmt, OciConnection& conn, T& params, std::vector<sb2>& indicators,
                  bind_t<T>& staging, std::vector<OCILobLocator*>& lob_locators) {
    indicators.assign(boost::pfr::tuple_size_v<T>, OCI_IND_NOTNULL);
    lob_locators.assign(boost::pfr::tuple_size_v<T>, nullptr);
    bind_params_impl(stmt, conn, params, indicators, staging, lob_locators,
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
using define_slot_t = std::conditional_t<is_optional_v<T>, std::vector<optional_value_t<T>>,
                            std::conditional_t<is_oci_lob_v<T>, std::vector<OCILobLocator*>, std::monostate>>;

template <typename T, std::size_t... I>
auto define_tuple(std::index_sequence<I...>)
    -> std::tuple<define_slot_t<boost::pfr::tuple_element_t<I, T>>...>;
template <typename T>
using define_t = decltype(define_tuple<T>(std::make_index_sequence<boost::pfr::tuple_size_v<T>>{}));

// One indicator array per field, sized to the batch -- but only ever
// populated for an optional<T> field. A plain field defines with no
// indicator at all: an unexpected NULL there silently leaves that row's
// slot holding whatever the batch buffer already had. See oci_client.h's
// select_rows doc comment for why that's a deliberate trade for this pass,
// not an oversight.
template <std::size_t I, typename T>
void define_one_column(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                        std::vector<std::vector<sb2>>& indicators, define_t<T>& staging) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    constexpr ub4 position = I + 1;
    OCIDefine* define_handle = nullptr;

    if constexpr (is_optional_v<FieldT>) {
        using ElemT = optional_value_t<FieldT>;
        auto& ind = indicators[I];
        ind.assign(batch.size(), OCI_IND_NOTNULL);
        auto& stage = std::get<I>(staging);
        stage.assign(batch.size(), ElemT{});
        if constexpr (is_fixed_string_v<ElemT>) {
            // rlskip = sizeof(ElemT): stage is a tightly-packed
            // vector<FixedString<N>>, so each row's fetched length lands in
            // that row's own length_ field, one whole FixedString<N> apart.
            OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                           stage[0].data(), static_cast<sb4>(ElemT::capacity), oci_type_code_v<FieldT>,
                           ind.data(), &stage[0].length_ref(), nullptr, OCI_DEFAULT);
            OCIDefineArrayOfStruct(define_handle, conn.err(),
                                   static_cast<ub4>(sizeof(ElemT)), static_cast<ub4>(sizeof(sb2)),
                                   static_cast<ub4>(sizeof(ElemT)), 0);
        } else {
            OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                           stage.data(), sizeof(ElemT), oci_type_code_v<FieldT>,
                           ind.data(), nullptr, nullptr, OCI_DEFAULT);
            OCIDefineArrayOfStruct(define_handle, conn.err(), sizeof(ElemT), sizeof(sb2), 0, 0);
        }
    } else if constexpr (is_oci_lob_v<FieldT>) {
        // Unlike every other branch here, the buffer OCI writes into isn't
        // part of the row struct at all: a LOB locator is a separate
        // OCI-managed allocation, so the "column" this defines against is
        // a standalone, batch-sized vector<OCILobLocator*> living in
        // `staging`, one entry per row, each individually allocated up
        // front. OCIDefineArrayOfStruct's pvskip is that vector's own
        // element stride (sizeof(OCILobLocator*)), not sizeof(T) -- there
        // is no host struct here the way FixedString<N>/optional<T> have.
        auto& ind = indicators[I];
        ind.assign(batch.size(), OCI_IND_NOTNULL);
        auto& locators = std::get<I>(staging);
        locators.assign(batch.size(), nullptr);
        for (auto& loc : locators) {
            OCIDescriptorAlloc(conn.env(), reinterpret_cast<void**>(&loc), OCI_DTYPE_LOB, 0, nullptr);
        }
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                       locators.data(), sizeof(OCILobLocator*), oci_type_code_v<FieldT>,
                       ind.data(), nullptr, nullptr, OCI_DEFAULT);
        OCIDefineArrayOfStruct(define_handle, conn.err(),
                               static_cast<ub4>(sizeof(OCILobLocator*)), static_cast<ub4>(sizeof(sb2)), 0, 0);
    } else if constexpr (is_fixed_string_v<FieldT>) {
        // rlskip = sizeof(T): OCI reports each row's fetched length into
        // that row's own FixedString::length_, one whole row apart -- same
        // stride pvskip already uses for the value itself.
        auto& first = boost::pfr::get<I>(batch[0]);
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                       first.data(), static_cast<sb4>(FieldT::capacity), oci_type_code_v<FieldT>,
                       nullptr, &first.length_ref(), nullptr, OCI_DEFAULT);
        OCIDefineArrayOfStruct(define_handle, conn.err(),
                               static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)),
                               static_cast<ub4>(sizeof(T)), 0);
    } else {
        OCIDefineByPos(stmt, &define_handle, conn.err(), position,
                       &boost::pfr::get<I>(batch[0]), sizeof(FieldT), oci_type_code_v<FieldT>,
                       nullptr, nullptr, nullptr, OCI_DEFAULT);
        OCIDefineArrayOfStruct(define_handle, conn.err(), sizeof(T), sizeof(sb2), 0, 0);
    }
}

template <typename T, std::size_t... I>
void define_columns(OCIStmt* stmt, OciConnection& conn, std::vector<T>& batch,
                     std::vector<std::vector<sb2>>& indicators, define_t<T>& staging,
                     std::index_sequence<I...>) {
    (define_one_column<I>(stmt, conn, batch, indicators, staging), ...);
}

// After a fetch, copies each optional field's per-row staging value (or
// nullopt, per its indicator) into batch[row]. A no-op for every other
// field -- its value already landed directly in batch[row] via the plain
// OCIDefineArrayOfStruct path above, nothing further to do.
template <std::size_t I, typename T>
void apply_one_column(std::vector<T>& batch, std::size_t row, OciConnection& conn,
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
            const std::string bytes = read_lob_bytes(conn, std::get<I>(staging)[row], is_oci_clob_v<FieldT>);
            if constexpr (is_oci_clob_v<FieldT>) field.text_data = bytes;
            else field.binary_data.assign(bytes.begin(), bytes.end());
        }
    }
}

template <typename T, std::size_t... I>
void apply_columns(std::vector<T>& batch, std::size_t row, OciConnection& conn,
                    const std::vector<std::vector<sb2>>& indicators, const define_t<T>& staging,
                    std::index_sequence<I...>) {
    (apply_one_column<I>(batch, row, conn, indicators, staging), ...);
}

// Frees every locator define_one_column allocated for a LOB column --
// called once, after the whole fetch loop finishes (success or error),
// never per-batch: the same locator array is reused across every
// OCIStmtFetch2 call on a given define, not reallocated each time.
template <std::size_t I, typename T>
void free_one_column_lobs(define_t<T>& staging) {
    using FieldT = boost::pfr::tuple_element_t<I, T>;
    if constexpr (is_oci_lob_v<FieldT>) {
        for (OCILobLocator* loc : std::get<I>(staging)) {
            if (loc) OCIDescriptorFree(loc, OCI_DTYPE_LOB);
        }
    }
}

template <typename T, std::size_t... I>
void free_lob_columns(define_t<T>& staging, std::index_sequence<I...>) {
    (free_one_column_lobs<I, T>(staging), ...);
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
    define_t<OutT> staging{};
    define_columns(stmt, conn, batch, indicators, staging,
                   std::make_index_sequence<boost::pfr::tuple_size_v<OutT>>{});

    ub4 prefetch = static_cast<ub4>(prefetch_rows);
    OCIAttrSet(stmt, OCI_HTYPE_STMT, &prefetch, 0, OCI_ATTR_PREFETCH_ROWS, conn.err());

    // iters=0: nothing to fetch up front, the loop below does all of it.
    ExecResult result = conn.execute(stmt, 0);
    if (result.status == ExecStatus::Success) {
        for (;;) {
            const sword status = OCIStmtFetch2(stmt, conn.err(), static_cast<ub4>(fetch_batch_size),
                                                OCI_FETCH_NEXT, 0, OCI_DEFAULT);
            if (status != OCI_SUCCESS && status != OCI_NO_DATA) {
                result = conn.is_disconnect_error() ? ExecResult{ExecStatus::ConnectionLost, status}
                                                     : ExecResult{ExecStatus::QueryError, status};
                break;
            }

            ub4 rows_fetched = 0;
            ub4 attr_size = sizeof(rows_fetched);
            OCIAttrGet(stmt, OCI_HTYPE_STMT, &rows_fetched, &attr_size, OCI_ATTR_ROWS_FETCHED, conn.err());

            for (ub4 row = 0; row < rows_fetched; ++row) {
                apply_columns(batch, row, conn, indicators, staging,
                              std::make_index_sequence<boost::pfr::tuple_size_v<OutT>>{});
            }
            on_batch(batch.data(), rows_fetched);

            if (status == OCI_NO_DATA) { result = {ExecStatus::Success, OCI_SUCCESS}; break; }
        }
    }

    // Runs on every exit path (success, no rows, or a mid-loop error) --
    // a define_one_column LOB branch always allocates its locators up
    // front, so they need freeing regardless of how the loop above ended.
    free_lob_columns<OutT>(staging, std::make_index_sequence<boost::pfr::tuple_size_v<OutT>>{});
    return result;
}

// ---- array bind: one OCIBindByName per field against rows[0], plus
// OCIBindArrayOfStruct telling OCI the stride to the next row. Rebound
// fresh for every chunk (insert_rows below), pointed at that chunk's own
// starting row -- an earlier version of this bound once against rows[0]
// and reused that single bind across every chunk via OCIStmtExecute's own
// rowoff parameter (intended for exactly this: re-running a subset of an
// already-bound array without rebinding). That crashed on the *second*
// chunk against a real database (confirmed with gdb: the first
// OCIStmtExecute, rowoff=0, succeeds; the next one, rowoff>0 against the
// same bind, segfaults inside OCI's own network-marshaling code,
// ttcacs/ttci2n) -- root cause not identified, but rebinding per chunk is
// the conventional, unambiguously-documented pattern, so this uses that
// instead of continuing to chase rowoff's exact real-world behavior here.
// ---------------------------------------------------------------------

template <std::size_t I, typename T>
void bind_array_field(OCIStmt* stmt, OciConnection& conn, std::vector<T>& rows,
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
                  "path -- each row's LOB needs its own locator allocated via OCIDescriptorAlloc "
                  "and written via OCILobWrite2, not a fixed-stride raw buffer OCIBindArrayOfStruct "
                  "can stride over. Use execute(conn, sql, row) in a loop for a row type with a "
                  "LOB field instead.");
    // Every field gets a real, always-OCI_IND_NOTNULL indicator array here,
    // even though nothing in this path is ever actually NULL: a null indp
    // crashed against a real database for an array bind (OCIBindArrayOfStruct
    // + iters > 1), even though the exact same nullptr indp is fine for a
    // single-row bind (execute(conn, sql, row) does this safely) and for an
    // array *define* on the fetch side (select_rows() does this safely too,
    // see run_select_fetch_loop). The mock does not exercise this at all,
    // since it never dereferences indp either way.
    auto& ind = indicators[I];
    ind.assign(count, OCI_IND_NOTNULL);

    const std::string placeholder = ":" + std::string(name);
    OCIBind* bind_handle = nullptr;

    if constexpr (is_fixed_string_v<FieldT>) {
        auto& first = boost::pfr::get<I>(rows[offset]);
        OCIBindByName(stmt, &bind_handle, conn.err(),
                      reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                      first.data(), static_cast<sb4>(FieldT::capacity), oci_type_code_v<FieldT>,
                      ind.data(), &first.length_ref(), nullptr, 0, nullptr, OCI_DEFAULT);
        // alskip = sizeof(T): each row's own length_ field sits inside its
        // own FixedString, one whole row apart from the previous row's --
        // same stride pvskip already uses for the value itself.
        OCIBindArrayOfStruct(bind_handle, conn.err(), static_cast<ub4>(sizeof(T)),
                              static_cast<ub4>(sizeof(sb2)), static_cast<ub4>(sizeof(T)), 0);
    } else {
        OCIBindByName(stmt, &bind_handle, conn.err(),
                      reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                      &boost::pfr::get<I>(rows[offset]), sizeof(FieldT), oci_type_code_v<FieldT>,
                      ind.data(), nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        OCIBindArrayOfStruct(bind_handle, conn.err(), static_cast<ub4>(sizeof(T)), static_cast<ub4>(sizeof(sb2)), 0, 0);
    }
}

template <typename T, std::size_t... I>
void bind_array_fields_impl(OCIStmt* stmt, OciConnection& conn, std::vector<T>& rows,
                             std::vector<std::vector<sb2>>& indicators,
                             std::size_t offset, std::size_t count, std::index_sequence<I...>) {
    constexpr auto names = boost::pfr::names_as_array<T>();
    (bind_array_field<I>(stmt, conn, rows, indicators, offset, count, names[I]), ...);
}

// Binds `count` rows starting at rows[offset] -- called once per chunk from
// insert_rows, not once for the whole vector (see the rowoff/rebind
// comment above).
template <typename T>
void bind_array_fields(OCIStmt* stmt, OciConnection& conn, std::vector<T>& rows,
                        std::vector<std::vector<sb2>>& indicators, std::size_t offset, std::size_t count) {
    bind_array_fields_impl(stmt, conn, rows, indicators, offset, count,
                            std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
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
    detail::bind_t<T> staging{};
    std::vector<OCILobLocator*> lob_locators;
    detail::bind_params(stmt, conn, params, indicators, staging, lob_locators);

    const ExecResult result = conn.execute(stmt, 1);
    for (OCILobLocator* loc : lob_locators) detail::free_temp_lob(conn, loc);
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
    detail::bind_t<InT> in_staging{};
    std::vector<OCILobLocator*> in_lob_locators;
    detail::bind_params(stmt, conn, input, in_indicators, in_staging, in_lob_locators);

    const ExecResult result = detail::run_select_fetch_loop<OutT>(conn, stmt, prefetch_rows, fetch_batch_size, on_batch);
    for (OCILobLocator* loc : in_lob_locators) detail::free_temp_lob(conn, loc);
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return result;
}

template <scalar_bindable T>
ExecResult insert_rows(OciConnection& conn, const std::string& sql, std::vector<T>& rows, std::size_t chunk_size) {
    if (rows.empty()) return {ExecStatus::Success, OCI_SUCCESS};

    OCIStmt* stmt = nullptr;
    OCIHandleAlloc(conn.env(), reinterpret_cast<void**>(&stmt), OCI_HTYPE_STMT, 0, nullptr);
    OCIStmtPrepare(stmt, conn.err(), reinterpret_cast<const text*>(sql.c_str()),
                   static_cast<ub4>(sql.size()), OCI_NTV_SYNTAX, OCI_DEFAULT);

    // Owned here, not inside bind_array_fields: each chunk's indicator
    // arrays must stay alive from that chunk's bind through its own
    // execute() call.
    std::vector<std::vector<sb2>> indicators(boost::pfr::tuple_size_v<T>);

    ExecResult result{ExecStatus::Success, OCI_SUCCESS};
    for (std::size_t offset = 0; offset < rows.size(); offset += chunk_size) {
        const std::size_t this_chunk = std::min(chunk_size, rows.size() - offset);
        detail::bind_array_fields(stmt, conn, rows, indicators, offset, this_chunk);
        result = conn.execute(stmt, static_cast<ub4>(this_chunk));
        if (result.status != ExecStatus::Success) break;
    }

    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    return result;
}

} // namespace binding
