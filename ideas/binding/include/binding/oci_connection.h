#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>

#include "binding/oci_compat.h"

namespace binding {

struct OciOutcome {
    bool success = false;
    sword status = OCI_ERROR;
};

// Owns the OCI environment/service/error handles for one database session
// and implements the reconnect policy:
//
//   - On a disconnect-class error (session/network lost), tear down and
//     re-establish the session, then retry the *whole* failed operation from
//     scratch, up to max_retries() times, sleeping retry_interval() between
//     attempts.
//   - On any other execution error (bad SQL, constraint violation, no data
//     found, ...) the operation is returned to the caller immediately,
//     un-retried -- it isn't a connectivity problem, and retrying would just
//     reproduce the same error.
//
// connect() must be called once before running statements; run_with_reconnect
// only handles a session dying *during* use, not the initial connect.
class OciConnection {
public:
    OciConnection(std::string connect_string, std::string username, std::string password,
                  int max_retries = 3,
                  std::chrono::milliseconds retry_interval = std::chrono::milliseconds(1000))
        : connect_string_(std::move(connect_string))
        , username_(std::move(username))
        , password_(std::move(password))
        , max_retries_(max_retries)
        , retry_interval_(retry_interval) {}

    ~OciConnection() { disconnect(); }

    OciConnection(const OciConnection&) = delete;
    OciConnection& operator=(const OciConnection&) = delete;

    bool connect() {
        OCIEnvCreate(&env_, OCI_DEFAULT, nullptr, nullptr, nullptr, nullptr, 0, nullptr);
        OCIHandleAlloc(env_, reinterpret_cast<void**>(&err_), OCI_HTYPE_ERROR, 0, nullptr);
        OCIHandleAlloc(env_, reinterpret_cast<void**>(&server_), OCI_HTYPE_SERVER, 0, nullptr);
        OCIServerAttach(server_, err_,
                        reinterpret_cast<const text*>(connect_string_.c_str()),
                        static_cast<sb4>(connect_string_.size()), OCI_DEFAULT);

        OCIHandleAlloc(env_, reinterpret_cast<void**>(&svc_), OCI_HTYPE_SVCCTX, 0, nullptr);
        OCIAttrSet(svc_, OCI_HTYPE_SVCCTX, server_, 0, OCI_ATTR_SERVER, err_);

        OCIHandleAlloc(env_, reinterpret_cast<void**>(&session_), OCI_HTYPE_SESSION, 0, nullptr);
        OCIAttrSet(session_, OCI_HTYPE_SESSION,
                   const_cast<char*>(username_.c_str()), static_cast<ub4>(username_.size()),
                   OCI_ATTR_USERNAME, err_);
        OCIAttrSet(session_, OCI_HTYPE_SESSION,
                   const_cast<char*>(password_.c_str()), static_cast<ub4>(password_.size()),
                   OCI_ATTR_PASSWORD, err_);

        const sword status = OCISessionBegin(svc_, err_, session_, OCI_CRED_RDBMS, OCI_DEFAULT);
        if (status != OCI_SUCCESS) {
            disconnect();
            return false;
        }
        OCIAttrSet(svc_, OCI_HTYPE_SVCCTX, session_, 0, OCI_ATTR_SESSION, err_);
        connected_ = true;
        return true;
    }

    void disconnect() {
        if (session_ && svc_) OCISessionEnd(svc_, err_, session_, OCI_DEFAULT);
        if (server_) OCIServerDetach(server_, err_, OCI_DEFAULT);
        if (session_) OCIHandleFree(session_, OCI_HTYPE_SESSION);
        if (svc_) OCIHandleFree(svc_, OCI_HTYPE_SVCCTX);
        if (server_) OCIHandleFree(server_, OCI_HTYPE_SERVER);
        if (err_) OCIHandleFree(err_, OCI_HTYPE_ERROR);
        if (env_) OCIHandleFree(env_, OCI_HTYPE_ENV);
        session_ = nullptr;
        svc_ = nullptr;
        server_ = nullptr;
        err_ = nullptr;
        env_ = nullptr;
        connected_ = false;
    }

    OCIEnv*    env() const noexcept { return env_; }
    OCISvcCtx* svc() const noexcept { return svc_; }
    OCIError*  err() const noexcept { return err_; }
    bool connected() const noexcept { return connected_; }
    int max_retries() const noexcept { return max_retries_; }
    std::chrono::milliseconds retry_interval() const noexcept { return retry_interval_; }

    // Inspects the last error recorded on err_ and classifies it as a lost
    // session/connection rather than a data or SQL execution problem. This
    // code list covers the common "session is gone" cases; tune it for your
    // environment (RAC failover, DRCP, firewall idle-kills, ...).
    bool is_disconnect_error() const {
        if (!err_) return true; // no session at all -- definitely need to reconnect

        sb4 error_code = 0;
        std::array<unsigned char, 512> buf{};
        OCIErrorGet(err_, 1, nullptr, &error_code, buf.data(),
                    static_cast<ub4>(buf.size()), OCI_HTYPE_ERROR);

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
        return std::find(std::begin(disconnect_codes), std::end(disconnect_codes), error_code)
               != std::end(disconnect_codes);
    }

    // Runs `attempt` (one full prepare+bind+execute[+fetch] cycle for a
    // single command). If it fails with a disconnect-class error, reconnects
    // and re-runs the *same* attempt from scratch, sleeping retry_interval()
    // in between, up to max_retries() times. Any other failure is returned
    // immediately, un-retried.
    template <typename Fn>
    bool run_with_reconnect(Fn&& attempt) {
        for (int try_num = 0;; ++try_num) {
            const OciOutcome outcome = attempt();
            if (outcome.success) return true;

            if (!is_disconnect_error()) return false; // exec error: never retried
            if (try_num >= max_retries_) return false; // retries exhausted

            std::this_thread::sleep_for(retry_interval_);
            disconnect();
            connect(); // if this fails too, err_ stays null -> next loop retries again
        }
    }

private:
    std::string connect_string_;
    std::string username_;
    std::string password_;
    int max_retries_;
    std::chrono::milliseconds retry_interval_;

    OCIEnv*     env_     = nullptr;
    OCIError*   err_     = nullptr;
    OCIServer*  server_  = nullptr;
    OCISvcCtx*  svc_     = nullptr;
    OCISession* session_ = nullptr;
    bool connected_ = false;
};

} // namespace binding
