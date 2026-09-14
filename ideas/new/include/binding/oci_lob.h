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

#include "binding/oci_call.h"
#include "binding/oci_connection.h"

#include <string>

namespace binding {

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

} // namespace binding
