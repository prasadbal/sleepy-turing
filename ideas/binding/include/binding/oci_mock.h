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
struct OCIServer;
struct OCISession;
struct OCIStmt;
struct OCIBind;
struct OCIDefine;
struct OCILobLocator;

// ---- Handle/attribute/mode constants -----------------------------------
// Values are internal to this mock -- they only need to be self-consistent,
// since the mock and the real <oci.h> are never compiled together.
#define OCI_DEFAULT       0
#define OCI_HTYPE_ENV     1
#define OCI_HTYPE_ERROR   2
#define OCI_HTYPE_SVCCTX  3
#define OCI_HTYPE_STMT    4
#define OCI_HTYPE_SESSION 5
#define OCI_HTYPE_SERVER  6
#define OCI_DTYPE_LOB     7
#define OCI_ATTR_SERVER   8
#define OCI_ATTR_SESSION  9
#define OCI_ATTR_USERNAME 10
#define OCI_ATTR_PASSWORD 11
#define OCI_CRED_RDBMS    1
#define OCI_NTV_SYNTAX    1
#define OCI_ONE_PIECE     1
#define OCI_FETCH_NEXT    2

// ---- Status codes --------------------------------------------------------
constexpr sword OCI_SUCCESS = 0;
constexpr sword OCI_ERROR   = -1;
constexpr sword OCI_NO_DATA = 100;

// ---- Indicator variable values ---------------------------------------------
constexpr sb2 OCI_IND_NOTNULL = 0;
constexpr sb2 OCI_IND_NULL    = -1;

// ---- External type constants ----------------------------------------------
constexpr ub2 SQLT_INT     = 3;
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

inline void set_mode(FailureMode mode, int disconnect_count = 1) {
    g_mode = mode;
    g_disconnects_remaining = disconnect_count;
}

struct MockDefine { void* ptr = nullptr; sb4 size = 0; ub2 dty = 0; sb2* indp = nullptr; };
inline std::vector<MockDefine> g_defines;
inline int g_fetch_row = 0;
inline constexpr int MOCK_ROW_COUNT = 3;

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

inline sword OCIServerAttach(OCIServer*, OCIError*, const text*, sb4, ub4) { return OCI_SUCCESS; }
inline sword OCIServerDetach(OCIServer*, OCIError*, ub4) { return OCI_SUCCESS; }

inline sword OCIAttrSet(dvoid*, ub4, dvoid*, ub4, ub4, OCIError*) { return OCI_SUCCESS; }

inline sword OCISessionBegin(OCISvcCtx*, OCIError*, OCISession*, ub4, ub4) { return OCI_SUCCESS; }
inline sword OCISessionEnd(OCISvcCtx*, OCIError*, OCISession*, ub4) { return OCI_SUCCESS; }

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

inline sword OCIBindByPos(OCIStmt*, OCIBind**, OCIError*, ub4 position,
                           dvoid*, sb4, ub2, dvoid* indp, ub2*, ub2*, ub4, ub4*, ub4) {
    auto& inds = binding::mock::g_last_bind_indicators;
    if (inds.size() < position) inds.resize(position, OCI_IND_NOTNULL);
    inds[position - 1] = indp ? *static_cast<sb2*>(indp) : OCI_IND_NOTNULL;
    return OCI_SUCCESS;
}

inline sword OCIDefineByPos(OCIStmt*, OCIDefine**, OCIError*, ub4 position,
                             dvoid* valuep, sb4 value_sz, ub2 dty,
                             dvoid* indp, ub2*, ub2*, ub4) {
    auto& defines = binding::mock::g_defines;
    if (defines.size() < position) defines.resize(position);
    defines[position - 1] = { valuep, value_sz, dty, static_cast<sb2*>(indp) };
    return OCI_SUCCESS;
}

inline sword OCIStmtExecute(OCISvcCtx*, OCIStmt*, OCIError*, ub4, ub4, const dvoid*, dvoid*, ub4) {
    using namespace binding::mock;
    g_execute_calls.fetch_add(1);
    if (g_mode.load() == FailureMode::ExecErrorAlways) {
        return OCI_ERROR; // e.g. ORA-00001 unique constraint violated -- not retryable
    }
    if (g_mode.load() == FailureMode::DisconnectThenRecover && g_disconnects_remaining.load() > 0) {
        --g_disconnects_remaining;
        return OCI_ERROR; // e.g. ORA-03113 end-of-file on communication channel
    }
    return OCI_SUCCESS;
}

inline sword OCIStmtFetch2(OCIStmt*, OCIError*, ub4, ub2, sb4, ub4) {
    using namespace binding::mock;
    if (g_fetch_row >= MOCK_ROW_COUNT) return OCI_NO_DATA;

    // Demo behavior (opt-in, see g_simulate_null_last_column): the last
    // defined column comes back NULL on every other row, so code driving
    // query() has a real NULL to exercise.
    const std::size_t null_column = g_defines.empty() ? 0 : g_defines.size() - 1;
    const bool simulate_null_this_row = g_simulate_null_last_column.load() && (g_fetch_row % 2 == 1);

    for (std::size_t i = 0; i < g_defines.size(); ++i) {
        const auto& d = g_defines[i];
        if (!d.ptr) continue;

        if (simulate_null_this_row && i == null_column) {
            if (d.indp) *d.indp = OCI_IND_NULL;
            continue; // OCI leaves the output buffer alone for a NULL column
        }
        if (d.indp) *d.indp = OCI_IND_NOTNULL;

        if (d.dty == SQLT_INT && d.size == sizeof(int)) {
            int v = 100 + g_fetch_row * 10 + static_cast<int>(i);
            std::memcpy(d.ptr, &v, sizeof(v));
        } else if (d.dty == SQLT_BDOUBLE && d.size == sizeof(double)) {
            double v = 1.5 * (g_fetch_row + 1) + static_cast<double>(i);
            std::memcpy(d.ptr, &v, sizeof(v));
        }
    }
    ++g_fetch_row;
    return OCI_SUCCESS;
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
