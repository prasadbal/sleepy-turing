#pragma once
// Minimal Oracle OCI stand-in, used only when the real Oracle client headers
// are not available -- see oci_compat.h, which picks between this file and
// the real <oci.h> and never includes both (both would define the same
// global OCI_* symbols and clash).
//
// This mirrors just enough of the real OCI function signatures that code
// written against this header compiles unchanged against the real thing.
// It is NOT a faithful OCI implementation and NOT for production use --
// it exists to exercise the reflection/reconnect/retry logic in this
// directory without an Oracle client install.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

// ---- Base types -------------------------------------------------------
using text   = unsigned char;
using dvoid  = void;
using sb2    = short;
using sb4    = int;
using ub4    = unsigned int;
using ub2    = unsigned short;
using ub1    = unsigned char;
using sword  = int;
using oraub8 = unsigned long long;

// ---- Opaque handle types -----------------------------------------------
struct OCIEnv;
struct OCISvcCtx;
struct OCIError;
struct OCIStmt;
struct OCIBind;
struct OCIDefine;
struct OCILobLocator;

// ---- Handle/attribute/mode constants -----------------------------------
// Values are internal to this mock -- they only need to be self-consistent,
// since the mock and the real <oci.h> are never compiled together.
#define OCI_DEFAULT       0
#define OCI_OBJECT        0x2 // env-create mode needed for OCIType/OCIObjectNew/OCICollAppend (oci_collection_bind.h)
#define OCI_HTYPE_ENV     1
#define OCI_HTYPE_ERROR   2
#define OCI_HTYPE_SVCCTX  3
#define OCI_HTYPE_STMT    4
#define OCI_DTYPE_LOB     7
#define OCI_NTV_SYNTAX    1
#define OCI_ONE_PIECE     1
#define OCI_FETCH_NEXT    2
#define OCI_ATTR_PREFETCH_ROWS 11
#define OCI_TEMP_CLOB     1
#define SQLCS_IMPLICIT    1
#ifndef OCI_DURATION_SESSION
#define OCI_DURATION_SESSION 10
#endif
#define OCI_ATTR_ROWS_FETCHED  197

// ---- Status codes --------------------------------------------------------
constexpr sword OCI_SUCCESS = 0;
constexpr sword OCI_ERROR   = -1;
constexpr sword OCI_NO_DATA = 100;

// ---- Indicator variable values ---------------------------------------------
constexpr sb2 OCI_IND_NOTNULL = 0;
constexpr sb2 OCI_IND_NULL    = -1;

// ---- External type constants ----------------------------------------------
constexpr ub2 SQLT_INT     = 3;
constexpr ub2 SQLT_UIN     = 68;
constexpr ub2 SQLT_CHR     = 1;   // VARCHAR2: explicit length, no null terminator
constexpr ub2 SQLT_AFC     = 96;  // CHAR: blank-padded to the column width
constexpr ub2 SQLT_STR     = 5;
constexpr ub2 SQLT_BFLOAT  = 21;
constexpr ub2 SQLT_BDOUBLE = 22;
constexpr ub2 SQLT_CLOB    = 112;

namespace binding::mock {

// Lets a demo/test declare what the next execute() should simulate, instead
// of the opaque "fail every Nth call" trick the first draft of this used.
enum class FailureMode { None, DisconnectThenRecover, ExecErrorAlways };

inline std::atomic<FailureMode> g_mode{FailureMode::None};
inline std::atomic<int> g_disconnects_remaining{0};
inline std::atomic<int> g_execute_calls{0};
inline std::atomic<int> g_last_iters{0}; // the `iters` OCIStmtExecute was last called with

inline void set_mode(FailureMode mode, int disconnect_count = 1) {
    g_mode = mode;
    g_disconnects_remaining = disconnect_count;
}

// pvskip/indskip: byte stride from one row's value/indicator to the next
// row's, for an array-of-struct fetch (see OCIDefineArrayOfStruct below) --
// 0 until that call sets them, which detail::define_one_field_array
// (oci_client.h) always does immediately after OCIDefineByPos.
struct MockDefine {
    void* ptr = nullptr; sb4 size = 0; ub2 dty = 0; sb2* indp = nullptr;
    ub4 pvskip = 0; ub4 indskip = 0;
    // rlenp/rlskip: where OCI reports each row's actual fetched length.
    // Needed for a SQLT_CHR column, whose value is not null-terminated --
    // FixedString::length_ref() is what gets passed here.
    ub2* rlenp = nullptr; ub4 rlskip = 0;
};
inline std::vector<MockDefine> g_defines;
inline int g_fetch_row = 0;
inline constexpr int MOCK_ROW_COUNT = 3;
inline std::atomic<int> g_last_rows_fetched{0}; // OCI_ATTR_ROWS_FETCHED after the last OCIStmtFetch2

// Lets a demo inspect what indicator value the last execute()'s bind calls
// set for each bind position -- 0 = OCI_IND_NOTNULL, -1 = OCI_IND_NULL.
inline std::vector<sb2> g_last_bind_indicators;

// Opt-in: when enabled, OCIStmtFetch2 (below) simulates a NULL on the *last*
// defined column of every other fetched row, so query()'s NULL handling has
// something real to exercise. Off by default -- a query() row struct with no
// nullable field has nowhere to put a simulated NULL (the column would just
// silently keep the previous row's stale value), so only turn this on for a
// query whose row type actually has an std::optional field in that position.
inline std::atomic<bool> g_simulate_null_last_column{false};
inline void set_simulate_null_last_column(bool enabled) { g_simulate_null_last_column = enabled; }

} // namespace binding::mock

// ---- Mock entry points -----------------------------------------------------
extern "C" {

inline sword OCIEnvCreate(OCIEnv** envhpp, ub4, dvoid*, dvoid*, dvoid*, dvoid*, size_t, dvoid**) {
    *envhpp = reinterpret_cast<OCIEnv*>(1); // non-null sentinel; never dereferenced
    return OCI_SUCCESS;
}

inline sword OCIHandleAlloc(const dvoid*, dvoid** hndlpp, ub4, size_t, dvoid**) {
    *hndlpp = reinterpret_cast<dvoid*>(1);
    return OCI_SUCCESS;
}

inline sword OCIHandleFree(dvoid*, ub4) { return OCI_SUCCESS; }

inline sword OCILogon2(OCIEnv*, OCIError*, OCISvcCtx** svchp,
                        const text*, ub4, const text*, ub4, const text*, ub4, ub4) {
    *svchp = reinterpret_cast<OCISvcCtx*>(1); // non-null sentinel; never dereferenced
    return OCI_SUCCESS;
}
inline sword OCILogoff(OCISvcCtx*, OCIError*) { return OCI_SUCCESS; }

inline sword OCIStmtPrepare(OCIStmt*, OCIError*, const text*, ub4, ub4, ub4) {
    binding::mock::g_defines.clear();
    binding::mock::g_fetch_row = 0;
    binding::mock::g_last_bind_indicators.clear();
    return OCI_SUCCESS;
}

inline sword OCIBindByName(OCIStmt*, OCIBind**, OCIError*, const text*, sb4,
                            dvoid*, sb4, ub2, dvoid* indp, ub2*, ub2*, ub4, ub4*, ub4) {
    // Tracked in call order (bind_fields() in oci_client.h always binds a
    // struct's fields in declaration order), same convention OCIBindByPos
    // uses positionally -- g_last_bind_indicators[i] is the i-th bound
    // field's indicator either way.
    binding::mock::g_last_bind_indicators.push_back(indp ? *static_cast<sb2*>(indp) : OCI_IND_NOTNULL);
    return OCI_SUCCESS;
}

inline sword OCIBindArrayOfStruct(OCIBind*, OCIError*, ub4, ub4, ub4, ub4) {
    // Skip-parameter plumbing for a real array bind's memory layout -- the
    // mock never actually reads through the bound pointers at multiple
    // strides (see OCIBindByName above, which only inspects the single
    // indicator passed for the *first* row), so there's nothing to record.
    return OCI_SUCCESS;
}

inline sword OCIBindByPos(OCIStmt*, OCIBind**, OCIError*, ub4 position,
                           dvoid*, sb4, ub2, dvoid* indp, ub2*, ub2*, ub4, ub4*, ub4) {
    auto& inds = binding::mock::g_last_bind_indicators;
    if (inds.size() < position) inds.resize(position, OCI_IND_NOTNULL);
    inds[position - 1] = indp ? *static_cast<sb2*>(indp) : OCI_IND_NOTNULL;
    return OCI_SUCCESS;
}

inline sword OCIDefineByPos(OCIStmt*, OCIDefine** defnpp, OCIError*, ub4 position,
                             dvoid* valuep, sb4 value_sz, ub2 dty,
                             dvoid* indp, ub2* rlenp, ub2*, ub4) {
    auto& defines = binding::mock::g_defines;
    if (defines.size() < position) defines.resize(position);
    defines[position - 1] = { valuep, value_sz, dty, static_cast<sb2*>(indp), 0, 0, rlenp, 0 };
    // Encode the column position directly as the "handle" value -- never
    // dereferenced, just decoded back by OCIDefineArrayOfStruct below so it
    // knows which g_defines entry to update. A real OCIDefine* is opaque to
    // callers too; nothing here relies on it pointing at real memory.
    if (defnpp) *defnpp = reinterpret_cast<OCIDefine*>(static_cast<std::uintptr_t>(position));
    return OCI_SUCCESS;
}

inline sword OCIDefineArrayOfStruct(OCIDefine* defnp, OCIError*, ub4 pvskip, ub4 indskip, ub4 rlskip, ub4) {
    auto& defines = binding::mock::g_defines;
    const auto position = static_cast<ub4>(reinterpret_cast<std::uintptr_t>(defnp));
    if (position >= 1 && position <= defines.size()) {
        defines[position - 1].pvskip = pvskip;
        defines[position - 1].indskip = indskip;
        defines[position - 1].rlskip = rlskip;
    }
    return OCI_SUCCESS;
}

inline sword OCIAttrSet(dvoid*, ub4, dvoid*, ub4, ub4, OCIError*) {
    // Only OCI_ATTR_PREFETCH_ROWS is ever set in this codebase, and the
    // mock's OCIStmtFetch2 already fetches as many rows as it's asked for
    // per call -- there's no separate client-side prefetch cache here to
    // configure.
    return OCI_SUCCESS;
}

inline sword OCIAttrGet(const dvoid*, ub4, dvoid* attributep, ub4* sizep, ub4 attrtype, OCIError*) {
    if (attrtype == OCI_ATTR_ROWS_FETCHED && attributep) {
        *static_cast<ub4*>(attributep) = static_cast<ub4>(binding::mock::g_last_rows_fetched.load());
        if (sizep) *sizep = sizeof(ub4);
    }
    return OCI_SUCCESS;
}

inline sword OCIStmtExecute(OCISvcCtx*, OCIStmt*, OCIError*, ub4 iters, ub4, const dvoid*, dvoid*, ub4) {
    using namespace binding::mock;
    g_execute_calls.fetch_add(1);
    g_last_iters.store(static_cast<int>(iters));
    if (g_mode.load() == FailureMode::ExecErrorAlways) {
        return OCI_ERROR; // e.g. ORA-00001 unique constraint violated -- not retryable
    }
    if (g_mode.load() == FailureMode::DisconnectThenRecover && g_disconnects_remaining.load() > 0) {
        --g_disconnects_remaining;
        return OCI_ERROR; // e.g. ORA-03113 end-of-file on communication channel
    }
    return OCI_SUCCESS;
}

inline sword OCIStmtFetch2(OCIStmt*, OCIError*, ub4 nrows, ub2, sb4, ub4) {
    using namespace binding::mock;

    // Real Oracle behavior (confirmed against a live database): the call
    // that returns the last, possibly-partial batch reports OCI_NO_DATA
    // directly -- not OCI_SUCCESS on a final full/partial batch followed
    // by a separate all-zero OCI_NO_DATA call. OCI_ATTR_ROWS_FETCHED (via
    // OCIAttrGet) still holds however many rows *this* call actually
    // wrote, valid either way.
    ub4 fetched = 0;
    for (; fetched < nrows && g_fetch_row < MOCK_ROW_COUNT; ++fetched, ++g_fetch_row) {
        // Demo behavior (opt-in, see g_simulate_null_last_column): the last
        // defined column comes back NULL on every other row, so code
        // driving select() has a real NULL to exercise.
        const std::size_t null_column = g_defines.empty() ? 0 : g_defines.size() - 1;
        const bool simulate_null_this_row = g_simulate_null_last_column.load() && (g_fetch_row % 2 == 1);

        for (std::size_t i = 0; i < g_defines.size(); ++i) {
            const auto& d = g_defines[i];
            if (!d.ptr) continue;

            auto* row_ptr = static_cast<unsigned char*>(d.ptr) + static_cast<std::size_t>(fetched) * d.pvskip;
            sb2* ind_ptr = d.indp ? reinterpret_cast<sb2*>(
                reinterpret_cast<unsigned char*>(d.indp) + static_cast<std::size_t>(fetched) * d.indskip) : nullptr;

            if (simulate_null_this_row && i == null_column) {
                if (ind_ptr) *ind_ptr = OCI_IND_NULL;
                continue; // OCI leaves the output buffer alone for a NULL column
            }
            if (ind_ptr) *ind_ptr = OCI_IND_NOTNULL;

            if (d.dty == SQLT_INT || d.dty == SQLT_UIN) {
                // Any integer width, not just sizeof(int): OCI takes the width
                // from the define's value_sz, so a std::int64_t or
                // std::uint32_t column is as ordinary as an int one. Writing
                // only 4-byte values here left a wider column reading back as
                // zero, which looked like a binder bug rather than a mock gap.
                const long long v = 100 + g_fetch_row * 10 + static_cast<int>(i);
                switch (d.size) {
                    case 2: { auto n = static_cast<short>(v);     std::memcpy(row_ptr, &n, sizeof(n)); break; }
                    case 4: { auto n = static_cast<int>(v);       std::memcpy(row_ptr, &n, sizeof(n)); break; }
                    case 8: { auto n = static_cast<long long>(v); std::memcpy(row_ptr, &n, sizeof(n)); break; }
                    default: break;
                }
            } else if (d.dty == SQLT_BDOUBLE && d.size == sizeof(double)) {
                double v = 1.5 * (g_fetch_row + 1) + static_cast<double>(i);
                std::memcpy(row_ptr, &v, sizeof(v));
            } else if ((d.dty == SQLT_CHR || d.dty == SQLT_AFC) && d.size > 0) {
                // A real VARCHAR2 fetch writes unterminated bytes into the
                // buffer and reports the length separately through rlenp --
                // mirrored here so FixedString's length_ref() round-trips
                // the same way it does against a real database.
                const std::string v = "row" + std::to_string(g_fetch_row) + "_col" + std::to_string(i);
                const auto n = std::min<std::size_t>(v.size(), static_cast<std::size_t>(d.size));
                std::memcpy(row_ptr, v.data(), n);
                if (d.rlenp) {
                    auto* rlen_ptr = reinterpret_cast<ub2*>(
                        reinterpret_cast<unsigned char*>(d.rlenp) + static_cast<std::size_t>(fetched) * d.rlskip);
                    *rlen_ptr = static_cast<ub2>(n);
                }
            }
        }
    }
    g_last_rows_fetched.store(static_cast<int>(fetched));
    return (g_fetch_row >= MOCK_ROW_COUNT) ? OCI_NO_DATA : OCI_SUCCESS;
}

inline sword OCIErrorGet(dvoid*, ub4, text*, sb4* errcodep, text* bufp, ub4 bufsiz, ub4) {
    using namespace binding::mock;
    sb4 code = 0;
    std::string_view msg = "ORA-00000: normal, successful completion";
    switch (g_mode.load()) {
        case FailureMode::DisconnectThenRecover:
            code = 3113;
            msg = "ORA-03113: end-of-file on communication channel";
            break;
        case FailureMode::ExecErrorAlways:
            code = 1;
            msg = "ORA-00001: unique constraint violated";
            break;
        default:
            break;
    }
    if (errcodep) *errcodep = code;
    if (bufp && bufsiz > 0) {
        const std::size_t n = std::min<std::size_t>(bufsiz - 1, msg.size());
        std::memcpy(bufp, msg.data(), n);
        bufp[n] = 0;
    }
    return OCI_SUCCESS;
}

inline sword OCIDescriptorAlloc(const dvoid*, dvoid** descpp, ub4, size_t, dvoid**) {
    *descpp = reinterpret_cast<dvoid*>(1);
    return OCI_SUCCESS;
}
inline sword OCIDescriptorFree(dvoid*, ub4) { return OCI_SUCCESS; }

// A descriptor straight out of OCIDescriptorAlloc is not yet a usable LOB --
// it has no underlying storage until it is either fetched from the database
// or turned into a temporary LOB. oci_client.h binds temporary LOBs, so the
// mock needs both calls.
inline sword OCILobCreateTemporary(OCISvcCtx*, OCIError*, OCILobLocator*, ub2, ub1, ub1, int, ub2) {
    return OCI_SUCCESS;
}

inline sword OCILobFreeTemporary(OCISvcCtx*, OCIError*, OCILobLocator*) { return OCI_SUCCESS; }

inline sword OCILobWrite2(OCISvcCtx*, OCIError*, OCILobLocator*, oraub8*, oraub8*, ub4,
                           dvoid*, oraub8, ub1, dvoid*, dvoid*, ub2, ub1) {
    return OCI_SUCCESS;
}
inline sword OCILobGetLength2(OCISvcCtx*, OCIError*, OCILobLocator*, oraub8* lenp) {
    if (lenp) *lenp = 0;
    return OCI_SUCCESS;
}
inline sword OCILobRead2(OCISvcCtx*, OCIError*, OCILobLocator*, oraub8*, oraub8*, ub4,
                          dvoid*, oraub8, ub1, dvoid*, dvoid*, ub2, ub1) {
    return OCI_SUCCESS;
}

} // extern "C"
