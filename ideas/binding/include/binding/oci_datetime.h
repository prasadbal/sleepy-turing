#pragma once
#include <type_traits>

#include "binding/oci_compat.h"

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

    short year() const noexcept { return raw_.OCIDateYYYY; }
    unsigned char month() const noexcept { return raw_.OCIDateMM; }
    unsigned char day() const noexcept { return raw_.OCIDateDD; }
    unsigned char hour() const noexcept { return raw_.OCIDateTime.OCITimeHH; }
    unsigned char minute() const noexcept { return raw_.OCIDateTime.OCITimeMI; }
    unsigned char second() const noexcept { return raw_.OCIDateTime.OCITimeSS; }

    friend bool operator==(const OciDate& a, const OciDate& b) noexcept {
        return a.year() == b.year() && a.month() == b.month() && a.day() == b.day() &&
               a.hour() == b.hour() && a.minute() == b.minute() && a.second() == b.second();
    }

private:
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

private:
    short year_ = 0;
    unsigned char month_ = 0, day_ = 0, hour_ = 0, minute_ = 0, second_ = 0;
};

template <typename T>
inline constexpr bool is_oci_datetime_v = std::is_same_v<T, OciTimestamp>;

} // namespace binding
