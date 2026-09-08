// Verifies ExecStatus::ConnectionLost against a genuinely killed session --
// something the mock can only simulate (see FailureMode::DisconnectThenRecover
// in oci_mock.h), not actually reproduce. This is the one thing
// live_oracle_demo.cpp doesn't cover: that demo's own "malformed statement"
// check only proves QueryError, since nothing in a normal run actually
// kills the session out from under it.
//
// Two connections: the "victim" runs a statement, then the "killer" issues
// ALTER SYSTEM KILL SESSION against the victim's own SID/SERIAL# (read back
// via V$SESSION), and the victim tries again -- which should now report
// ConnectionLost rather than hang or crash.
//
// Compile exactly like live_oracle_demo.cpp -- see that file's header
// comment for the full command and the Instant-Client-version notes.
//
// Verified against gvenzl/oracle-free:23 (docker) with Oracle Instant
// Client 19.32: the victim's SID/SERIAL# read back correctly, the kill
// statement succeeded, and the victim's next execute() came back
// ExecStatus::ConnectionLost with the underlying ORA-03113 status -- the
// same classification path is_disconnect_error() already used before this
// rewrite, just no longer wrapped in an automatic retry loop.

#include <cstdio>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"

namespace {
const char* status_name(binding::ExecStatus s) {
    switch (s) {
        case binding::ExecStatus::Success:        return "Success";
        case binding::ExecStatus::ConnectionLost: return "ConnectionLost";
        case binding::ExecStatus::QueryError:     return "QueryError";
    }
    return "?";
}
} // namespace

struct SidSerial {
    int sid;
    int serial;
};

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <connect_string> <username> <password>\n", argv[0]);
        return 2;
    }

    binding::OciConnection victim(argv[1], argv[2], argv[3]);
    binding::OciConnection killer(argv[1], argv[2], argv[3]);
    if (!victim.connect() || !killer.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }

    std::vector<SidSerial> self;
    std::function<void(const SidSerial*, std::size_t)> on_batch =
        [&](const SidSerial* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) self.push_back(batch[i]);
        };
    binding::select_rows<SidSerial>(
        victim, "SELECT SID, SERIAL# FROM V$SESSION WHERE AUDSID = SYS_CONTEXT('USERENV','SESSIONID')",
        5, 5, on_batch);
    if (self.empty()) {
        std::fprintf(stderr, "could not read back the victim's own SID/SERIAL#\n");
        return 1;
    }
    std::printf("victim session: SID=%d SERIAL#=%d\n", self[0].sid, self[0].serial);

    char kill_sql[128];
    std::snprintf(kill_sql, sizeof(kill_sql), "ALTER SYSTEM KILL SESSION '%d,%d' IMMEDIATE",
                  self[0].sid, self[0].serial);
    auto kill_result = binding::execute(killer, kill_sql);
    std::printf("kill statement -> %s\n", status_name(kill_result.status));

    // The server needs a moment to actually tear the session down after
    // IMMEDIATE returns -- this is a fixed, generous wait rather than a
    // polling loop, since there is no cheap, reliable "is it dead yet"
    // check to poll from the client side without risking the exact
    // ambiguous states this test exists to get past.
    for (volatile long i = 0; i < 300000000L; ++i) {
    }

    // A PL/SQL block, not a SELECT: no bind, no output column to define,
    // so this exercises OciConnection::execute() directly rather than
    // tripping over an unrelated "define not done" error from asking a
    // SELECT to run through the no-define execute() path.
    auto after = binding::execute(victim, "BEGIN NULL; END;");
    std::printf("execute() on the killed session -> %s (oci_status=%d)\n",
                status_name(after.status), (int)after.oci_status);

    const bool ok = (after.status == binding::ExecStatus::ConnectionLost);
    std::printf("\n%s\n", ok ? "PASSED: killed session correctly classified as ConnectionLost"
                              : "FAILED: expected ConnectionLost");
    return ok ? 0 : 1;
}
