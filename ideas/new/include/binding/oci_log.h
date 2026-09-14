#pragma once
// Query logging: opt-in, off by default -- see ideas/binding's
// set_query_logger for the same idea over there. OciStatement calls into
// this at execute() time with the SQL text plus every bindName() value
// rendered as text; bindOutput() calls are never logged (nothing to log
// yet -- the value doesn't exist until after execute()/fetch() runs).
//
// Rendering a bound value here is harder than in ideas/binding: bindName()
// is type-erased (an OCI type code + a raw void*, not a C++ type a
// template could dispatch on), so render_typed_value() below switches on
// the *runtime* SQLT_* code instead of a compile-time type. It only
// covers the common cases (integer widths, float/double, a character
// buffer, OciDate via OCIDateToText) -- an unrecognized type code renders
// as "<unrendered type N>" rather than guessing at how to interpret
// arbitrary bytes.

#include "binding/oci_connection.h"

#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace binding {

using StatementLogger = std::function<void(std::string_view line)>;

inline StatementLogger& statement_logger() {
    static StatementLogger logger;
    return logger;
}

inline void set_statement_logger(StatementLogger logger) { statement_logger() = std::move(logger); }

// data/len are exactly what was passed to bindName() -- len is the
// buffer size the caller gave, not a separately-tracked content length,
// so a character buffer renders however many bytes of `len` look like
// real content (trimmed of trailing NULs), not necessarily the exact
// value Oracle will actually bind if the caller over-allocated.
inline std::string render_typed_value(OciConnection& conn, ub2 data_type, const void* data, sb4 len) {
    if (!data) return "NULL";
    switch (data_type) {
        case SQLT_INT:
            if (len == 2) return std::to_string(*static_cast<const short*>(data));
            if (len == 4) return std::to_string(*static_cast<const int*>(data));
            if (len == 8) return std::to_string(*static_cast<const long long*>(data));
            break;
        case SQLT_UIN:
            if (len == 2) return std::to_string(*static_cast<const unsigned short*>(data));
            if (len == 4) return std::to_string(*static_cast<const unsigned int*>(data));
            if (len == 8) return std::to_string(*static_cast<const unsigned long long*>(data));
            break;
        case SQLT_BFLOAT:
            return std::to_string(*static_cast<const float*>(data));
        case SQLT_BDOUBLE:
            return std::to_string(*static_cast<const double*>(data));
        case SQLT_CHR:
        case SQLT_AFC: {
            const char* chars = static_cast<const char*>(data);
            std::size_t n = static_cast<std::size_t>(len);
            while (n > 0 && chars[n - 1] == '\0') --n; // trim trailing NULs from an over-sized buffer
            return "'" + std::string(chars, n) + "'";
        }
        case SQLT_ODT: {
            std::array<unsigned char, 64> buf{};
            ub4 buf_size = static_cast<ub4>(buf.size());
            const auto* date = static_cast<const ::OCIDate*>(data);
            if (OCIDateToText(conn.err(), date,
                              reinterpret_cast<const text*>("DD-MON-RR"), 9,
                              nullptr, 0, &buf_size, buf.data()) == OCI_SUCCESS) {
                return "'" + std::string(reinterpret_cast<const char*>(buf.data()), buf_size) + "'";
            }
            return "<OciDate, unrenderable>";
        }
        case SQLT_CLOB:
            return "<CLOB>";
        case SQLT_BLOB:
            return "<BLOB>";
        default:
            break;
    }
    return "<unrendered type " + std::to_string(data_type) + ">";
}

} // namespace binding
