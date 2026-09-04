#pragma once
#include <chrono>
#include <string>

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
    // Creates the OCI environment with OCI_OBJECT (not OCI_DEFAULT):
    // oci_collection_bind.h's OCIType/OCIObjectNew/OCICollAppend/
    // OCIBindObject calls need the object cache this mode initializes;
    // without it they fail with ORA-21301 "not initialized in object
    // mode". OCI_OBJECT is additive over the plain scalar bind/define
    // path, so it doesn't change behavior for callers that never touch
    // collections -- confirmed by running the scalar-only path unchanged
    // against a real database after this switched from OCI_DEFAULT.
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

    // Runs `attempt` (one full prepare+bind+execute[+fetch] cycle for a
    // single command). If it fails with a disconnect-class error, reconnects
    // and re-runs the *same* attempt from scratch, sleeping retry_interval()
    // in between, up to max_retries() times. Any other failure is returned
    // immediately, un-retried.
    template <typename Fn>
    bool run_with_reconnect(Fn&& attempt);

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
