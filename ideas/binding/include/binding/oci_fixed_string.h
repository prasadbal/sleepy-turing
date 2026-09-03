#pragma once
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "binding/oci_compat.h"

// ============================================================================
// A fixed-capacity character buffer, for binding/defining a CHAR(N) or
// VARCHAR2(N) column -- OCI needs a buffer size fixed before it can define
// an output column, the same reason OciClob/OciXml (oci_lob.h) exist for
// LOB columns. One type serves both CHAR and VARCHAR2 identically from the
// client's side: that distinction is a server-side storage detail (fixed,
// blank-padded vs. variable-length up to N) that doesn't change how the
// client binds to it, so OciChar<N> and OciVarchar2<N> below are the same
// type under two names, chosen for readability at the call site.
// ============================================================================

namespace binding {

template <std::size_t Capacity>
class FixedString {
public:
    static constexpr std::size_t capacity = Capacity;

    FixedString() = default;
    explicit FixedString(std::string_view s) { assign(s); }

    // Truncates silently past Capacity -- matches how OCI itself would
    // truncate an oversized bind value against a fixed column width.
    void assign(std::string_view s) {
        length_ = static_cast<ub2>(s.size() > Capacity ? Capacity : s.size());
        std::memcpy(data_, s.data(), length_);
    }

    std::string_view view() const { return std::string_view(data_, length_); }
    operator std::string_view() const { return view(); } // picked up by is_bindable_leaf / raw_bind_args automatically
    std::string str() const { return std::string(view()); }

    char* data() { return data_; }
    const char* data() const { return data_; }

    // OCI writes the actual fetched length here (as rlenp) when this is
    // used as a select() output column -- see define_one_field.
    ub2& length_ref() { return length_; }
    ub2 length() const { return length_; }

private:
    char data_[Capacity] = {};
    ub2 length_ = 0;
};

template <typename T>
struct is_fixed_string_impl : std::false_type {};
template <std::size_t N>
struct is_fixed_string_impl<FixedString<N>> : std::true_type {};

template <typename T>
inline constexpr bool is_fixed_string_v = is_fixed_string_impl<std::remove_cv_t<T>>::value;

template <std::size_t N> using OciChar = FixedString<N>;     // maps to a CHAR(N) column
template <std::size_t N> using OciVarchar2 = FixedString<N>; // maps to a VARCHAR2(N) column

} // namespace binding
