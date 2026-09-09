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
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
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
struct OCIDateTime;

// Mirrors the real ::OCITime/::OCIDate struct shape exactly (field names
// included) so binding/oci_datetime.h's OciDate -- which touches these
// fields directly rather than through OCIDateGetDate/OCIDateSetDate (real
// macros, not functions -- see oci_datetime.h) -- compiles and runs
// identically against the mock and a real client.
struct OCITime { unsigned char OCITimeHH = 0, OCITimeMI = 0, OCITimeSS = 0; };
struct OCIDate { sb2 OCIDateYYYY = 0; unsigned char OCIDateMM = 0, OCIDateDD = 0; OCITime OCIDateTime; };

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
#define OCI_DTYPE_TIMESTAMP 68
#define OCI_NTV_SYNTAX    1
#define OCI_ONE_PIECE     1
#define OCI_FETCH_NEXT    2
#define OCI_ATTR_PREFETCH_ROWS 11
#define OCI_TEMP_CLOB     1
#define OCI_TEMP_BLOB     2
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
constexpr ub2 SQLT_BLOB    = 113;
constexpr ub2 SQLT_ODT       = 156;
constexpr ub2 SQLT_TIMESTAMP = 187;

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

// ----------------------------------------------------------------------------
// Real, addressable backing storage for mock descriptors that something
// actually reads back. OCI_DTYPE_TIMESTAMP needs this for OCIDateTimeGet
// Date/GetTime (OciTimestamp::from_text/to_text's OCI-format-model
// conversion); OCI_DTYPE_LOB needs it so OCILobWrite2/OCILobRead2/
// OCILobGetLength2 (oci_client.h's LOB bind/fetch path) can actually store
// and return bytes instead of being no-ops. Every other descriptor type
// still only needs to be a non-null, never-dereferenced sentinel (the `1`
// OCIDescriptorAlloc below falls back to).
// ----------------------------------------------------------------------------
struct MockDateTimeDescriptor {
    sb2 year = 1970;
    unsigned char month = 1, day = 1, hour = 0, minute = 0, second = 0;
    ub4 fsec = 0;
};

// Backing store for a mock LOB locator: OCILobWrite2 (bind side) and the
// SQLT_CLOB/SQLT_BLOB branch of OCIStmtFetch2 (fetch side) both just set
// `data` directly; OCILobRead2/OCILobGetLength2 read it back. No read
// cursor needed -- unlike a real polling LOB read, the mock always serves
// the whole thing in one OCILobRead2 call, matching how details/
// oci_client.h's read_lob_bytes always calls it (one OCI_ONE_PIECE read
// sized off OCILobGetLength2, never FIRST_PIECE/NEXT_PIECE polling).
struct MockLobDescriptor {
    std::string data;
};

// ----------------------------------------------------------------------------
// A deliberately small subset of Oracle's format-model elements -- just
// enough to exercise "an arbitrary caller-supplied format string" end to end
// without a real Oracle client. The real OCIDateFromText/OCIDateTimeFromText
// (and their ToText counterparts) implement Oracle's full format-model
// surface -- spelled month/day names, week/Julian-day elements, fill mode,
// quoted literals, and more -- via the client library's own interpreter.
// This mock does not attempt that: only the numeric elements and AM/PM a
// batch/reporting date string actually uses in practice (YYYY, YY, RR, MM,
// DD, HH24, HH12/HH, MI, SS, AM/PM), plus verbatim literal characters
// (separators like '-', '/', ':', ' ').
// ----------------------------------------------------------------------------
enum class DateFormatToken { Year4, Year2, RRYear, Month, MonthAbbrev, Day, Hour24, Hour12, Minute, Second, Meridiem, Literal };
struct FormatElement { DateFormatToken kind; char literal = 0; };

inline std::vector<FormatElement> tokenize_date_format(std::string_view fmt) {
    auto upper_at = [&](std::size_t pos) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(fmt[pos])));
    };
    auto matches = [&](std::size_t pos, std::string_view keyword) {
        if (pos + keyword.size() > fmt.size()) return false;
        for (std::size_t k = 0; k < keyword.size(); ++k) {
            if (upper_at(pos + k) != keyword[k]) return false;
        }
        return true;
    };

    std::vector<FormatElement> tokens;
    std::size_t i = 0;
    while (i < fmt.size()) {
        // Longest match first: "HH24"/"HH12" before "HH", "YYYY" before "YY".
        if (matches(i, "YYYY"))      { tokens.push_back({DateFormatToken::Year4});  i += 4; }
        else if (matches(i, "HH24")) { tokens.push_back({DateFormatToken::Hour24}); i += 4; }
        else if (matches(i, "HH12")) { tokens.push_back({DateFormatToken::Hour12}); i += 4; }
        else if (matches(i, "RR"))   { tokens.push_back({DateFormatToken::RRYear}); i += 2; }
        else if (matches(i, "YY"))   { tokens.push_back({DateFormatToken::Year2});  i += 2; }
        else if (matches(i, "MON"))  { tokens.push_back({DateFormatToken::MonthAbbrev}); i += 3; }
        else if (matches(i, "MM"))   { tokens.push_back({DateFormatToken::Month});  i += 2; }
        else if (matches(i, "DD"))   { tokens.push_back({DateFormatToken::Day});    i += 2; }
        else if (matches(i, "HH"))   { tokens.push_back({DateFormatToken::Hour12}); i += 2; }
        else if (matches(i, "MI"))   { tokens.push_back({DateFormatToken::Minute}); i += 2; }
        else if (matches(i, "SS"))   { tokens.push_back({DateFormatToken::Second}); i += 2; }
        else if (matches(i, "AM") || matches(i, "PM")) { tokens.push_back({DateFormatToken::Meridiem}); i += 2; }
        else { tokens.push_back({DateFormatToken::Literal, fmt[i]}); i += 1; }
    }
    return tokens;
}

// Oracle's RR century-rollover rule -- deliberately duplicated from
// oci_datetime.h's detail::oracle_rr_year rather than shared: this file is
// what oci_datetime.h's real client-facing OciDate/OciTimestamp types build
// on top of (via oci_compat.h's mock fallback), not the other way around, so
// it cannot include that header back without a cycle.
inline int mock_current_year() {
    const std::time_t t = std::time(nullptr);
    const std::tm* lt = std::localtime(&t);
    return lt ? (1900 + lt->tm_year) : 2000;
}
inline int mock_rr_year(int rr, int current_yr) {
    const int cur_century = current_yr / 100;
    const int cur_yy = current_yr % 100;
    const int century = (rr <= 49) ? (cur_yy <= 49 ? cur_century : cur_century + 1)
                                   : (cur_yy <= 49 ? cur_century - 1 : cur_century);
    return century * 100 + rr;
}

struct ParsedMockDateTime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    bool is_pm = false, has_meridiem = false;
};

// Parses `text` against `fmt`'s tokens, left to right -- every numeric
// element consumes a fixed width (2 digits, 4 for YYYY), every Literal
// element must match the input character exactly. Returns false (an OCI
// status code, not a C++ exception, is what the caller of this mock
// function actually gets) on any mismatch, short input, or leftover input.
inline bool parse_with_mock_format(std::string_view text, std::string_view fmt, ParsedMockDateTime& out) {
    const auto tokens = tokenize_date_format(fmt);
    std::size_t pos = 0;
    auto read_digits = [&](int width, int& value) {
        if (pos + static_cast<std::size_t>(width) > text.size()) return false;
        int v = 0;
        for (int k = 0; k < width; ++k) {
            const char c = text[pos + static_cast<std::size_t>(k)];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        value = v;
        pos += static_cast<std::size_t>(width);
        return true;
    };

    for (const auto& tok : tokens) {
        switch (tok.kind) {
            case DateFormatToken::Year4:  if (!read_digits(4, out.year)) return false; break;
            case DateFormatToken::Year2:  { int yy = 0; if (!read_digits(2, yy)) return false; out.year = 2000 + yy; break; }
            case DateFormatToken::RRYear: { int rr = 0; if (!read_digits(2, rr)) return false;
                                            out.year = mock_rr_year(rr, mock_current_year()); break; }
            case DateFormatToken::Month:  if (!read_digits(2, out.month)) return false; break;
            case DateFormatToken::MonthAbbrev: {
                // "MON" -- Oracle's real default DATE format uses this, not
                // MM, so from_text()/to_text() being unconfigured (i.e.
                // resolving to OciDate/OciTimestamp's seeded "DD-MON-RR"
                // default) needs this token to actually round-trip. Only
                // the 3-letter English abbreviation, matched
                // case-insensitively -- real Oracle also accepts full month
                // names (MONTH) and honors the format mask's own
                // capitalization when rendering (Mon/MON/mon); this mock
                // does neither, see the class comment on tokenize_date_format.
                if (pos + 3 > text.size()) return false;
                char upper[3];
                for (int k = 0; k < 3; ++k) {
                    upper[k] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[pos + static_cast<std::size_t>(k)])));
                }
                static constexpr std::array<std::string_view, 12> kNames = {
                    "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
                };
                const std::string_view found(upper, 3);
                int month_num = 0;
                for (std::size_t m = 0; m < kNames.size(); ++m) {
                    if (kNames[m] == found) { month_num = static_cast<int>(m) + 1; break; }
                }
                if (month_num == 0) return false;
                out.month = month_num;
                pos += 3;
                break;
            }
            case DateFormatToken::Day:    if (!read_digits(2, out.day)) return false; break;
            case DateFormatToken::Hour24: if (!read_digits(2, out.hour)) return false; break;
            case DateFormatToken::Hour12: if (!read_digits(2, out.hour)) return false; out.has_meridiem = true; break;
            case DateFormatToken::Minute: if (!read_digits(2, out.minute)) return false; break;
            case DateFormatToken::Second: if (!read_digits(2, out.second)) return false; break;
            case DateFormatToken::Meridiem: {
                if (pos + 2 > text.size()) return false;
                const char c0 = static_cast<char>(std::toupper(static_cast<unsigned char>(text[pos])));
                const char c1 = static_cast<char>(std::toupper(static_cast<unsigned char>(text[pos + 1])));
                if (c1 != 'M' || (c0 != 'A' && c0 != 'P')) return false;
                out.is_pm = (c0 == 'P');
                out.has_meridiem = true;
                pos += 2;
                break;
            }
            case DateFormatToken::Literal:
                if (pos >= text.size() || text[pos] != tok.literal) return false;
                pos += 1;
                break;
        }
    }
    if (out.has_meridiem) {
        // 12-hour value read into out.hour above (1-12) -> 24-hour.
        const int h12 = out.hour;
        out.hour = out.is_pm ? (h12 == 12 ? 12 : h12 + 12) : (h12 == 12 ? 0 : h12);
    }
    return pos == text.size();
}

inline std::string zero_pad_mock(int value, int width) {
    std::string s = std::to_string(value);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
    return s;
}

// The inverse of parse_with_mock_format: renders year/month/day/hour/
// minute/second against fmt's tokens.
inline std::string render_with_mock_format(std::string_view fmt, int year, int month, int day,
                                            int hour, int minute, int second) {
    std::string out;
    for (const auto& tok : tokenize_date_format(fmt)) {
        switch (tok.kind) {
            case DateFormatToken::Year4:  out += zero_pad_mock(year, 4); break;
            case DateFormatToken::Year2:
            case DateFormatToken::RRYear: out += zero_pad_mock(year % 100, 2); break;
            case DateFormatToken::Month:  out += zero_pad_mock(month, 2); break;
            case DateFormatToken::MonthAbbrev: {
                static constexpr std::array<const char*, 12> kNames = {
                    "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
                };
                out += (month >= 1 && month <= 12) ? kNames[month - 1] : "???";
                break;
            }
            case DateFormatToken::Day:    out += zero_pad_mock(day, 2); break;
            case DateFormatToken::Hour24: out += zero_pad_mock(hour, 2); break;
            case DateFormatToken::Hour12: { const int h = hour % 12; out += zero_pad_mock(h == 0 ? 12 : h, 2); break; }
            case DateFormatToken::Minute: out += zero_pad_mock(minute, 2); break;
            case DateFormatToken::Second: out += zero_pad_mock(second, 2); break;
            case DateFormatToken::Meridiem: out += (hour < 12 ? "AM" : "PM"); break;
            case DateFormatToken::Literal: out += tok.literal; break;
        }
    }
    return out;
}

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
            } else if ((d.dty == SQLT_CLOB || d.dty == SQLT_BLOB) && d.size == sizeof(OCILobLocator*)) {
                // row_ptr here points into the standalone vector<OCILobLocator*>
                // define_one_column allocated (see out_staging_slot_t's LOB
                // case) -- *not* into the row struct, unlike every other
                // branch in this loop. Each element is already a real
                // MockLobDescriptor* from OCIDescriptorAlloc; just set its
                // content directly, the same way OCILobWrite2 would if the
                // caller had bound this value instead of fetched it.
                auto* locator = *reinterpret_cast<OCILobLocator**>(row_ptr);
                auto* desc = reinterpret_cast<binding::mock::MockLobDescriptor*>(locator);
                desc->data = "lob_row" + std::to_string(g_fetch_row) + "_col" + std::to_string(i);
            } else if (d.dty == SQLT_ODT && d.size == sizeof(::OCIDate)) {
                // Writes the real 7-byte ::OCIDate layout directly -- OciDate
                // (oci_datetime.h) wraps that struct with no descriptor and
                // no indirection, so a plain memcpy here is the mock's exact
                // equivalent of what OCIDefineByPos would actually fill in.
                ::OCIDate v{};
                v.OCIDateYYYY = static_cast<sb2>(2020 + g_fetch_row);
                v.OCIDateMM = static_cast<unsigned char>(1 + (g_fetch_row % 12));
                v.OCIDateDD = static_cast<unsigned char>(1 + static_cast<int>(i));
                std::memcpy(row_ptr, &v, sizeof(v));
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

inline sword OCIDescriptorAlloc(const dvoid*, dvoid** descpp, ub4 type, size_t, dvoid**) {
    using namespace binding::mock;
    if (type == OCI_DTYPE_TIMESTAMP) {
        *descpp = reinterpret_cast<dvoid*>(new MockDateTimeDescriptor{});
    } else if (type == OCI_DTYPE_LOB) {
        *descpp = reinterpret_cast<dvoid*>(new MockLobDescriptor{});
    } else {
        *descpp = reinterpret_cast<dvoid*>(1); // anything else: never dereferenced
    }
    return OCI_SUCCESS;
}
inline sword OCIDescriptorFree(dvoid* descp, ub4 type) {
    if (type == OCI_DTYPE_TIMESTAMP) {
        delete reinterpret_cast<binding::mock::MockDateTimeDescriptor*>(descp);
    } else if (type == OCI_DTYPE_LOB) {
        delete reinterpret_cast<binding::mock::MockLobDescriptor*>(descp);
    }
    return OCI_SUCCESS;
}

inline sword OCIDateTimeConstruct(void*, OCIError*, OCIDateTime* datetime, sb2 year, unsigned char month,
                                   unsigned char day, unsigned char hour, unsigned char minute,
                                   unsigned char second, ub4 fsec, text*, size_t) {
    using namespace binding::mock;
    *reinterpret_cast<MockDateTimeDescriptor*>(datetime) =
        MockDateTimeDescriptor{year, month, day, hour, minute, second, fsec};
    return OCI_SUCCESS;
}

// Parses date_str against fmt via the mock's small format-model interpreter
// (see binding::mock::parse_with_mock_format above) and writes the result
// directly into *date -- the real OCIDateFromText does the equivalent using
// Oracle's own interpreter, needing only err (never a live session/round
// trip: this is a pure client-side text<->value conversion).
inline sword OCIDateFromText(OCIError*, const text* date_str, ub4 d_str_length,
                              const text* fmt, ub1 fmt_length,
                              const text*, ub4,
                              OCIDate* date) {
    using namespace binding::mock;
    ParsedMockDateTime parsed;
    const std::string_view text_sv(reinterpret_cast<const char*>(date_str), d_str_length);
    const std::string_view fmt_sv(reinterpret_cast<const char*>(fmt), fmt_length);
    if (!parse_with_mock_format(text_sv, fmt_sv, parsed)) return OCI_ERROR;
    date->OCIDateYYYY = static_cast<sb2>(parsed.year);
    date->OCIDateMM = static_cast<unsigned char>(parsed.month);
    date->OCIDateDD = static_cast<unsigned char>(parsed.day);
    date->OCIDateTime.OCITimeHH = static_cast<unsigned char>(parsed.hour);
    date->OCIDateTime.OCITimeMI = static_cast<unsigned char>(parsed.minute);
    date->OCIDateTime.OCITimeSS = static_cast<unsigned char>(parsed.second);
    return OCI_SUCCESS;
}

// The inverse of OCIDateFromText: renders *date against fmt into buf,
// failing (rather than truncating) if it doesn't fit in the caller's buffer
// -- *buf_size is the buffer's capacity on entry, the rendered length on a
// successful return, matching the real function's in/out convention.
inline sword OCIDateToText(OCIError*, const OCIDate* date,
                            const text* fmt, ub1 fmt_length,
                            const text*, ub4,
                            ub4* buf_size, text* buf) {
    using namespace binding::mock;
    const std::string_view fmt_sv(reinterpret_cast<const char*>(fmt), fmt_length);
    const std::string rendered = render_with_mock_format(fmt_sv,
        date->OCIDateYYYY, date->OCIDateMM, date->OCIDateDD,
        date->OCIDateTime.OCITimeHH, date->OCIDateTime.OCITimeMI, date->OCIDateTime.OCITimeSS);
    if (rendered.size() > *buf_size) return OCI_ERROR;
    std::memcpy(buf, rendered.data(), rendered.size());
    *buf_size = static_cast<ub4>(rendered.size());
    return OCI_SUCCESS;
}

// TIMESTAMP counterparts of OCIDateFromText/OCIDateToText above, operating
// on the OCIDateTime descriptor (see MockDateTimeDescriptor) instead of a
// flat ::OCIDate. `hndl` is an OCIEnv* in this codebase's usage (see
// OciTimestamp::from_text/to_text in oci_datetime.h) -- still never an
// OCISvcCtx*/live session, same reason as the OciDate pair above.
inline sword OCIDateTimeFromText(void*, OCIError*,
                                  const text* date_str, size_t dstr_length,
                                  const text* fmt, ub1 fmt_length,
                                  const text*, size_t,
                                  OCIDateTime* datetime) {
    using namespace binding::mock;
    ParsedMockDateTime parsed;
    const std::string_view text_sv(reinterpret_cast<const char*>(date_str), dstr_length);
    const std::string_view fmt_sv(reinterpret_cast<const char*>(fmt), fmt_length);
    if (!parse_with_mock_format(text_sv, fmt_sv, parsed)) return OCI_ERROR;
    *reinterpret_cast<MockDateTimeDescriptor*>(datetime) = MockDateTimeDescriptor{
        static_cast<sb2>(parsed.year), static_cast<unsigned char>(parsed.month),
        static_cast<unsigned char>(parsed.day), static_cast<unsigned char>(parsed.hour),
        static_cast<unsigned char>(parsed.minute), static_cast<unsigned char>(parsed.second), 0};
    return OCI_SUCCESS;
}

inline sword OCIDateTimeToText(void*, OCIError*, const OCIDateTime* datetime,
                                const text* fmt, ub1 fmt_length, ub1,
                                const text*, size_t,
                                ub4* buf_size, text* buf) {
    using namespace binding::mock;
    const auto* d = reinterpret_cast<const MockDateTimeDescriptor*>(datetime);
    const std::string_view fmt_sv(reinterpret_cast<const char*>(fmt), fmt_length);
    const std::string rendered = render_with_mock_format(fmt_sv, d->year, d->month, d->day, d->hour, d->minute, d->second);
    if (rendered.size() > *buf_size) return OCI_ERROR;
    std::memcpy(buf, rendered.data(), rendered.size());
    *buf_size = static_cast<ub4>(rendered.size());
    return OCI_SUCCESS;
}

// Reads the fields OCIDateTimeConstruct/OCIDateTimeFromText last wrote back
// out of the descriptor -- what lets OciTimestamp::from_text (oci_datetime.h)
// stay a plain value type (year/month/day/hour/minute/second fields, no live
// descriptor held between calls) even when it's built via the OCI text
// conversion instead of the hand-rolled default-format parser.
inline sword OCIDateTimeGetDate(void*, OCIError*, const OCIDateTime* datetime,
                                 sb2* year, unsigned char* month, unsigned char* day) {
    const auto* d = reinterpret_cast<const binding::mock::MockDateTimeDescriptor*>(datetime);
    *year = d->year; *month = d->month; *day = d->day;
    return OCI_SUCCESS;
}
inline sword OCIDateTimeGetTime(void*, OCIError*, OCIDateTime* datetime,
                                 unsigned char* hour, unsigned char* minute, unsigned char* second, ub4* fsec) {
    const auto* d = reinterpret_cast<const binding::mock::MockDateTimeDescriptor*>(datetime);
    *hour = d->hour; *minute = d->minute; *second = d->second;
    if (fsec) *fsec = d->fsec;
    return OCI_SUCCESS;
}

// A descriptor straight out of OCIDescriptorAlloc is not yet a usable LOB --
// it has no underlying storage until it is either fetched from the database
// or turned into a temporary LOB. oci_client.h binds temporary LOBs, so the
// mock needs both calls.
inline sword OCILobCreateTemporary(OCISvcCtx*, OCIError*, OCILobLocator*, ub2, ub1, ub1, int, ub2) {
    // No-op: the descriptor already has real backing storage (an empty
    // std::string) from OCIDescriptorAlloc -- nothing further to set up
    // before OCILobWrite2 below can just assign into it.
    return OCI_SUCCESS;
}

inline sword OCILobFreeTemporary(OCISvcCtx*, OCIError*, OCILobLocator*) { return OCI_SUCCESS; }

inline sword OCILobWrite2(OCISvcCtx*, OCIError*, OCILobLocator* locp, oraub8* byte_amtp, oraub8* char_amtp, ub4,
                           dvoid* bufp, oraub8 buflen, ub1, dvoid*, dvoid*, ub2, ub1) {
    auto* desc = reinterpret_cast<binding::mock::MockLobDescriptor*>(locp);
    desc->data.assign(static_cast<const char*>(bufp), static_cast<std::size_t>(buflen));
    if (byte_amtp) *byte_amtp = buflen;
    if (char_amtp) *char_amtp = buflen;
    return OCI_SUCCESS;
}
inline sword OCILobGetLength2(OCISvcCtx*, OCIError*, OCILobLocator* locp, oraub8* lenp) {
    auto* desc = reinterpret_cast<binding::mock::MockLobDescriptor*>(locp);
    if (lenp) *lenp = static_cast<oraub8>(desc->data.size());
    return OCI_SUCCESS;
}
inline sword OCILobRead2(OCISvcCtx*, OCIError*, OCILobLocator* locp, oraub8* byte_amtp, oraub8* char_amtp, ub4,
                          dvoid* bufp, oraub8 bufl, ub1, dvoid*, dvoid*, ub2, ub1) {
    auto* desc = reinterpret_cast<binding::mock::MockLobDescriptor*>(locp);
    const std::size_t to_copy = std::min(desc->data.size(), static_cast<std::size_t>(bufl));
    std::memcpy(bufp, desc->data.data(), to_copy);
    if (byte_amtp) *byte_amtp = to_copy;
    if (char_amtp) *char_amtp = to_copy;
    return OCI_SUCCESS;
}

} // extern "C"
