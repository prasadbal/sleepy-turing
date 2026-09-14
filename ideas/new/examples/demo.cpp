// Demo for the new call_oci/OciHandleGuard/OciStatement architecture
// (ideas/new) -- built against the mock OCI backend (binding/oci_mock.h,
// shared with ideas/binding) since there's no real Oracle client in this
// environment. See examples/live_oracle_demo.cpp for the real-database
// verification.
//
//   1. connect/disconnect using OCIHandleGuard-managed handles.
//   2. execute() with no bind -- DDL/literal DML.
//   3. Statement state checking: using fetch() before execute() throws
//      OciStatementStateError instead of making a nonsensical OCI call.
//   4. bindName() + execute() -- a single-row UPDATE-shaped bind.
//   5. bindOutput() + execute(iters=1) + rows_fetched() -- fetching a
//      single row's columns positionally, type-erased.
//   6. A batch fetch: bindOutput() with elemSize for array-of-struct,
//      execute(iters=0), then fetch() in a loop -- mirrors ideas/
//      binding's select_rows() shape, built from OciStatement's pieces
//      directly instead of a reflection-based free function.
//   7. Zero-row query: execute(iters=1) transitions straight to
//      EndOfFetch, classified Success (not a failure) -- oci_status is
//      OCI_NO_DATA, not something a caller has to treat as QueryError.
//   8. OCILob: create a temporary CLOB, write into it, read it back --
//      exercises the mock's own MockLobDescriptor round-trip (shared
//      with ideas/binding's oci_mock.h).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "binding/oci_connection.h"
#include "binding/oci_lob.h"
#include "binding/oci_log.h"
#include "binding/oci_statement.h"

using namespace binding;

namespace {
const char* status_name(ExecStatus s) {
    switch (s) {
        case ExecStatus::Success:        return "Success";
        case ExecStatus::ConnectionLost: return "ConnectionLost";
        case ExecStatus::QueryError:     return "QueryError";
    }
    return "?";
}
} // namespace

int main() {
    OciConnection conn("orcl", "app_user", "secret");
    if (!conn.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }
    std::printf("--- Demo 1: connect() via OCIHandleGuard-managed handles ---\n");
    std::printf("connected=%s\n\n", conn.connected() ? "true" : "false");

    std::printf("--- Demo 2: execute() with no bind -- DDL/literal DML ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("CREATE TABLE trades (trade_id NUMBER, notional NUMBER)");
        auto r = stmt.execute(1);
        std::printf("result=%s\n\n", status_name(r.status));
    }

    std::printf("--- Demo 3: state checking -- fetch() before execute() throws ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT trade_id FROM trades");
        try {
            stmt.fetch(10);
            std::printf("  FAIL: fetch() should have thrown\n");
        } catch (const OciStatementStateError& e) {
            std::printf("  OK: threw OciStatementStateError: %s\n", e.what());
        }
    }
    std::printf("\n");

    std::printf("--- Demo 4: bindName() -- single-row bind by name ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("UPDATE trades SET notional = :notional WHERE trade_id = :trade_id");
        int trade_id = 100;
        double notional = 42.5;
        stmt.bindName("notional", SQLT_BDOUBLE, &notional, sizeof(notional));
        stmt.bindName("trade_id", SQLT_INT, &trade_id, sizeof(trade_id));
        auto r = stmt.execute(1);
        std::printf("result=%s\n\n", status_name(r.status));
    }

    std::printf("--- Demo 5: bindOutput() -- single-row fetch, type-erased, positional ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT trade_id, notional FROM trades");
        int trade_id = 0;
        double notional = 0.0;
        stmt.bindOutput(1, SQLT_INT, &trade_id, sizeof(trade_id), nullptr, nullptr);
        stmt.bindOutput(2, SQLT_BDOUBLE, &notional, sizeof(notional), nullptr, nullptr);
        auto r = stmt.execute(1); // iters=1: fetches the first row as part of execute
        std::printf("result=%s trade_id=%d notional=%f rows_fetched=%u\n\n",
                    status_name(r.status), trade_id, notional, stmt.rows_fetched());
    }

    std::printf("--- Demo 6: batch fetch -- bindOutput() with elemSize, execute+fetch loop ---\n");
    {
        struct Row { int trade_id; double notional; };
        OciStatement stmt(conn);
        stmt.prepare("SELECT trade_id, notional FROM trades");
        stmt.set_prefetch_rows(100);

        std::vector<Row> batch(10);
        stmt.bindOutput(1, SQLT_INT, &batch[0].trade_id, sizeof(int), nullptr, nullptr, sizeof(Row));
        stmt.bindOutput(2, SQLT_BDOUBLE, &batch[0].notional, sizeof(double), nullptr, nullptr, sizeof(Row));

        auto exec_result = stmt.execute(0); // 0: nothing fetched yet, fetch loop does it all
        std::vector<Row> collected;
        if (exec_result.status == ExecStatus::Success) {
            for (;;) {
                auto fetch_result = stmt.fetch(static_cast<ub4>(batch.size()));
                if (fetch_result.status != ExecStatus::Success) break;
                const ub4 n = stmt.rows_fetched();
                for (ub4 i = 0; i < n; ++i) collected.push_back(batch[i]);
                // The fetch CALL's own status is the real stopping signal --
                // not stmt.state(), which (against a real database with
                // prefetching enabled) can already read EndOfFetch before
                // every prefetched row has actually been drained by fetch()
                // yet. See docs/oci_statement_lifecycle_notes.md.
                if (fetch_result.call.status == OCI_NO_DATA) break;
            }
        }
        std::printf("rows collected=%zu\n", collected.size());
        for (auto& row : collected) std::printf("  trade_id=%d notional=%f\n", row.trade_id, row.notional);
    }
    std::printf("\n");

    std::printf("--- Demo 7: zero-row query -- OCI_NO_DATA classified Success, not an error ---\n");
    {
        OciStatement stmt(conn);
        stmt.prepare("SELECT trade_id FROM empty_table");
        int trade_id = -1;
        stmt.bindOutput(1, SQLT_INT, &trade_id, sizeof(trade_id), nullptr, nullptr);
        binding::mock::g_fetch_row = binding::mock::MOCK_ROW_COUNT; // simulate an already-exhausted result set
        auto r = stmt.execute(1);
        std::printf("status=%s oci_status=%d (OCI_NO_DATA=%d) state=%s\n\n",
                    status_name(r.status), r.call.status, OCI_NO_DATA,
                    stmt.state() == OciStatement::State::EndOfFetch ? "EndOfFetch" : "other");
        binding::mock::g_fetch_row = 0; // reset for anything running after this demo
    }

    std::printf("--- Demo 8: OCILob -- create temporary, write, read back ---\n");
    {
        OCILob lob(conn);
        lob.create_temporary(OCI_TEMP_CLOB);
        const std::string value = "FRTB sensitivities report body";
        lob.write(value.data(), value.size());
        const std::string readback = lob.read(/*is_char_lob=*/true);
        std::printf("wrote %zu bytes, read back: [%s] (match=%s)\n",
                    value.size(), readback.c_str(), readback == value ? "true" : "false");
    }

    std::printf("\n--- Demo 9: set_statement_logger() ---\n");
    {
        std::vector<std::string> logged;
        set_statement_logger([&](std::string_view line) { logged.emplace_back(line); });

        OciStatement stmt(conn);
        stmt.prepare("UPDATE trades SET notional = :notional WHERE trade_id = :trade_id");
        int trade_id = 100;
        double notional = 42.5;
        sb2 null_indicator = OCI_IND_NULL;
        stmt.bindName("notional", SQLT_BDOUBLE, &notional, sizeof(notional));
        stmt.bindName("trade_id", SQLT_INT, &trade_id, sizeof(trade_id));
        stmt.execute(1);

        OciStatement stmt2(conn);
        stmt2.prepare("UPDATE trades SET notional = :notional WHERE trade_id = :trade_id");
        stmt2.bindName("notional", SQLT_BDOUBLE, &notional, sizeof(notional), &null_indicator);
        stmt2.bindName("trade_id", SQLT_INT, &trade_id, sizeof(trade_id));
        stmt2.execute(1);

        for (auto& line : logged) std::printf("  %s\n", line.c_str());
        set_statement_logger(nullptr);
    }

    std::printf("\n--- Demo 10: bindNameArray() -- chunked array-bind insert ---\n");
    {
        struct Row { int id; double notional; };
        std::vector<Row> rows = {{1, 1.5}, {2, 3.0}, {3, 4.5}, {4, 6.0}, {5, 7.5}};
        constexpr std::size_t chunk_size = 2; // 2+2+1: exercises a partial final chunk

        OciStatement stmt(conn);
        stmt.prepare("INSERT INTO trades VALUES(:id, :notional)");
        std::vector<sb2> indicators(chunk_size, OCI_IND_NOTNULL);

        std::size_t chunks_run = 0;
        for (std::size_t offset = 0; offset < rows.size(); offset += chunk_size) {
            const std::size_t this_chunk = std::min(chunk_size, rows.size() - offset);
            stmt.bindNameArray("id", SQLT_INT, &rows[offset].id, sizeof(int), sizeof(Row), indicators.data());
            stmt.bindNameArray("notional", SQLT_BDOUBLE, &rows[offset].notional, sizeof(double), sizeof(Row),
                               indicators.data());
            auto r = stmt.execute(static_cast<ub4>(this_chunk));
            std::printf("  chunk offset=%zu size=%zu status=%s state=%s\n", offset, this_chunk,
                        status_name(r.status), stmt.state() == OciStatement::State::Executed ? "Executed" : "other");
            ++chunks_run;
        }
        std::printf("ran %zu chunks over %zu rows, same prepared statement throughout\n", chunks_run, rows.size());
    }

    conn.disconnect();
    return 0;
}
