#pragma once
#include <chrono>
#include <string>

#include "binding/oci_compat.h"

namespace binding {

// What running a statement against a session came back as. There is no
// retry logic anywhere in this file: a caller that gets ConnectionLost
// decides for itself whether and how to reconnect and try again (this
// class's own connect()/disconnect() are what it would use to do that); a
// caller that gets QueryError knows retrying is pointless -- bad SQL, a
// constraint violation, and the like will just fail the same way again.
enum class ExecStatus { Success, ConnectionLost, QueryError };
struct ExecResult { ExecStatus status; sword oci_status; };

// Owns the OCI environment/service/error handles for one database session.
//
// connect() must be called once before running statements.
//
// Implementation in details/oci_connection.h.
class OciConnection {
public:
    OciConnection(std::string connect_string, std::string username, std::string password,
                  int max_retries = 3,
                  std::chrono::milliseconds retry_interval = std::chrono::milliseconds(1000));

    ~OciConnection();

    OciConnection(const OciConnection&) = delete;
    OciConnection& operator=(const OciConnection&) = delete;

    // Establishes env + error handles, then logs on via OCILogon2 -- one
    // call that does what used to be OCIHandleAlloc(SERVER) +
    // OCIServerAttach + OCIHandleAlloc(SVCCTX) + OCIAttrSet(SERVER) +
    // OCIHandleAlloc(SESSION) + OCIAttrSet(USERNAME) + OCIAttrSet(PASSWORD)
    // + OCISessionBegin + OCIAttrSet(SESSION), for the plain username/
    // password case (no connection pooling, no external authentication)
    // this class actually needs. Every call is checked; a failure tears
    // down whatever partially succeeded via disconnect() rather than
    // continuing on with a handle from a call that never happened.
    //
    bool connect();

    void disconnect();

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
    bool is_disconnect_error() const;

    // Runs an already-prepared, already-bound statement against this
    // session and classifies the result. `iters` is OCIStmtExecute's own
    // parameter: 1 for an ordinary single-row statement (DML or SELECT), N
    // for an array bind of N rows, 0 for a SELECT you intend to fetch from
    // without pre-fetching any rows at execute time. The caller builds the
    // statement (prepare + bind) and owns/frees the handle -- this only
    // runs it.
    ExecResult execute(OCIStmt* stmt, ub4 iters = 1) const;

private:
    std::string connect_string_;
    std::string username_;
    std::string password_;
    int max_retries_;
    std::chrono::milliseconds retry_interval_;

    OCIEnv*    env_ = nullptr;
    OCIError*  err_ = nullptr;
    OCISvcCtx* svc_ = nullptr;
    bool connected_ = false;
};

} // namespace binding

#include "binding/details/oci_connection.h"
