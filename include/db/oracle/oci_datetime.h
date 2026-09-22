#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "binding/oci_compat.h"
#include "binding/oci_connection.h"

// ============================================================================
// OciDate/OciTimestamp used to have a second, parallel way to get a value
// from/to text: a hand-rolled parser/renderer for Oracle's own default
// formats ("DD-MON-RR" / "DD-MON-RR HH.MI.SS AM"), living entirely in this
// header with no OciConnection involved at all. That parser duplicated
// logic OCI's real client library already implements correctly (including
// the RR century-rollover rule below) and diverged from it the moment a
// site's actual NLS_DATE_FORMAT wasn't the out-of-the-box default -- which,
// for most reporting shops, it isn't. It has been removed in favor of one
// path for all format-model text conversion: from_text()/to_text() below,
// via OCIDateFromText/OCIDateToText (OCIDateTimeFromText/ToText for
// OciTimestamp) -- Oracle's own interpreter, reachable with a connect()ed
// OciConnection but no live session/round-trip (see from_text()'s comment).
// default_format() is seeded with Oracle's actual defaults, so the
// zero-configuration case behaves the same as the old hand-rolled
// constructor/to_string() did, just through the real interpreter now.
// ============================================================================

namespace binding {

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
// allocation -- unlike OciTimestamp below (and OciClob/OciBlob,
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

    short year() const noexcept { return raw_.OCIDateYYYY; }
    unsigned char month() const noexcept { return raw_.OCIDateMM; }
    unsigned char day() const noexcept { return raw_.OCIDateDD; }
    unsigned char hour() const noexcept { return raw_.OCIDateTime.OCITimeHH; }
    unsigned char minute() const noexcept { return raw_.OCIDateTime.OCITimeMI; }
    unsigned char second() const noexcept { return raw_.OCIDateTime.OCITimeSS; }

    // Sets the format used by the no-explicit-format overloads of
    // from_text()/to_text() below -- e.g. a site that always represents
    // dates as "YYYYMMDD" calls this once at startup instead of repeating
    // the format string at every call site. Independent of OciTimestamp's
    // own default_format() below; the two are set separately. Seeded with
    // Oracle's own real out-of-the-box DATE default ("DD-MON-RR"), so
    // from_text()/to_text() work with no configuration exactly as the old
    // hand-rolled constructor/to_string() did -- just through the real
    // OCIDateFromText/OCIDateToText interpreter now, not a reimplementation
    // of it. Not thread-safe against a concurrent writer -- set it once
    // during startup, before any OciDate parsing/rendering runs on another
    // thread.
    static void set_default_format(std::string_view format) { default_format_storage() = std::string(format); }
    static const std::string& default_format() { return default_format_storage(); }

    // Parses `text` against an explicit Oracle format model (e.g.
    // "YYYYMMDD", "DD/MM/YYYY") via OCIDateFromText -- Oracle's own
    // client-side format-model interpreter. `format` empty (the default)
    // uses whatever set_default_format() last configured, which starts out
    // as Oracle's real default DATE format, "DD-MON-RR".
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
    // explicit Oracle format model via OCIDateToText. Same default-format
    // and connection-but-not-session rules as from_text().
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
    static std::string_view resolve_format(std::string_view format) {
        if (!format.empty()) return format;
        if (default_format_storage().empty()) {
            // Only reachable if a caller explicitly cleared the default
            // (e.g. set_default_format("")) -- the seed value below means
            // this never fires from a fresh, unconfigured state.
            throw std::runtime_error(
                "binding: OciDate::from_text/to_text needs a format -- pass one explicitly, "
                "or call OciDate::set_default_format(...) to restore a default");
        }
        return default_format_storage();
    }
    static std::string& default_format_storage() {
        static std::string fmt = "DD-MON-RR"; // Oracle's own real DATE default
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
// row-by-row instead, mirroring OciClob/OciBlob's own locator lifecycle).
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

    // Sets the format used by the no-explicit-format overloads of
    // from_text()/to_text() below -- see OciDate::set_default_format's
    // comment above; this is OciTimestamp's own, independent default (a
    // site's TIMESTAMP format, e.g. "YYYYMMDD HH24:MI:SS", is rarely the
    // same string as its DATE-only format). Seeded with Oracle's own real
    // out-of-the-box TIMESTAMP default, "DD-MON-RR HH.MI.SS AM" (minus the
    // fractional-seconds element this class doesn't model), so
    // from_text()/to_text() work unconfigured exactly as the old
    // hand-rolled constructor/to_string() did.
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
    // value, which starts out as Oracle's real default TIMESTAMP format.
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
            // Only reachable if a caller explicitly cleared the default --
            // see OciDate::resolve_format's identical comment above.
            throw std::runtime_error(
                "binding: OciTimestamp::from_text/to_text needs a format -- pass one explicitly, "
                "or call OciTimestamp::set_default_format(...) to restore a default");
        }
        return default_format_storage();
    }
    static std::string& default_format_storage() {
        static std::string fmt = "DD-MON-RR HH.MI.SS AM"; // Oracle's own real TIMESTAMP default
        return fmt;
    }

    short year_ = 0;
    unsigned char month_ = 0, day_ = 0, hour_ = 0, minute_ = 0, second_ = 0;
};

template <typename T>
inline constexpr bool is_oci_datetime_v = std::is_same_v<T, OciTimestamp>;

} // namespace binding
