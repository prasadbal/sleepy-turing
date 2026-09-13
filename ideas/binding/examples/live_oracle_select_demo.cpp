// Verifies select() -- the struct-free, positional-output, single-row
// fetch -- against a real database: OCIStmtExecute(iters=1) genuinely
// fetches the first row as part of execute() itself (no separate
// OCIStmtFetch2 call), a zero-row query leaves the outputs untouched and
// reports OCI_NO_DATA rather than an error, and a multi-row query
// silently takes just the first row.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the compile command and Instant-Client-version notes.
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_select_demo.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_select_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_select_demo <connect_string> <username> <password>

#include <cstdio>
#include <string>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_fixed_string.h"

namespace {
bool check(bool cond, const char* what, int* failures) {
    std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) ++*failures;
    return cond;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <connect_string> <username> <password>\n", argv[0]);
        return 2;
    }

    binding::OciConnection conn(argv[1], argv[2], argv[3]);
    if (!conn.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }
    std::printf("connected.\n\n");

    binding::execute(conn, "DROP TABLE select_demo_test");
    binding::execute(conn, "CREATE TABLE select_demo_test (id NUMBER, name VARCHAR2(16))");
    binding::execute(conn, "INSERT INTO select_demo_test VALUES (1, 'ALPHA')");
    binding::execute(conn, "INSERT INTO select_demo_test VALUES (2, 'BETA')");
    binding::execute(conn, "INSERT INTO select_demo_test VALUES (3, 'GAMMA')");

    int failures = 0;

    // 1. SELECT COUNT(*) straight into a plain long long -- no struct.
    long long count = 0;
    auto r1 = binding::select(conn, "SELECT COUNT(*) FROM select_demo_test", count);
    check(r1.status == binding::ExecStatus::Success, "COUNT(*) select() succeeded", &failures);
    check(count == 3, "COUNT(*) returned 3", &failures);

    // 2. Two columns, positionally, into two plain variables.
    int id = 0;
    binding::FixedString<16> name;
    auto r2 = binding::select(conn, "SELECT id, name FROM select_demo_test WHERE id = 2", id, name);
    check(r2.status == binding::ExecStatus::Success, "id=2 select() succeeded", &failures);
    check(id == 2 && name.str() == "BETA", "id=2 fetched id=2, name=BETA correctly", &failures);

    // 3. Zero matching rows: outputs untouched, status Success, oci_status
    //    reports OCI_NO_DATA rather than an error.
    int no_id = -999;
    binding::FixedString<16> no_name("UNTOUCHED");
    auto r3 = binding::select(conn, "SELECT id, name FROM select_demo_test WHERE id = 999", no_id, no_name);
    check(r3.status == binding::ExecStatus::Success, "zero-row query still classified Success", &failures);
    check(r3.oci_status == OCI_NO_DATA, "zero-row query's oci_status is OCI_NO_DATA (100)", &failures);
    check(no_id == -999 && no_name.str() == "UNTOUCHED", "outputs left untouched when no row matched", &failures);

    // 4. Multiple matching rows: silently takes just the first (id order).
    int first_id = 0;
    binding::FixedString<16> first_name;
    auto r4 = binding::select(conn, "SELECT id, name FROM select_demo_test ORDER BY id", first_id, first_name);
    check(r4.status == binding::ExecStatus::Success, "multi-row query succeeded", &failures);
    check(first_id == 1 && first_name.str() == "ALPHA",
          "multi-row query silently took just the first row (id=1, ALPHA)", &failures);

    // 5. Failure classification still works: a bad table name is a real
    //    QueryError, not confused with the zero-row/OCI_NO_DATA case.
    long long bad = 0;
    auto r5 = binding::select(conn, "SELECT COUNT(*) FROM no_such_table_xyz", bad);
    check(r5.status == binding::ExecStatus::QueryError, "bad table name -> QueryError, not Success", &failures);

    std::printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");

    binding::execute(conn, "DROP TABLE select_demo_test");
    conn.disconnect();
    return failures == 0 ? 0 : 1;
}
