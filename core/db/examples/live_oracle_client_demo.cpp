// Real-database verification for oci_client.h -- the reflection layer
// built on top of OciStatement. See live_oracle_demo.cpp for the
// lower-level OciStatement verification this builds on.
//
// Builds as the db_live_oracle_client_demo target; needs a real Oracle client
// configured (see live_oracle_demo.cpp for the cmake -D options), then:
//
//   cmake --build build/linux-release --target db_live_oracle_client_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> build/linux-release/core/db/db_live_oracle_client_demo <connect_string> <user> <password>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

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

    execute(conn, "DROP TABLE new_arch_client_test");
    check(execute(conn, "CREATE TABLE new_arch_client_test (id NUMBER, notional NUMBER, name VARCHAR2(16), "
                        "report CLOB)").status == ExecStatus::Success,
          "CREATE TABLE succeeded");

    std::printf("\n--- execute() with named bind params, including a nullable field ---\n");
    {
        struct Row { int id; FixedString<16> name; std::optional<double> notional; };
        Row r1{1, FixedString<16>("Alpha"), 2.5};
        auto e1 = execute(conn, "INSERT INTO new_arch_client_test(id, name, notional) VALUES(:id, :name, :notional)", r1);
        check(e1.status == ExecStatus::Success, "insert row 1 (notional=2.5) succeeded");

        Row r2{2, FixedString<16>("Beta"), std::nullopt};
        auto e2 = execute(conn, "INSERT INTO new_arch_client_test(id, name, notional) VALUES(:id, :name, :notional)", r2);
        check(e2.status == ExecStatus::Success, "insert row 2 (notional=NULL) succeeded");
    }

    std::printf("\n--- select_rows() -- batch fetch by name, mixed column order + optional field ---\n");
    {
        // Deliberately NOT the table's own column order, and includes an
        // optional<double> to make sure the NULL row (id=2) round-trips
        // as std::nullopt, not a stale/garbage value.
        struct Row { std::optional<double> notional; int id; FixedString<16> name; };
        std::vector<Row> collected;
        auto r = select_rows<Row>(conn, "SELECT id, notional, name FROM new_arch_client_test ORDER BY id",
                                  10, 10,
            [&](const Row* rows, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) collected.push_back(rows[i]);
            });
        check(r.status == ExecStatus::Success, "select_rows() succeeded");
        check(collected.size() == 2, "collected exactly 2 rows");
        if (collected.size() == 2) {
            check(collected[0].id == 1 && collected[0].notional.has_value() && *collected[0].notional == 2.5 &&
                  collected[0].name.view() == "Alpha",
                  "row 1: id=1, notional=2.5, name=Alpha (by-name matching, non-declared column order)");
            check(collected[1].id == 2 && !collected[1].notional.has_value() && collected[1].name.view() == "Beta",
                  "row 2: notional correctly round-tripped as NULL (nullopt), not a stale value");
        }
    }

    std::printf("\n--- select() -- struct-free positional fetch ---\n");
    {
        long long count = 0;
        auto r = select(conn, "SELECT COUNT(*) FROM new_arch_client_test", count);
        check(r.status == ExecStatus::Success && count == 2, "SELECT COUNT(*) via select() == 2");
    }

    std::printf("\n--- insert_rows() -- array bind, FixedString<N> + scalar rows, a partial final chunk ---\n");
    {
        struct BulkRow { int id; FixedString<8> code; double notional; };
        std::vector<BulkRow> rows;
        for (int i = 10; i < 17; ++i) rows.push_back(BulkRow{i, FixedString<8>("C" + std::to_string(i)), i * 1.5});
        auto r = insert_rows(conn, "INSERT INTO new_arch_client_test(id, name, notional) VALUES(:id, :code, :notional)",
                             rows, 3); // 7 rows, chunk 3: 3+3+1
        check(r.status == ExecStatus::Success, "insert_rows() (3+3+1 chunks) succeeded");

        long long count = 0;
        select(conn, "SELECT COUNT(*) FROM new_arch_client_test", count);
        check(count == 9, "row count is 9 (2 earlier + 7 bulk)");

        // "code" is the struct field name here, matched by name against
        // the SELECT list -- so the query aliases the actual "name"
        // column to "code" for this fetch.
        struct CheckRow { int id; FixedString<8> code; double notional; };
        std::vector<CheckRow> last;
        select_rows<CheckRow>(conn, "SELECT id, name AS code, notional FROM new_arch_client_test WHERE id = 16", 1, 1,
            [&](const CheckRow* rows2, std::size_t n) { for (std::size_t i = 0; i < n; ++i) last.push_back(rows2[i]); });
        check(!last.empty() && last[0].code.view() == "C16" && last[0].notional == 24.0,
              "last row of the partial final chunk (id=16) has the correct FixedString + scalar value");
    }

    std::printf("\n--- OciClob field: execute() bind + select_rows() fetch, real locator round-trip ---\n");
    {
        struct InRow { int id; OciClob report; };
        InRow in{100, OciClob("FRTB sensitivities report body, row 100")};
        auto e = execute(conn, "INSERT INTO new_arch_client_test(id, report) VALUES(:id, :report)", in);
        check(e.status == ExecStatus::Success, "LOB insert via execute() succeeded");

        struct OutRow { int id; OciClob report; };
        std::vector<OutRow> collected;
        auto r = select_rows<OutRow>(conn, "SELECT id, report FROM new_arch_client_test WHERE id = 100", 1, 1,
            [&](const OutRow* rows, std::size_t n) { for (std::size_t i = 0; i < n; ++i) collected.push_back(rows[i]); });
        check(r.status == ExecStatus::Success, "LOB select_rows() succeeded");
        check(!collected.empty() && collected[0].report.text_data == "FRTB sensitivities report body, row 100",
              "LOB content round-tripped byte-for-byte through a real OCILob-backed locator");
    }

    std::printf("\n--- plain FixedString<N>: empty binds as NULL, NULL fetches as empty, no optional<> needed ---\n");
    {
        // Row 200: bound with an empty FixedString<N> -- Oracle can't
        // store '' for VARCHAR2, so this lands as a real NULL either way.
        // Row 201: name left out of the INSERT entirely (an explicit SQL
        // NULL), just to confirm the fetch side doesn't care which way
        // the NULL was produced.
        struct InRow { int id; FixedString<16> name; };
        InRow empty_name{200, FixedString<16>("")};
        auto e1 = execute(conn, "INSERT INTO new_arch_client_test(id, name) VALUES(:id, :name)", empty_name);
        check(e1.status == ExecStatus::Success, "insert with an empty (zero-length) FixedString<N> succeeded");
        auto e2 = execute(conn, "INSERT INTO new_arch_client_test(id) VALUES(201)");
        check(e2.status == ExecStatus::Success, "insert with name column omitted (SQL NULL) succeeded");

        // Row 202 has real content and is fetched FIRST (fetch_batch_size
        // 1, so each row gets its own OCIStmtFetch2 call reusing the same
        // one-row batch buffer) -- this is the actual bug this fix
        // closes: batch[0].name genuinely holds "Real" from the first
        // fetch() call, and the second call (row 201, NULL) reuses that
        // same buffer slot. A freshly-constructed, never-reused buffer
        // would pass this check even without the fix (FixedString<N>'s
        // own default constructor already zero-fills), so ordering
        // real-content-then-NULL with a small batch size is what makes
        // this a real test rather than a coincidental pass.
        auto e3 = execute(conn, "INSERT INTO new_arch_client_test(id, name) VALUES(202, 'Real')");
        check(e3.status == ExecStatus::Success, "insert row 202 with real content succeeded");

        struct OutRow { int id; FixedString<16> name; };
        std::vector<OutRow> rows;
        auto r = select_rows<OutRow>(conn,
            "SELECT id, name FROM new_arch_client_test WHERE id IN (200,201,202) ORDER BY id DESC", 1, 1,
            [&](const OutRow* rows2, std::size_t n) { for (std::size_t i = 0; i < n; ++i) rows.push_back(rows2[i]); });
        check(r.status == ExecStatus::Success, "select_rows() succeeded");
        check(rows.size() == 3, "collected exactly 3 rows");
        if (rows.size() == 3) {
            check(rows[0].name.view() == "Real", "row 202 (fetched first) has its real content");
            check(rows[1].name.view().empty(),
                  "row 201 (SQL NULL, fetched second into the SAME reused buffer) is empty, not stale 'Real'");
            check(rows[2].name.view().empty(), "row 200 (empty-string bind) fetched back as empty, not garbage");
        }
    }

    execute(conn, "DROP TABLE new_arch_client_test");

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
