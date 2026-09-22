// Verifies the "EndOfFetch during exec, for a genuinely empty table"
// scenario across every entry point that can hit it: with zero real
// rows, execute(0) itself reaches OCI_ATTR_STMT_STATE == END_OF_FETCH
// immediately -- the same mechanism as the prefetch-overrun case
// documented in docs/oci_statement_lifecycle_notes.md, just guaranteed
// rather than conditional on prefetch_rows vs. the real row count (zero
// rows means "done" no matter what prefetch_rows says). Every one of
// these paths calls describeColumns()/describeColumnPosition()/
// bindOutput() after execute() -- exactly the calls that had to be
// relaxed to tolerate EndOfFetch -- so an empty table exercises that
// fix even harder than the original prefetch-overrun finding did.
//
// Builds as the db_live_oracle_empty_table_demo target; needs a real Oracle
// client configured (see live_oracle_demo.cpp for the cmake -D options), then:
//
//   cmake --build build/linux-release --target db_live_oracle_empty_table_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> build/linux-release/core/db/db_live_oracle_empty_table_demo <connect_string> <user> <password>
#include <cstdio>

#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>

using namespace marketlib::db::oracle;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
    if (!ok) ++g_failures;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: %s <connect_string> <username> <password>\n", argv[0]);
        return 1;
    }
    OciConnection conn(argv[1], argv[2], argv[3]);
    check(conn.connect(), "connect() succeeded");

    execute(conn, "DROP TABLE new_arch_empty_test");
    check(execute(conn, "CREATE TABLE new_arch_empty_test (id NUMBER, name VARCHAR2(16))").status ==
              ExecStatus::Success,
          "CREATE TABLE succeeded");
    // Deliberately no rows inserted -- the table stays empty for every check below.

    std::printf("\n--- select_generic() on an empty table ---\n");
    {
        std::size_t rows_seen = 0;
        auto r = select_generic(conn, "SELECT id, name FROM new_arch_empty_test", 10, 10,
            [&](const GenericBatch& batch) { rows_seen += batch.row_count; });
        check(r.status == ExecStatus::Success, "classified Success, not QueryError");
        check(r.call.status == OCI_NO_DATA, "oci_status is OCI_NO_DATA (100)");
        check(rows_seen == 0, "callback saw 0 rows, was invoked without throwing");
    }

    std::printf("\n--- select_rows<T>() on an empty table ---\n");
    {
        struct Row { int id; FixedString<16> name; };
        std::size_t rows_seen = 0;
        auto r = select_rows<Row>(conn, "SELECT id, name FROM new_arch_empty_test", 10, 10,
            [&](const Row*, std::size_t n) { rows_seen += n; });
        check(r.status == ExecStatus::Success, "classified Success, not QueryError");
        check(rows_seen == 0, "callback saw 0 rows, was invoked without throwing");
    }

    std::printf("\n--- select<T...>() on an empty table ---\n");
    {
        int id = -999;
        auto r = select(conn, "SELECT id FROM new_arch_empty_test WHERE id = 1", id);
        check(r.status == ExecStatus::Success, "classified Success, not QueryError");
        check(r.call.status == OCI_NO_DATA, "oci_status is OCI_NO_DATA (100)");
        check(id == -999, "output argument left untouched, not overwritten with garbage");
    }

    std::printf("\n--- low-level OciStatement directly, prefetch=100, empty table ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT id, name FROM new_arch_empty_test");
        stmt.set_prefetch_rows(100);
        auto exec_result = stmt.execute(0);
        check(exec_result.status == ExecStatus::Success, "execute(0) classified Success");
        check(stmt.state() == OciStatement::State::EndOfFetch,
              "state() is already EndOfFetch right after execute(0), before any bindOutput()/fetch() call");

        auto columns = stmt.describeColumns();
        check(columns.size() == 2, "describeColumns() still works from EndOfFetch -- found 2 columns");

        int id_buf = 0;
        stmt.bindOutput(1, SQLT_INT, &id_buf, sizeof(id_buf), nullptr, nullptr, sizeof(id_buf));
        check(true, "bindOutput() didn't throw from EndOfFetch");

        auto fetch_result = stmt.fetch(10);
        check(fetch_result.status == ExecStatus::Success, "fetch() from EndOfFetch classified Success");
        check(stmt.rows_fetched() == 0, "rows_fetched() is 0, not garbage");
    }

    execute(conn, "DROP TABLE new_arch_empty_test");

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
