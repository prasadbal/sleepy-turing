// Real-database verification for the new call_oci/OciHandleGuard/
// OciStatement/OCILob architecture (ideas/new) -- the mock-based
// examples/demo.cpp exercises the same call shapes, but only a real
// database proves the actual OCI semantics this design depends on:
// iters>0 fetching during execute, OCI_NO_DATA on a genuinely empty
// result, state violations being caught before a real (confusing)
// ORA-##### ever has a chance to occur, and a real LOB round-trip.
//
// Not part of any CMake build; compile directly:
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/new/include -I <INSTANT_CLIENT>/sdk/include \
//       ideas/new/examples/live_oracle_demo.cpp \
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT> \
//       -lclntsh -o live_oracle_demo_new
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_demo_new <connect_string> <username> <password>

#include <cstdio>
#include <string>
#include <vector>

#include "binding/oci_connection.h"
#include "binding/oci_lob.h"
#include "binding/oci_statement.h"

using namespace binding;

namespace {
int g_failures = 0;
void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) ++g_failures;
}
const char* status_name(ExecStatus s) {
    switch (s) {
        case ExecStatus::Success:        return "Success";
        case ExecStatus::ConnectionLost: return "ConnectionLost";
        case ExecStatus::QueryError:     return "QueryError";
    }
    return "?";
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <connect_string> <username> <password>\n", argv[0]);
        return 2;
    }

    OciConnection conn(argv[1], argv[2], argv[3]);
    check(conn.connect(), "connect() succeeded");
    if (!conn.connected()) return 1;

    {
        OciStatement stmt(conn);
        stmt.prepare("DROP TABLE new_arch_test");
        stmt.execute(1); // ignore failure: may not exist yet
    }
    {
        OciStatement stmt(conn);
        stmt.prepare("CREATE TABLE new_arch_test (id NUMBER, notional NUMBER)");
        auto r = stmt.execute(1);
        check(r.status == ExecStatus::Success, "CREATE TABLE succeeded");
    }

    std::printf("\n--- state checking: fetch() before execute() ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT id FROM new_arch_test");
        bool threw = false;
        try {
            stmt.fetch(10);
        } catch (const OciStatementStateError&) {
            threw = true;
        }
        check(threw, "fetch() before execute() threw OciStatementStateError, no OCI call attempted");
    }

    std::printf("\n--- bindName(): insert 3 rows one at a time ---\n");
    for (int i = 1; i <= 3; ++i) {
        OciStatement stmt(conn);
        stmt.prepare("INSERT INTO new_arch_test VALUES(:id, :notional)");
        double notional = i * 1.5;
        stmt.bindName("id", SQLT_INT, &i, sizeof(i));
        stmt.bindName("notional", SQLT_BDOUBLE, &notional, sizeof(notional));
        auto r = stmt.execute(1);
        check(r.status == ExecStatus::Success, ("insert row " + std::to_string(i) + " succeeded").c_str());
    }

    std::printf("\n--- bindOutput(): single-row fetch, iters=1 fetches during execute ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT id, notional FROM new_arch_test WHERE id = 2");
        int id = 0;
        double notional = 0.0;
        stmt.bindOutput(1, SQLT_INT, &id, sizeof(id), nullptr, nullptr);
        stmt.bindOutput(2, SQLT_BDOUBLE, &notional, sizeof(notional), nullptr, nullptr);
        auto r = stmt.execute(1);
        check(r.status == ExecStatus::Success, "single-row select() succeeded");
        check(id == 2 && notional == 3.0, "id=2, notional=3.0 fetched correctly");
    }

    std::printf("\n--- batch fetch: bindOutput() with elemSize, execute+fetch loop ---\n");
    {
        struct Row { int id; double notional; };
        OciStatement stmt(conn);
        stmt.prepare("SELECT id, notional FROM new_arch_test ORDER BY id");
        stmt.set_prefetch_rows(100);
        std::vector<Row> batch(2); // deliberately smaller than the 3 real rows: exercises >1 fetch call
        stmt.bindOutput(1, SQLT_INT, &batch[0].id, sizeof(int), nullptr, nullptr, sizeof(Row));
        stmt.bindOutput(2, SQLT_BDOUBLE, &batch[0].notional, sizeof(double), nullptr, nullptr, sizeof(Row));

        auto exec_result = stmt.execute(0);
        std::vector<Row> collected;
        int fetch_calls = 0;
        if (exec_result.status == ExecStatus::Success) {
            for (;;) {
                auto fetch_result = stmt.fetch(static_cast<ub4>(batch.size()));
                ++fetch_calls;
                if (fetch_result.status != ExecStatus::Success) break;
                const ub4 n = stmt.rows_fetched();
                for (ub4 i = 0; i < n; ++i) collected.push_back(batch[i]);
                if (stmt.state() == OciStatement::State::EndOfFetch) break;
            }
        }
        check(collected.size() == 3, "collected all 3 rows across multiple fetch() calls");
        check(fetch_calls >= 2, "took more than one fetch() call for 3 rows at batch size 2");
        if (collected.size() == 3) {
            check(collected[0].id == 1 && collected[1].id == 2 && collected[2].id == 3,
                  "rows came back in id order: 1, 2, 3");
        }
    }

    std::printf("\n--- zero-row query: OCI_NO_DATA classified Success, not QueryError ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT id FROM new_arch_test WHERE id = 999");
        int id = -1;
        stmt.bindOutput(1, SQLT_INT, &id, sizeof(id), nullptr, nullptr);
        auto r = stmt.execute(1);
        check(r.status == ExecStatus::Success, "zero-row query classified Success");
        check(r.call.status == OCI_NO_DATA, "oci_status is OCI_NO_DATA (100)");
        check(id == -1, "output left untouched when no row matched");
        check(stmt.state() == OciStatement::State::EndOfFetch, "state transitioned straight to EndOfFetch");
    }

    std::printf("\n--- a genuine failure: bad table name ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT id FROM no_such_table_xyz");
        auto r = stmt.execute(1);
        check(r.status == ExecStatus::QueryError, "bad table name -> QueryError");
        check(!r.call.error_text.empty(), "call_oci retrieved real error text via OCIErrorGet");
        if (!r.call.error_text.empty()) {
            std::printf("    error_code=%d error_text=%s\n", r.call.error_code, r.call.error_text.c_str());
        }
    }

    std::printf("\n--- OCILob: create temporary, write, read back ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("CREATE TABLE new_arch_lob_test (id NUMBER, body CLOB)");
        stmt.execute(1);

        OCILob write_lob(conn);
        write_lob.create_temporary(OCI_TEMP_CLOB);
        const std::string value = "FRTB sensitivities-based method report body, real Oracle round trip";
        write_lob.write(value.data(), value.size());

        OciStatement insert_stmt(conn);
        insert_stmt.prepare("INSERT INTO new_arch_lob_test VALUES(:id, :body)");
        int id = 1;
        insert_stmt.bindName("id", SQLT_INT, &id, sizeof(id));
        OCILobLocator* loc = write_lob.locator();
        insert_stmt.bindName("body", SQLT_CLOB, &loc, sizeof(loc));
        auto insert_result = insert_stmt.execute(1);
        check(insert_result.status == ExecStatus::Success, "LOB insert succeeded");

        OCILob read_lob(conn);
        OciStatement select_stmt(conn);
        select_stmt.prepare("SELECT body FROM new_arch_lob_test WHERE id = 1");
        select_stmt.bindOutput(1, SQLT_CLOB, read_lob.locator_address(), sizeof(OCILobLocator*), nullptr, nullptr);
        auto select_result = select_stmt.execute(1);
        check(select_result.status == ExecStatus::Success, "LOB select succeeded");
        const std::string readback = read_lob.read(/*is_char_lob=*/true);
        check(readback == value, "LOB content round-tripped byte-for-byte through a real insert+select");

        OciStatement cleanup(conn);
        cleanup.prepare("DROP TABLE new_arch_lob_test");
        cleanup.execute(1);
    }

    {
        OciStatement stmt(conn);
        stmt.prepare("DROP TABLE new_arch_test");
        stmt.execute(1);
    }

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED", g_failures);
    conn.disconnect();
    return g_failures == 0 ? 0 : 1;
}
