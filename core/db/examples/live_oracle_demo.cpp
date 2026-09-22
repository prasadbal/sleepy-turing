// Real-database verification for the call_oci/OciHandleGuard/OciStatement/
// OCILob architecture -- the mock-based demo.cpp exercises the same call
// shapes, but only a real database proves the actual OCI semantics this
// design depends on: iters>0 fetching during execute, OCI_NO_DATA on a
// genuinely empty result, state violations being caught before a real
// (confusing) ORA-##### ever has a chance to occur, and a real LOB
// round-trip.
//
// Builds as the db_live_oracle_demo target. Against the OCI mock it compiles
// but cannot do anything useful -- configure with a real Oracle client first:
//
//   cmake --preset linux-release -DORACLE_OCI_INCLUDE_DIR=<INSTANT_CLIENT>/sdk/include -DORACLE_OCI_LIBRARY=<INSTANT_CLIENT>/libclntsh.so
//   cmake --build build/linux-release --target db_live_oracle_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> build/linux-release/core/db/db_live_oracle_demo <connect_string> <username> <password>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <db/oracle/oci_connection.h>
#include <db/oracle/oci_lob.h>
#include <db/oracle/oci_log.h>
#include <db/oracle/oci_statement.h>

using namespace marketlib::db::oracle;

namespace {
int g_failures = 0;
void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) ++g_failures;
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
                // The fetch CALL's own status is the real stopping signal,
                // not stmt.state(): with set_prefetch_rows() set above the
                // real row count, real Oracle reports OCI_ATTR_STMT_STATE
                // == END_OF_FETCH immediately after execute(), before this
                // loop's first fetch() call has run at all -- it means the
                // server has nothing more to send, not that this class has
                // drained everything already buffered client-side. Found
                // by this exact test crashing/under-collecting before this
                // fix -- see docs/oci_statement_lifecycle_notes.md.
                if (fetch_result.call.status == OCI_NO_DATA) break;
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

    std::printf("\n--- set_statement_logger(): real OciDateToText rendering ---\n");
    {
        OciStatement create(conn);
        create.prepare("CREATE TABLE new_arch_log_test (id NUMBER, cob_date DATE)");
        create.execute(1);

        std::vector<std::string> logged;
        set_statement_logger([&](std::string_view line) { logged.emplace_back(line); });

        OciStatement stmt(conn);
        stmt.prepare("INSERT INTO new_arch_log_test VALUES(:id, :cob_date)");
        int id = 1;
        ::OCIDate cob_date{};
        OCIDateFromText(conn.err(), reinterpret_cast<const text*>("13-SEP-26"), 9,
                        reinterpret_cast<const text*>("DD-MON-RR"), 9, nullptr, 0, &cob_date);
        stmt.bindName("id", SQLT_INT, &id, sizeof(id));
        stmt.bindName("cob_date", SQLT_ODT, &cob_date, sizeof(cob_date));
        stmt.execute(1);

        set_statement_logger(nullptr);
        check(logged.size() == 1, "exactly one log line for the INSERT");
        if (!logged.empty()) {
            std::printf("    %s\n", logged[0].c_str());
            check(logged[0].find("id=1") != std::string::npos, "id rendered correctly");
            check(logged[0].find("cob_date='13-SEP-26'") != std::string::npos,
                  "OciDate rendered via a real OCIDateToText call, not a placeholder");
        }

        OciStatement cleanup(conn);
        cleanup.prepare("DROP TABLE new_arch_log_test");
        cleanup.execute(1);
    }

    std::printf("\n--- describeColumnPosition(): by-name matching, reversed order + extra column ---\n");
    {
        OciStatement create(conn);
        create.prepare("CREATE TABLE new_arch_byname_test (id NUMBER, middle_extra NUMBER, name VARCHAR2(16))");
        create.execute(1);
        {
            OciStatement insert(conn);
            insert.prepare("INSERT INTO new_arch_byname_test VALUES(:id, :middle_extra, :name)");
            int id = 7;
            int extra = 999;
            char name_buf[17] = "SEVEN";
            insert.bindName("id", SQLT_INT, &id, sizeof(id));
            insert.bindName("middle_extra", SQLT_INT, &extra, sizeof(extra));
            insert.bindName("name", SQLT_CHR, name_buf, static_cast<sb4>(std::strlen(name_buf)));
            insert.execute(1);
        }

        // Query column order: id, middle_extra, name -- deliberately NOT
        // the order these get bound to output variables below, plus an
        // extra column (middle_extra) the caller doesn't even want.
        OciStatement select(conn);
        select.prepare("SELECT id, middle_extra, name FROM new_arch_byname_test WHERE id = 7");
        auto exec_result = select.execute(0); // iters=0: resolves describe info, no rows yet
        check(exec_result.status == ExecStatus::Success, "select execute() succeeded");

        // Bound in the OPPOSITE order from the SELECT list -- name first,
        // id second -- resolved purely by name, not position. name_len
        // is a real rlenp target (not nullptr): SQLT_CHR (VARCHAR2)
        // needs it to report the actual content length -- without it,
        // Oracle blank-pads the whole buffer to its declared size
        // instead of writing just the real value and leaving the rest
        // alone (confirmed here: omitting it the first time round
        // produced "SEVEN" + 11 trailing spaces, not "SEVEN\0...").
        char name_buf[17] = {};
        ub2 name_len = 0;
        int id = 0;
        ub4 name_pos = select.describeColumnPosition("name");
        ub4 id_pos = select.describeColumnPosition("id");
        check(name_pos == 3, "describeColumnPosition(\"name\") resolved to real position 3");
        check(id_pos == 1, "describeColumnPosition(\"id\") resolved to real position 1");
        select.bindOutput(name_pos, SQLT_CHR, name_buf, sizeof(name_buf) - 1, &name_len, nullptr);
        select.bindOutput(id_pos, SQLT_INT, &id, sizeof(id), nullptr, nullptr);

        auto fetch_result = select.fetch(1);
        check(fetch_result.status == ExecStatus::Success, "fetch() after post-execute bindOutput() succeeded");
        check(id == 7 && std::string(name_buf, name_len) == "SEVEN",
              "id=7, name=SEVEN correct despite reversed bind order + extra middle column");

        ub4 missing_pos = select.describeColumnPosition("totally_made_up_column");
        check(missing_pos == 0, "a column name with no match returns 0, not a wrong guess");

        OciStatement cleanup(conn);
        cleanup.prepare("DROP TABLE new_arch_byname_test");
        cleanup.execute(1);
    }

    std::printf("\n--- bindNameArray(): chunked array-bind insert ---\n");
    {
        struct Row { int id; double notional; };
        OciStatement create(conn);
        create.prepare("CREATE TABLE new_arch_bulk_test (id NUMBER, notional NUMBER)");
        create.execute(1);

        std::vector<Row> rows;
        for (int i = 1; i <= 7; ++i) rows.push_back(Row{i, i * 1.5});
        constexpr std::size_t chunk_size = 3; // 3+3+1: a real partial final chunk

        OciStatement insert(conn);
        insert.prepare("INSERT INTO new_arch_bulk_test VALUES(:id, :notional)");
        std::vector<sb2> indicators(chunk_size, OCI_IND_NOTNULL);

        bool all_chunks_ok = true;
        for (std::size_t offset = 0; offset < rows.size(); offset += chunk_size) {
            const std::size_t this_chunk = std::min(chunk_size, rows.size() - offset);
            insert.bindNameArray("id", SQLT_INT, &rows[offset].id, sizeof(int), sizeof(Row), indicators.data());
            insert.bindNameArray("notional", SQLT_BDOUBLE, &rows[offset].notional, sizeof(double), sizeof(Row),
                                 indicators.data());
            auto r = insert.execute(static_cast<ub4>(this_chunk));
            if (r.status != ExecStatus::Success) all_chunks_ok = false;
        }
        check(all_chunks_ok, "all 3 chunks (3+3+1) executed successfully, same prepared statement throughout");

        long long count = 0;
        OciStatement count_stmt(conn);
        count_stmt.prepare("SELECT COUNT(*) FROM new_arch_bulk_test");
        count_stmt.bindOutput(1, SQLT_INT, &count, sizeof(count), nullptr, nullptr);
        count_stmt.execute(1);
        check(count == 7, "all 7 rows actually landed in the table, not just 3 or a partial count");

        // Spot-check the row from the LAST chunk (offset=6, chunk of 1) --
        // exactly the case a stride/rebind bug would get wrong.
        int last_id = 0;
        double last_notional = 0.0;
        OciStatement spot_check(conn);
        spot_check.prepare("SELECT id, notional FROM new_arch_bulk_test WHERE id = 7");
        spot_check.bindOutput(1, SQLT_INT, &last_id, sizeof(last_id), nullptr, nullptr);
        spot_check.bindOutput(2, SQLT_BDOUBLE, &last_notional, sizeof(last_notional), nullptr, nullptr);
        spot_check.execute(1);
        check(last_id == 7 && last_notional == 10.5,
              "row 7 (from the final, partial chunk) has the correct value: id=7, notional=10.5");

        OciStatement cleanup(conn);
        cleanup.prepare("DROP TABLE new_arch_bulk_test");
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
