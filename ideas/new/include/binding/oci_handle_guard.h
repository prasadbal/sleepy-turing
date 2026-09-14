#pragma once
// OciHandleGuard<HandleType, HandleTypeEnum>: one generic RAII wrapper
// for every OCI handle kind, instead of hand-writing an OCIHandleAlloc/
// OCIHandleFree pair per handle type. HandleTypeEnum is the OCI_HTYPE_*
// value for that handle -- allocation and freeing both need it, and
// tying it to the type via a template parameter (rather than passing it
// at every call site) makes it impossible to allocate as one type and
// free as another by mistake.
//
// Not every alias below is used by this project's own OciConnection --
// OCIServerHandle/OCISvcCtxHandle/OCISessionHandle exist for a codebase
// doing the older, manual OCIServerAttach + OCIHandleAlloc(SVCCTX) +
// OCISessionBegin + OCIAttrSet(OCI_ATTR_SESSION) sequence. This project's
// OciConnection uses OCILogon2 instead, which does all of that
// atomically in one call and never exposes those three handles
// separately -- see oci_connection.h. They're declared here anyway,
// both for completeness against the full set of handle kinds a
// lower-level caller might need, and because getting the
// OCI_ATTR_SESSION attach step wrong in the manual sequence is exactly
// what produced a real ORA-24324 ("service handle not initialized")
// while building this -- a service handle with a server but no session
// attached, which read back as valid via OCI_ATTR_SERVER while still
// being unusable. See docs/oci_statement_lifecycle_notes.md (copied from
// ideas/binding, still accurate here) for the full story.

#include "binding/oci_compat.h"

namespace binding {

template <typename HandleType, ub4 HandleTypeEnum>
class OciHandleGuard {
public:
    // env is always an OCIEnv* -- even a handle type that isn't itself
    // the environment (a statement handle, an error handle) is allocated
    // *from* the environment handle, per OCIHandleAlloc's own contract.
    explicit OciHandleGuard(OCIEnv* env) {
        OCIHandleAlloc(env, reinterpret_cast<void**>(&handle_), HandleTypeEnum, 0, nullptr);
    }

    ~OciHandleGuard() {
        if (handle_) OCIHandleFree(handle_, HandleTypeEnum);
    }

    OciHandleGuard(const OciHandleGuard&) = delete;
    OciHandleGuard& operator=(const OciHandleGuard&) = delete;

    OciHandleGuard(OciHandleGuard&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    OciHandleGuard& operator=(OciHandleGuard&& other) noexcept {
        if (this != &other) {
            if (handle_) OCIHandleFree(handle_, HandleTypeEnum);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HandleType* get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HandleType* handle_ = nullptr;
};

using OCIEnvHandle     = OciHandleGuard<OCIEnv,     OCI_HTYPE_ENV>;
using OCIErrorHandle   = OciHandleGuard<OCIError,   OCI_HTYPE_ERROR>;
using OCIServerHandle  = OciHandleGuard<OCIServer,  OCI_HTYPE_SERVER>;
using OCISvcCtxHandle  = OciHandleGuard<OCISvcCtx,  OCI_HTYPE_SVCCTX>;
using OCISessionHandle = OciHandleGuard<OCISession, OCI_HTYPE_SESSION>;
using OCIStmtHandle    = OciHandleGuard<OCIStmt,    OCI_HTYPE_STMT>;

} // namespace binding
