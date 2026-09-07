#pragma once
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "binding/oci_compat.h"
#include "binding/oci_connection.h"

namespace binding {
namespace detail {

// Zero-pads a non-negative integer to at least `width` digits -- used only
// to render OciDate/OciTimestamp back to Oracle's default text format
// (their to_string() below), never for parsing.
inline std::string zero_pad(int value, int width) {
    std::string s = std::to_string(value);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
    return s;
}

inline const char* oracle_month_name(unsigned char month) {
    static constexpr std::array<const char*, 12> kNames = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
    };
    if (month < 1 || month > 12) {
        throw std::runtime_error("binding: month " + std::to_string(static_cast<int>(month)) +
                                 " is out of range 1-12");
    }
    return kNames[month - 1];
}

// The "MON" element of Oracle's default date format -- a 3-letter English
// month abbreviation, matched case-insensitively (Oracle itself accepts
// any case: "Sep", "SEP", "sep" all parse the same). Returns 0 if `text`
// isn't one of the twelve.
inline unsigned char oracle_month_from_abbrev(std::string_view text) {
    if (text.size() != 3) return 0;
    char upper[3];
    for (int i = 0; i < 3; ++i) {
        upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    }
    static constexpr std::array<std::string_view, 12> kNames = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
    };
    const std::string_view u(upper, 3);
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames[i] == u) return static_cast<unsigned char>(i + 1);
    }
    return 0;
}

// The current calendar year, per the system clock -- what Oracle's RR
// format element resolves a two-digit year against (see oracle_rr_year
// below). A real Oracle server resolves RR against its own SYSDATE, so a
// client-side parse of an RR-formatted string only matches what Oracle
// itself would produce when the two clocks agree on the year, same as any
// client-side interpretation of RR would.
inline int current_year() {
    using namespace std::chrono;
    const auto today = year_month_day{floor<days>(system_clock::now())};
    return static_cast<int>(today.year());
}

// Oracle's RR century-rollover rule (the RR entry in Oracle's Format
// Models reference): a two-digit year resolves to the current century,
// unless that would put it more than 50 years from the current year, in
// which case it rolls to the adjacent century instead. This is what lets
// "RR" (unlike plain "YY") read "49" as 2049 and "50" as 1950 when the
// current year is 2026, rather than always assuming the current century.
inline int oracle_rr_year(int rr, int current_yr) {
    const int cur_century = current_yr / 100;
    const int cur_yy = current_yr % 100;
    int century;
    if (rr <= 49) {
        century = (cur_yy <= 49) ? cur_century : cur_century + 1;
    } else {
        century = (cur_yy <= 49) ? cur_century - 1 : cur_century;
    }
    return century * 100 + rr;
}

// Parses one unsigned decimal field, throwing (naming `whole_text`, the
// full string being parsed, not just this field) if `text` isn't exactly
// `digits` characters of 0-9 in range [lo, hi].
inline int parse_field(std::string_view text, int digits, int lo, int hi, const char* field,
                       std::string_view whole_text) {
    if (static_cast<int>(text.size()) != digits) {
        throw std::runtime_error("binding: '" + std::string(whole_text) +
                                 "' is not a valid Oracle date/time -- " + field + " must be " +
                                 std::to_string(digits) + " digits");
    }
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < lo || value > hi) {
        throw std::runtime_error("binding: '" + std::string(whole_text) +
                                 "' is not a valid Oracle date/time -- " + field + " '" +
                                 std::string(text) + "' is not in range " + std::to_string(lo) +
                                 "-" + std::to_string(hi));
    }
    return value;
}

struct ParsedOracleDate {
    short year;
    unsigned char month;
    unsigned char day;
};

// Parses Oracle's default DATE format, "DD-MON-RR" (e.g. "07-SEP-26") --
// what TO_DATE(text) resolves against with no explicit format mask, under
// the out-of-the-box NLS_DATE_FORMAT. `whole_text` is what error messages
// name; it defaults to `date_text` itself, but a caller parsing a larger
// string (OciTimestamp's constructor below, parsing the date portion of
// "DD-MON-RR HH.MI.SS AM") passes the whole thing through, so a malformed
// date reports the string the caller actually had, not just the substring
// this function happened to be given.
inline ParsedOracleDate parse_oracle_date(std::string_view date_text, std::string_view whole_text) {
    const auto dash1 = date_text.find('-');
    const auto dash2 =
        (dash1 == std::string_view::npos) ? std::string_view::npos : date_text.find('-', dash1 + 1);
    if (dash1 == std::string_view::npos || dash2 == std::string_view::npos) {
        throw std::runtime_error("binding: '" + std::string(whole_text) +
                                 "' is not a valid Oracle date (expected DD-MON-RR)");
    }

    const std::string_view day_text = date_text.substr(0, dash1);
    const std::string_view mon_text = date_text.substr(dash1 + 1, dash2 - dash1 - 1);
    const std::string_view rr_text = date_text.substr(dash2 + 1);

    const int day = parse_field(day_text, 2, 1, 31, "day", whole_text);
    const unsigned char month = oracle_month_from_abbrev(mon_text);
    if (month == 0) {
        throw std::runtime_error("binding: '" + std::string(whole_text) +
                                 "' is not a valid Oracle date -- '" + std::string(mon_text) +
                                 "' is not a month abbreviation (expected JAN..DEC)");
    }
    const int rr = parse_field(rr_text, 2, 0, 99, "year", whole_text);

    return ParsedOracleDate{static_cast<short>(oracle_rr_year(rr, current_year())), month,
                            static_cast<unsigned char>(day)};
}

inline ParsedOracleDate parse_oracle_date(std::string_view date_text) {
    return parse_oracle_date(date_text, date_text);
}

struct ParsedOracleTime {
    unsigned char hour; // 0-23
    unsigned char minute;
    unsigned char second;
};

// Parses the time-of-day portion of Oracle's default TIMESTAMP format,
// "HH.MI.SS AM" / "HH.MI.SS PM" -- a 12-hour clock (HH is 1-12), which is
// why the AM/PM indicator is mandatory here even though it's absent from
// parse_oracle_date's plain DATE format above. `whole_text` is what error
// messages name, same reason as parse_oracle_date's own parameter.
inline ParsedOracleTime parse_oracle_time(std::string_view time_text, std::string_view whole_text) {
    const auto space = time_text.find(' ');
    if (space == std::string_view::npos) {
        throw std::runtime_error("binding: '" + std::string(whole_text) +
                                 "' is not a valid Oracle timestamp -- missing AM/PM "
                                 "(expected HH.MI.SS AM)");
    }
    const std::string_view clock = time_text.substr(0, space);
    const std::string_view meridiem = time_text.substr(space + 1);

    const auto dot1 = clock.find('.');
    const auto dot2 = (dot1 == std::string_view::npos) ? std::string_view::npos : clock.find('.', dot1 + 1);
    if (dot1 == std::string_view::npos || dot2 == std::string_view::npos) {
        throw std::runtime_error("binding: '" + std::string(whole_text) +
                                 "' is not a valid Oracle timestamp (expected HH.MI.SS AM)");
    }

    const int hour12 = parse_field(clock.substr(0, dot1), 2, 1, 12, "hour", whole_text);
    const int minute =
        parse_field(clock.substr(dot1 + 1, dot2 - dot1 - 1), 2, 0, 59, "minute", whole_text);
    const int second = parse_field(clock.substr(dot2 + 1), 2, 0, 59, "second", whole_text);

    std::string upper_meridiem(meridiem);
    for (char& c : upper_meridiem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    unsigned char hour24;
    if (upper_meridiem == "AM") {
        hour24 = static_cast<unsigned char>(hour12 == 12 ? 0 : hour12);
    } else if (upper_meridiem == "PM") {
        hour24 = static_cast<unsigned char>(hour12 == 12 ? 12 : hour12 + 12);
    } else {
        throw std::runtime_error("binding: '" + std::string(whole_text) +
                                 "' is not a valid Oracle timestamp -- '" + std::string(meridiem) +
                                 "' is not AM or PM");
    }

    return ParsedOracleTime{hour24, static_cast<unsigned char>(minute), static_cast<unsigned char>(second)};
}

// Renders an hour in [0,23] back to Oracle's 12-hour default display --
// the inverse of parse_oracle_time's AM/PM handling above.
inline void to_oracle_12h(unsigned char hour24, int& hour12, const char*& meridiem) {
    meridiem = (hour24 < 12) ? "AM" : "PM";
    const int h = hour24 % 12;
    hour12 = (h == 0) ? 12 : h;
}

} // namespace detail

// Oracle DATE column -- year/month/day/hour/minute/second (Oracle's DATE
// type always carries a time-of-day, even when only the calendar date
// matters, e.g. a COB/close-of-business date, which conventionally means
// midnight). Binds/defines as SQLT_ODT, directly through the real
// ::OCIDate struct's own documented fields (OCIDateYYYY/MM/DD,
// OCIDateTime.OCITimeHH/MI/SS). Oracle's OCIDateGetDate/OCIDateSetDate/
// OCIDateGetTime/OCIDateSetTime are actually macros over these same
// fields (see orl.h), not linkable functions, so touching the fields
// directly here is equivalent to calling them and avoids needing to
// replicate those macros in the mock.
//
// Fixed-size (7 bytes on a real client), no descriptor/locator, no
// allocation -- unlike OciTimestamp below (and OciClob/OciXml,
// binding/oci_lob.h), an OciDate field's bytes sit inline in its row
// struct at a fixed offset, so it needs no special-casing in
// bind_one_field/define_one_field_array at all: it flows through the
// same plain-field path as an arithmetic field, and works with the real
// array bind/fetch (OCIBindArrayOfStruct/OCIDefineArrayOfStruct)
// insert()/select() already use.
class OciDate {
public:
    OciDate() = default;

    // hour/minute/second default to midnight -- the common case for a
    // COB/business date, which has no meaningful time-of-day component.
    OciDate(short year, unsigned char month, unsigned char day,
            unsigned char hour = 0, unsigned char minute = 0, unsigned char second = 0) {
        raw_.OCIDateYYYY = year;
        raw_.OCIDateMM = month;
        raw_.OCIDateDD = day;
        raw_.OCIDateTime.OCITimeHH = hour;
        raw_.OCIDateTime.OCITimeMI = minute;
        raw_.OCIDateTime.OCITimeSS = second;
    }

    // Parses Oracle's default DATE format, "DD-MON-RR" (e.g. "07-SEP-26")
    // -- the format TO_DATE(text) resolves against with no explicit mask,
    // under the out-of-the-box NLS_DATE_FORMAT. Time-of-day defaults to
    // midnight, same as (and for the same reason as) the numeric
    // constructor above: the default DATE format carries no time
    // component. Throws std::runtime_error, naming `text`, if it doesn't
    // match that shape.
    explicit OciDate(std::string_view text) : OciDate(from_text(text)) {}

    short year() const noexcept { return raw_.OCIDateYYYY; }
    unsigned char month() const noexcept { return raw_.OCIDateMM; }
    unsigned char day() const noexcept { return raw_.OCIDateDD; }
    unsigned char hour() const noexcept { return raw_.OCIDateTime.OCITimeHH; }
    unsigned char minute() const noexcept { return raw_.OCIDateTime.OCITimeMI; }
    unsigned char second() const noexcept { return raw_.OCIDateTime.OCITimeSS; }

    // The inverse of the string constructor above: Oracle's default DATE
    // format, "DD-MON-RR" -- what TO_CHAR(date_col) produces with no
    // explicit mask. Time-of-day is not part of this format and is not
    // rendered, even if this OciDate carries one (the same asymmetry the
    // numeric constructor already has: the default format only ever
    // writes and reads midnight).
    std::string to_string() const {
        return detail::zero_pad(day(), 2) + "-" + detail::oracle_month_name(month()) + "-" +
               detail::zero_pad(year() % 100, 2);
    }

    // Sets the format used by the no-explicit-format overloads of
    // from_text()/to_text() below -- e.g. a site that always represents
    // dates as "YYYYMMDD" calls this once at startup instead of repeating
    // the format string at every call site. Independent of OciTimestamp's
    // own default_format() below; the two are set separately. Not
    // thread-safe against a concurrent writer -- set it once during startup,
    // before any OciDate parsing/rendering runs on another thread.
    static void set_default_format(std::string_view format) { default_format_storage() = std::string(format); }
    static const std::string& default_format() { return default_format_storage(); }

    // Parses `text` against an explicit Oracle format model (e.g.
    // "YYYYMMDD", "DD/MM/YYYY") via OCIDateFromText -- Oracle's own
    // client-side format-model interpreter, unlike the hand-rolled
    // default-format ("DD-MON-RR") parser the single-argument constructor
    // above uses. `format` empty (the default) uses whatever
    // set_default_format() last configured; throws if neither was given.
    //
    // Needs conn.err() to be valid, so `conn` must already be connect()ed --
    // but no live session/round-trip happens here. OCIDateFromText takes
    // only an OCIError*, never an OCISvcCtx*, and never talks to the server:
    // the format-model interpreter runs in the OCI client library, in
    // process, the same way TO_DATE's parsing logic does, just evaluated
    // client-side instead of on the server.
    static OciDate from_text(OciConnection& conn, std::string_view value,
                              std::string_view format = {}, std::string_view language = {}) {
        const std::string_view fmt = resolve_format(format);
        OciDate result;
        const sword status = OCIDateFromText(conn.err(),
            reinterpret_cast<const ::text*>(value.data()), static_cast<ub4>(value.size()),
            reinterpret_cast<const ::text*>(fmt.data()), static_cast<ub1>(fmt.size()),
            language.empty() ? nullptr : reinterpret_cast<const ::text*>(language.data()),
            static_cast<ub4>(language.size()), &result.raw_);
        if (status != OCI_SUCCESS) {
            throw std::runtime_error("binding: OCIDateFromText failed parsing '" + std::string(value) +
                                     "' against format '" + std::string(fmt) + "'");
        }
        return result;
    }

    // The inverse of from_text() above: renders this value using an
    // explicit Oracle format model via OCIDateToText, rather than
    // to_string()'s fixed "DD-MON-RR" rendering. Same default-format and
    // connection-but-not-session rules as from_text().
    std::string to_text(OciConnection& conn, std::string_view format = {}, std::string_view language = {}) const {
        const std::string_view fmt = resolve_format(format);
        std::array<unsigned char, 128> buf{};
        ub4 buf_size = static_cast<ub4>(buf.size());
        const sword status = OCIDateToText(conn.err(), &raw_,
            reinterpret_cast<const ::text*>(fmt.data()), static_cast<ub1>(fmt.size()),
            language.empty() ? nullptr : reinterpret_cast<const ::text*>(language.data()),
            static_cast<ub4>(language.size()), &buf_size, buf.data());
        if (status != OCI_SUCCESS) {
            throw std::runtime_error("binding: OCIDateToText failed rendering against format '" +
                                     std::string(fmt) + "'");
        }
        return std::string(reinterpret_cast<const char*>(buf.data()), buf_size);
    }

    friend bool operator==(const OciDate& a, const OciDate& b) noexcept {
        return a.year() == b.year() && a.month() == b.month() && a.day() == b.day() &&
               a.hour() == b.hour() && a.minute() == b.minute() && a.second() == b.second();
    }

private:
    static OciDate from_text(std::string_view text) {
        const auto parsed = detail::parse_oracle_date(text);
        return OciDate(parsed.year, parsed.month, parsed.day);
    }

    static std::string_view resolve_format(std::string_view format) {
        if (!format.empty()) return format;
        if (default_format_storage().empty()) {
            throw std::runtime_error(
                "binding: OciDate::from_text/to_text needs a format -- pass one explicitly, "
                "or call OciDate::set_default_format(...) once at startup");
        }
        return default_format_storage();
    }
    static std::string& default_format_storage() {
        static std::string fmt; // empty until set_default_format() is called
        return fmt;
    }

    ::OCIDate raw_{};
};
static_assert(sizeof(OciDate) == sizeof(::OCIDate),
              "OciDate must add no members beyond the raw ::OCIDate -- its exact size is what "
              "the real array bind/fetch (OCIBindArrayOfStruct/OCIDefineArrayOfStruct) relies on "
              "for the byte stride between rows");

template <typename T>
inline constexpr bool is_oci_date_v = std::is_same_v<T, OciDate>;

// Oracle TIMESTAMP column -- unlike OciDate, TIMESTAMP's C-side
// representation is an opaque OCIDateTime descriptor (like a LOB
// locator), allocated per value via OCIDescriptorAlloc(OCI_DTYPE_TIMESTAMP)
// and populated via OCIDateTimeConstruct -- there is no fixed-size value
// to read/write inline, so this does NOT work with the array bind/fetch
// mechanisms insert(vector<T>&)/select() use for a batch: a
// std::optional<OciTimestamp> or OciTimestamp field is excluded from
// those paths (bind_one_field's is_oci_datetime_v branch handles it
// row-by-row instead, mirroring OciClob/OciXml's own locator lifecycle).
// Fractional seconds and timezone are not modeled -- add them if a real
// use needs sub-second precision or a TIMESTAMP WITH (LOCAL) TIME ZONE
// column; this covers plain TIMESTAMP (year/month/day/hour/minute/second).
class OciTimestamp {
public:
    OCIDateTime* locator = nullptr;

    OciTimestamp() = default;
    OciTimestamp(short year, unsigned char month, unsigned char day,
                 unsigned char hour = 0, unsigned char minute = 0, unsigned char second = 0)
        : year_(year), month_(month), day_(day), hour_(hour), minute_(minute), second_(second) {}

    // Parses Oracle's default TIMESTAMP format, "DD-MON-RR HH.MI.SS AM"
    // (e.g. "07-SEP-26 03.15.22 PM") -- what TO_TIMESTAMP(text) resolves
    // against under the out-of-the-box NLS_TIMESTAMP_FORMAT, minus the
    // fractional-seconds element (XFF) this class doesn't model (see the
    // class comment above). Throws std::runtime_error, naming `text`, if
    // it doesn't match that shape.
    explicit OciTimestamp(std::string_view text) : OciTimestamp(from_text(text)) {}

    short year() const noexcept { return year_; }
    unsigned char month() const noexcept { return month_; }
    unsigned char day() const noexcept { return day_; }
    unsigned char hour() const noexcept { return hour_; }
    unsigned char minute() const noexcept { return minute_; }
    unsigned char second() const noexcept { return second_; }

    void set(short year, unsigned char month, unsigned char day,
             unsigned char hour, unsigned char minute, unsigned char second) {
        year_ = year; month_ = month; day_ = day;
        hour_ = hour; minute_ = minute; second_ = second;
    }

    // The inverse of the string constructor above: Oracle's default
    // TIMESTAMP format, minus fractional seconds for the same reason the
    // constructor doesn't parse them.
    std::string to_string() const {
        int hour12;
        const char* meridiem;
        detail::to_oracle_12h(hour(), hour12, meridiem);
        return detail::zero_pad(day(), 2) + "-" + detail::oracle_month_name(month()) + "-" +
               detail::zero_pad(year() % 100, 2) + " " + detail::zero_pad(hour12, 2) + "." +
               detail::zero_pad(minute(), 2) + "." + detail::zero_pad(second(), 2) + " " + meridiem;
    }

    // Sets the format used by the no-explicit-format overloads of
    // from_text()/to_text() below -- see OciDate::set_default_format's
    // comment above; this is OciTimestamp's own, independent default (a
    // site's TIMESTAMP format, e.g. "YYYYMMDD HH24:MI:SS", is rarely the
    // same string as its DATE-only format).
    static void set_default_format(std::string_view format) { default_format_storage() = std::string(format); }
    static const std::string& default_format() { return default_format_storage(); }

    // Parses `text` against an explicit Oracle format model via
    // OCIDateTimeFromText -- see OciDate::from_text's comment above for why
    // this needs `conn` (an OCIEnv*/OCIError*) but not a live session/round
    // trip. Builds a temporary OCIDateTime descriptor to parse into, reads
    // the fields back out via OCIDateTimeGetDate/GetTime, then frees the
    // descriptor -- OciTimestamp stores plain year/month/day/hour/minute/
    // second fields here, not a live descriptor, same as every other
    // constructor on this class; bind_one_field (details/oci_client.h)
    // builds its own descriptor from those fields again at bind time via
    // OCIDateTimeConstruct. `format` empty uses set_default_format()'s
    // value; throws if neither was given.
    //
    // Note this does not change OciTimestamp's existing bulk-insert
    // restriction: insert(conn, query_text, std::vector<T>&) still
    // static_asserts against any OciTimestamp field regardless of how its
    // value was constructed, since that limit is about the OCIDateTime
    // descriptor having no fixed-stride array-bind representation, not
    // about how the text got parsed. A row type with a bulk-inserted
    // TIMESTAMP still needs insert(conn, query_text, T&) in a loop.
    static OciTimestamp from_text(OciConnection& conn, std::string_view value,
                                  std::string_view format = {}, std::string_view language = {}) {
        const std::string_view fmt = resolve_format(format);

        OCIDateTime* temp = nullptr;
        if (OCIDescriptorAlloc(conn.env(), reinterpret_cast<void**>(&temp), OCI_DTYPE_TIMESTAMP, 0, nullptr) != OCI_SUCCESS) {
            throw std::runtime_error("binding: OCIDescriptorAlloc(OCI_DTYPE_TIMESTAMP) failed");
        }

        sword status = OCIDateTimeFromText(conn.env(), conn.err(),
            reinterpret_cast<const ::text*>(value.data()), static_cast<std::size_t>(value.size()),
            reinterpret_cast<const ::text*>(fmt.data()), static_cast<ub1>(fmt.size()),
            language.empty() ? nullptr : reinterpret_cast<const ::text*>(language.data()),
            static_cast<std::size_t>(language.size()), temp);

        sb2 year = 0; unsigned char month = 0, day = 0, hour = 0, minute = 0, second = 0; ub4 fsec = 0;
        if (status == OCI_SUCCESS) status = OCIDateTimeGetDate(conn.env(), conn.err(), temp, &year, &month, &day);
        if (status == OCI_SUCCESS) status = OCIDateTimeGetTime(conn.env(), conn.err(), temp, &hour, &minute, &second, &fsec);

        OCIDescriptorFree(reinterpret_cast<void*>(temp), OCI_DTYPE_TIMESTAMP);

        if (status != OCI_SUCCESS) {
            throw std::runtime_error("binding: OCIDateTimeFromText failed parsing '" + std::string(value) +
                                     "' against format '" + std::string(fmt) + "'");
        }
        return OciTimestamp(year, month, day, hour, minute, second);
    }

    // The inverse of from_text() above: builds a temporary descriptor from
    // this value's own fields (the same OCIDateTimeConstruct call
    // bind_one_field makes at bind time), renders it via OCIDateTimeToText,
    // then frees the descriptor.
    std::string to_text(OciConnection& conn, std::string_view format = {}, std::string_view language = {}) const {
        const std::string_view fmt = resolve_format(format);

        OCIDateTime* temp = nullptr;
        if (OCIDescriptorAlloc(conn.env(), reinterpret_cast<void**>(&temp), OCI_DTYPE_TIMESTAMP, 0, nullptr) != OCI_SUCCESS) {
            throw std::runtime_error("binding: OCIDescriptorAlloc(OCI_DTYPE_TIMESTAMP) failed");
        }

        sword status = OCIDateTimeConstruct(conn.env(), conn.err(), temp,
                                            year_, month_, day_, hour_, minute_, second_, 0, nullptr, 0);
        std::array<unsigned char, 128> buf{};
        ub4 buf_size = static_cast<ub4>(buf.size());
        if (status == OCI_SUCCESS) {
            status = OCIDateTimeToText(conn.env(), conn.err(), temp,
                reinterpret_cast<const ::text*>(fmt.data()), static_cast<ub1>(fmt.size()), 0,
                language.empty() ? nullptr : reinterpret_cast<const ::text*>(language.data()),
                static_cast<std::size_t>(language.size()), &buf_size, buf.data());
        }

        OCIDescriptorFree(reinterpret_cast<void*>(temp), OCI_DTYPE_TIMESTAMP);

        if (status != OCI_SUCCESS) {
            throw std::runtime_error("binding: OCIDateTimeToText failed rendering against format '" +
                                     std::string(fmt) + "'");
        }
        return std::string(reinterpret_cast<const char*>(buf.data()), buf_size);
    }

private:
    static std::string_view resolve_format(std::string_view format) {
        if (!format.empty()) return format;
        if (default_format_storage().empty()) {
            throw std::runtime_error(
                "binding: OciTimestamp::from_text/to_text needs a format -- pass one explicitly, "
                "or call OciTimestamp::set_default_format(...) once at startup");
        }
        return default_format_storage();
    }
    static std::string& default_format_storage() {
        static std::string fmt; // empty until set_default_format() is called
        return fmt;
    }

    static OciTimestamp from_text(std::string_view text) {
        const auto space = text.find(' ');
        if (space == std::string_view::npos) {
            throw std::runtime_error("binding: '" + std::string(text) +
                                     "' is not a valid Oracle timestamp "
                                     "(expected DD-MON-RR HH.MI.SS AM)");
        }
        const auto date = detail::parse_oracle_date(text.substr(0, space), text);
        const auto time = detail::parse_oracle_time(text.substr(space + 1), text);
        return OciTimestamp(date.year, date.month, date.day, time.hour, time.minute, time.second);
    }

    short year_ = 0;
    unsigned char month_ = 0, day_ = 0, hour_ = 0, minute_ = 0, second_ = 0;
};

template <typename T>
inline constexpr bool is_oci_datetime_v = std::is_same_v<T, OciTimestamp>;

} // namespace binding
