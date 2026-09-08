// Exercises oci_client.h/oci_connection.h against a REAL Oracle database --
// everything else in examples/ runs against the mock (binding/oci_mock.h)
// since there's no Oracle client in the default build environment. This
// file needs one, and is not part of the normal CMake build for that
// reason -- compile and run it directly (one command, no line breaks):
//
//   g++ -std=c++20 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_demo.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_demo <connect_string> <username> <password>
//
// -rpath-link is required, not optional -- without it the link step fails
// on undefined references to internal Oracle symbols that actually live in
// libclntsh.so's own dependencies (libclntshcore.so, libnnz.so, libaio.so.1),
// which the linker won't locate via -L alone.
//
// Verified against gvenzl/oracle-free:23 (docker container "oracle-free",
// ORACLE_PASSWORD=BindingTest123, port 1521, service FREEPDB1) with Oracle
// Instant Client 19.32 (basic + SDK). A second Instant Client install on
// this machine, labeled 23.1, failed two different ways -- OCIEnvCreate
// itself failed until ORACLE_HOME was set to it (its directory layout looks
// like a partial database install, not a standard Instant Client extraction:
// it ships oracore/rdbms/network subdirectories a real Instant Client
// doesn't), and even with that fixed, OCILogon2 failed with ORA-28041
// ("authentication protocol internal error") -- an authentication-protocol
// mismatch against this server version. Instant Client 19 needed neither
// workaround and connected cleanly on the first attempt. If you hit either
// symptom against a different client install, that's the first thing to
// check.
//
// Drops and recreates its own table, inserts a batch of rows one at a
// time (this design has no bulk array-insert -- see README's "What's
// deliberately not here"), then fetches them back in batches smaller than
// the total row count, so the array-fetch path genuinely runs more than
// once against a real server, and verifies every value against what was
// inserted rather than just printing it for a human to eyeball.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_datetime.h"
#include "binding/oci_fixed_string.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

} // namespace

struct LiveRow {
    int id;
    std::optional<double> amount;
    binding::FixedString<16> desk;
    binding::OciDate cob_date;
    std::optional<binding::FixedString<8>> risk_class;
    std::optional<binding::OciDate> maturity_date;
};

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <connect_string> <username> <password>\n", argv[0]);
        return 2;
    }
    binding::OciConnection conn(argv[1], argv[2], argv[3]);
    if (!conn.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }
    std::printf("connected.\n");

    binding::execute(conn, "DROP TABLE binding_live_test"); // ignore failure: may not exist yet
    auto create = binding::execute(conn, "CREATE TABLE binding_live_test ("
                                          "id NUMBER, amount NUMBER, desk VARCHAR2(16), "
                                          "cob_date DATE, risk_class VARCHAR2(8), maturity_date DATE)");
    check(create.status == binding::ExecStatus::Success, "CREATE TABLE");

    constexpr int kRowCount = 37; // deliberately not a multiple of the fetch batch size below
    std::vector<LiveRow> inserted;
    for (int i = 0; i < kRowCount; ++i) {
        LiveRow row{};
        row.id = i;
        row.amount = (i % 5 == 0) ? std::nullopt : std::make_optional(100.5 + i);
        row.desk = binding::FixedString<16>(i % 2 == 0 ? "RATES_LDN" : "FX_NEWYORK_DESK"); // second is 15 chars, fits 16
        row.cob_date = binding::OciDate(2026, 1 + (i % 12), 1 + (i % 28));
        row.risk_class = (i % 3 == 0) ? std::nullopt : std::make_optional(binding::FixedString<8>("IR"));
        row.maturity_date = (i % 4 == 0) ? std::nullopt : std::make_optional(binding::OciDate(2030, 1, 1 + (i % 28)));
        auto r = binding::execute(conn,
            "INSERT INTO binding_live_test VALUES(:id,:amount,:desk,:cob_date,:risk_class,:maturity_date)", row);
        if (r.status != binding::ExecStatus::Success) {
            std::printf("  FAIL insert row %d (status=%d oci_status=%d)\n", i, (int)r.status, (int)r.oci_status);
            ++g_failures;
        }
        inserted.push_back(row);
    }
    std::printf("inserted %d rows one at a time.\n", kRowCount);

    std::vector<LiveRow> fetched;
    int batch_count = 0;
    std::function<void(const LiveRow*, std::size_t)> on_batch =
        [&](const LiveRow* batch, std::size_t count) {
            ++batch_count;
            for (std::size_t i = 0; i < count; ++i) fetched.push_back(batch[i]);
        };
    // fetch_batch_size=10 against 37 rows: exercises 4 real fetch calls
    // against the server, the last one partial.
    auto select = binding::select_rows<LiveRow>(
        conn, "SELECT id, amount, desk, cob_date, risk_class, maturity_date FROM binding_live_test ORDER BY id",
        /*prefetch_rows=*/50, /*fetch_batch_size=*/10, on_batch);
    check(select.status == binding::ExecStatus::Success, "SELECT (batched fetch)");
    check(batch_count == 4, "fetch happened in exactly 4 batches (10+10+10+7)");
    check(fetched.size() == inserted.size(), "row count round-trips");

    bool all_match = true;
    for (std::size_t i = 0; i < fetched.size() && i < inserted.size(); ++i) {
        const auto& a = inserted[i];
        const auto& b = fetched[i];
        const bool amount_ok = a.amount.has_value() == b.amount.has_value() &&
                                (!a.amount || *a.amount == *b.amount);
        const bool desk_ok = a.desk.str() == b.desk.str();
        const bool cob_ok = a.cob_date == b.cob_date;
        const bool risk_ok = a.risk_class.has_value() == b.risk_class.has_value() &&
                              (!a.risk_class || a.risk_class->str() == b.risk_class->str());
        const bool maturity_ok = a.maturity_date.has_value() == b.maturity_date.has_value() &&
                                  (!a.maturity_date || *a.maturity_date == *b.maturity_date);
        if (!(amount_ok && desk_ok && cob_ok && risk_ok && maturity_ok)) {
            std::printf("  FAIL row %d mismatch (amount=%d desk=%d cob=%d risk=%d maturity=%d)\n",
                        (int)i, amount_ok, desk_ok, cob_ok, risk_ok, maturity_ok);
            all_match = false;
        }
    }
    check(all_match, "every fetched row matches what was inserted (scalars, FixedString, OciDate, NULLs)");

    std::printf("\n--- failure classification against a real server ---\n");
    auto bad_sql = binding::execute(conn, "INSERT INTO binding_live_test VALUES(:not_a_placeholder_match)");
    check(bad_sql.status == binding::ExecStatus::QueryError, "malformed statement -> QueryError");

    binding::execute(conn, "DROP TABLE binding_live_test");
    conn.disconnect();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
