#pragma once
// Owns the OCI environment/service/error handles for one database
// session, same job as ideas/binding's OciConnection, rewritten on top
// of call_oci and OciHandleGuard instead of hand-checking each raw OCI
// call's return code inline.
//
// env_ (OCIEnvCreate) and svc_ (OCILogon2) aren't plain OCIHandleAlloc
// allocations, so they can't use OciHandleGuard directly -- both have
// their own dedicated create/destroy API pair (OCIEnvCreate/
// OCIHandleFree(ENV), OCILogon2/OCILogoff). err_ genuinely is a plain
// OCIHandleAlloc handle, so it uses OCIErrorHandle.

#include "binding/oci_call.h"
#include "binding/oci_handle_guard.h"

#include <array>
#include <optional>
#include <string>

namespace binding {

enum class ExecStatus { Success, ConnectionLost, QueryError };

// Carries both the classified status (what a caller branches on) and the
// full call_oci result underneath it (the raw OCI status plus whatever
// error text/code was retrieved) -- classification is a judgment call
// (is this error code "the session is gone"?); the raw detail is what
// that judgment was actually based on, kept around rather than discarded
// so a caller that wants the real ORA-##### text still can.
struct ExecResult {
    ExecStatus status = ExecStatus::Success;
    OciCallResult call;
};

class OciConnection {
public:
    OciConnection(std::string connect_string, std::string username, std::string password)
        : connect_string_(std::move(connect_string)), username_(std::move(username)),
          password_(std::move(password)) {}

    ~OciConnection() { disconnect(); }

    OciConnection(const OciConnection&) = delete;
    OciConnection& operator=(const OciConnection&) = delete;

    bool connect() {
        if (OCIEnvCreate(&env_, OCI_DEFAULT, nullptr, nullptr, nullptr, nullptr, 0, nullptr) != OCI_SUCCESS) {
            env_ = nullptr;
            return false;
        }
        err_.emplace(env_);

        const OciCallResult login = call_oci(OCILogon2, env_, err_->get(), &svc_,
            reinterpret_cast<const text*>(username_.c_str()), static_cast<ub4>(username_.size()),
            reinterpret_cast<const text*>(password_.c_str()), static_cast<ub4>(password_.size()),
            reinterpret_cast<const text*>(connect_string_.c_str()), static_cast<ub4>(connect_string_.size()),
            static_cast<ub4>(OCI_DEFAULT));

        // OCI_SUCCESS_WITH_INFO is a real, successful login (ORA-28002
        // password-about-to-expire being the practical case behind it),
        // not a failure -- see docs/oci_statement_lifecycle_notes.md
        // (carried over from ideas/binding, same finding applies here).
        // login.error_text carries that warning when present, for
        // whoever wants to surface it; not read here.
        if (login.status != OCI_SUCCESS && login.status != OCI_SUCCESS_WITH_INFO) {
            disconnect();
            return false;
        }
        connected_ = true;
        return true;
    }

    void disconnect() {
        if (svc_ && err_) OCILogoff(svc_, err_->get());
        err_.reset();
        if (env_) OCIHandleFree(env_, OCI_HTYPE_ENV);
        svc_ = nullptr;
        env_ = nullptr;
        connected_ = false;
    }

    OCIEnv* env() const noexcept { return env_; }
    OCISvcCtx* svc() const noexcept { return svc_; }
    OCIError* err() const noexcept { return err_ ? err_->get() : nullptr; }
    bool connected() const noexcept { return connected_; }

    // Classifies a call_oci result using this connection's own knowledge
    // of which ORA-codes mean "the session is gone" -- doesn't re-query
    // OCI, since call.error_code is already sitting there from whichever
    // call_oci invocation produced it. OCI_SUCCESS, OCI_SUCCESS_WITH_INFO,
    // and OCI_NO_DATA all classify as Success here -- none of the three
    // is a query failure (see oci_connection.h's own header comment and
    // docs/oci_statement_lifecycle_notes.md for OCI_NO_DATA/
    // OCI_SUCCESS_WITH_INFO specifically).
    ExecStatus classify(const OciCallResult& call) const {
        if (call.status == OCI_SUCCESS || call.status == OCI_SUCCESS_WITH_INFO || call.status == OCI_NO_DATA) {
            return ExecStatus::Success;
        }
        static constexpr sb4 disconnect_codes[] = {
            28,    // ORA-00028: your session has been killed
            1012,  // ORA-01012: not logged on
            2396,  // ORA-02396: exceeded maximum idle time
            3113,  // ORA-03113: end-of-file on communication channel
            3114,  // ORA-03114: not connected to ORACLE
            3135,  // ORA-03135: connection lost contact
            12153, // ORA-12153: TNS:not connected
            12537, // ORA-12537: TNS:connection closed
            12571, // ORA-12571: TNS:packet writer failure
            25408, // ORA-25408: can not safely replay call
        };
        for (sb4 code : disconnect_codes) {
            if (call.error_code == code) return ExecStatus::ConnectionLost;
        }
        return ExecStatus::QueryError;
    }

private:
    std::string connect_string_, username_, password_;
    OCIEnv* env_ = nullptr;
    std::optional<OCIErrorHandle> err_;
    OCISvcCtx* svc_ = nullptr;
    bool connected_ = false;
};

} // namespace binding
