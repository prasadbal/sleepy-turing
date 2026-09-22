#pragma once
// OCILob: the low-level LOB *locator* lifecycle -- OCIDescriptorAlloc,
// OCILobCreateTemporary/OCILobWrite2 on the way in, OCILobGetLength2/
// OCILobRead2 on the way out, OCILobFreeTemporary/OCIDescriptorFree
// either way. Deliberately not the user-facing value type (something
// like ideas/binding's OciClob/OciBlob, a plain std::string/vector
// wrapper with no OCI-specific member at all) -- this class exists
// purely to own the locator and its own alloc/populate/free lifecycle so
// nothing above it ever touches an OCILobLocator* directly. A value type
// on top of this would hold the actual text/bytes and use OCILob
// internally only for the duration of one bind or one read, the same
// separation ideas/binding already has via its make_temp_lob/
// free_temp_lob/read_lob_bytes free functions -- this is that same idea
// as an owned object instead of a set of functions threaded through by
// the caller.

#include <db/oracle/oci_call.h>
#include <db/oracle/oci_connection.h>

#include <string>
#include <type_traits>
#include <vector>

namespace marketlib::db::oracle {

class OCILob {
public:
    // Allocates the locator only -- usable immediately as a define
    // target (locator_address()) for a fetch, or turned into a real
    // temporary LOB via create_temporary() for a bind.
    explicit OCILob(OciConnection& conn) : conn_(conn) {
        OCIDescriptorAlloc(conn_.env(), reinterpret_cast<void**>(&locator_), OCI_DTYPE_LOB, 0, nullptr);
    }

    ~OCILob() {
        if (locator_) {
            if (temporary_) OCILobFreeTemporary(conn_.svc(), conn_.err(), locator_);
            OCIDescriptorFree(locator_, OCI_DTYPE_LOB);
        }
    }

    OCILob(const OCILob&) = delete;
    OCILob& operator=(const OCILob&) = delete;

    OCILob(OCILob&& other) noexcept
        : conn_(other.conn_), locator_(other.locator_), temporary_(other.temporary_) {
        other.locator_ = nullptr;
        other.temporary_ = false;
    }

    // lob_type is OCI_TEMP_CLOB or OCI_TEMP_BLOB. Only meaningful before
    // a value has actually been written -- this is the bind-side path.
    OciCallResult create_temporary(ub1 lob_type) {
        temporary_ = true;
        return call_oci(OCILobCreateTemporary, conn_.svc(), conn_.err(), locator_,
                        static_cast<ub2>(0), static_cast<ub1>(SQLCS_IMPLICIT), lob_type,
                        static_cast<int>(0), static_cast<ub2>(OCI_DURATION_SESSION));
    }

    OciCallResult write(const void* data, std::size_t size) {
        oraub8 byte_amt = static_cast<oraub8>(size);
        oraub8 char_amt = 0;
        return call_oci(OCILobWrite2, conn_.svc(), conn_.err(), locator_, &byte_amt, &char_amt,
                        static_cast<ub4>(1), const_cast<void*>(data), static_cast<oraub8>(size),
                        static_cast<ub1>(OCI_ONE_PIECE), nullptr, nullptr,
                        static_cast<ub2>(0), static_cast<ub1>(SQLCS_IMPLICIT));
    }

    // Reads the whole value in one OCI_ONE_PIECE call, sized off
    // OCILobGetLength2 first -- is_char_lob controls whether the length
    // (characters for a CLOB, bytes for a BLOB) needs headroom for a
    // multi-byte charset on the read buffer (AL32UTF8's worst case: 4
    // bytes/character). Same approach as ideas/binding's read_lob_bytes,
    // now as a method instead of a free function taking the locator as
    // a parameter.
    std::string read(bool is_char_lob) const {
        oraub8 length = 0;
        OCILobGetLength2(conn_.svc(), conn_.err(), locator_, &length);
        if (length == 0) return {};
        const std::size_t buffer_bytes =
            is_char_lob ? static_cast<std::size_t>(length) * 4 + 16 : static_cast<std::size_t>(length);
        std::string buf(buffer_bytes, '\0');
        oraub8 byte_amt = static_cast<oraub8>(buffer_bytes);
        oraub8 char_amt = 0;
        OCILobRead2(conn_.svc(), conn_.err(), locator_, &byte_amt, &char_amt, 1,
                    buf.data(), static_cast<oraub8>(buffer_bytes), OCI_ONE_PIECE,
                    nullptr, nullptr, 0, SQLCS_IMPLICIT);
        buf.resize(static_cast<std::size_t>(byte_amt));
        return buf;
    }

    // The address OCIDefineByPos needs as its define target for a LOB
    // column -- OCI writes the fetched value's locator reference through
    // this pointer during fetch.
    OCILobLocator** locator_address() noexcept { return &locator_; }
    OCILobLocator* locator() const noexcept { return locator_; }

private:
    OciConnection& conn_;
    OCILobLocator* locator_ = nullptr;
    bool temporary_ = false;
};

// The user-facing value types this header's own comment above said would
// eventually sit on top of OCILob: a plain std::string/vector wrapper
// with no OCI-specific member at all, usable as an ordinary struct field
// with no connection needed to declare one -- oci_client.h's reflection
// layer is what actually constructs an OCILob (which does need a
// connection) transiently, once per bind or once per fetched row, and
// copies bytes in or out of these. Ported unchanged from ideas/binding's
// own oci_lob.h; see that file's header comment for the fuller rationale
// (nullable LOB fields and insert_rows()-style array-bind aren't wired in
// for a LOB field there either, and the same restrictions apply here).
class OciClob {
public:
    OciClob() = default;
    explicit OciClob(std::string data) : text_data(std::move(data)) {}
    std::string text_data;
};

class OciBlob {
public:
    OciBlob() = default;
    explicit OciBlob(std::vector<unsigned char> data) : binary_data(std::move(data)) {}
    std::vector<unsigned char> binary_data;
};

template <typename T> inline constexpr bool is_oci_clob_v = std::is_same_v<T, OciClob>;
template <typename T> inline constexpr bool is_oci_blob_v = std::is_same_v<T, OciBlob>;
template <typename T> inline constexpr bool is_oci_lob_v = is_oci_clob_v<T> || is_oci_blob_v<T>;

} // namespace marketlib::db::oracle
