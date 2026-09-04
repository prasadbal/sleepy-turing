#pragma once
// Implementation of binding::OciConnection -- see binding/oci_connection.h
// for the interface and behavioral contract. Not meant to be included
// directly.
#include "binding/oci_connection.h"

#include <algorithm>
#include <array>
#include <thread>

namespace binding {

inline OciConnection::OciConnection(std::string connect_string, std::string username, std::string password,
                                     int max_retries, std::chrono::milliseconds retry_interval)
    : connect_string_(std::move(connect_string))
    , username_(std::move(username))
    , password_(std::move(password))
    , max_retries_(max_retries)
    , retry_interval_(retry_interval) {}

inline OciConnection::~OciConnection() { disconnect(); }

inline bool OciConnection::connect() {
    if (OCIEnvCreate(&env_, OCI_OBJECT, nullptr, nullptr, nullptr, nullptr, 0, nullptr) != OCI_SUCCESS) {
        env_ = nullptr; // no err_ handle exists yet to get a message out of
        return false;
    }
    if (OCIHandleAlloc(env_, reinterpret_cast<void**>(&err_), OCI_HTYPE_ERROR, 0, nullptr) != OCI_SUCCESS) {
        disconnect();
        return false;
    }
    const sword status = OCILogon2(env_, err_, &svc_,
        reinterpret_cast<const text*>(username_.c_str()), static_cast<ub4>(username_.size()),
        reinterpret_cast<const text*>(password_.c_str()), static_cast<ub4>(password_.size()),
        reinterpret_cast<const text*>(connect_string_.c_str()), static_cast<ub4>(connect_string_.size()),
        OCI_DEFAULT);
    if (status != OCI_SUCCESS) {
        disconnect();
        return false;
    }
    connected_ = true;
    return true;
}

inline void OciConnection::disconnect() {
    if (svc_ && err_) OCILogoff(svc_, err_);
    if (err_) OCIHandleFree(err_, OCI_HTYPE_ERROR);
    if (env_) OCIHandleFree(env_, OCI_HTYPE_ENV);
    svc_ = nullptr;
    err_ = nullptr;
    env_ = nullptr;
    connected_ = false;
}

inline bool OciConnection::is_disconnect_error() const {
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

template <typename Fn>
bool OciConnection::run_with_reconnect(Fn&& attempt) {
    for (int try_num = 0;; ++try_num) {
        // Only run the operation when there is a live session to run it on.
        // A failed reconnect below leaves env_/svc_/err_ null, and the
        // previous version went straight back into attempt(), which then made
        // OCI calls through those null handles -- OCIHandleAlloc(nullptr, ...)
        // followed by OCIStmtPrepare on the null statement that came back.
        if (connected_) {
            const OciOutcome outcome = attempt();
            if (outcome.success) return true;

            if (!is_disconnect_error()) return false; // exec error: never retried
        }

        if (try_num >= max_retries_) return false; // retries exhausted

        std::this_thread::sleep_for(retry_interval_);
        disconnect();
        connect(); // if this fails, connected_ stays false -> next loop retries the connect
    }
}

} // namespace binding
