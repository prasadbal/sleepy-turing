// Benchmarks select_rows()'s fetch loop against a real Oracle database at
// real scale (500,000 rows by default) -- everything else in examples/ that
// touches real Oracle (live_oracle_demo.cpp, live_oracle_disconnect_demo.cpp)
// is a correctness check on a few dozen rows, not a performance measurement.
// This is a timing comparison, not a correctness demo, in the same spirit
// as lookup_benchmark.cpp -- compile with optimizations on regardless of
// anything else, or the numbers mostly measure debug-build overhead instead
// of what they're supposed to.
//
// Not part of the normal CMake build -- see live_oracle_demo.cpp's header
// comment for the full compile command and Instant-Client-version notes;
// this needs the exact same include/lib setup, just with -O2 added:
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_fetch_benchmark.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_fetch_benchmark
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_fetch_benchmark <connect_string> <username> <password> [row_count]
//
// Row generation avoids this library's own insert path entirely (which has
// no bulk array-insert -- see README's "What's deliberately not here" --
// and 500,000 individual round trips just to set up test data would dwarf
// the fetch benchmark it's supposed to set up for): one CREATE TABLE ... AS
// SELECT, generating rows from a Cartesian join of two small row sources
// rather than CONNECT BY LEVEL <= 500000, which is markedly slower at this
// size against a real optimizer.
//
// Round trips are read directly from the server's own accounting
// (V$SESSTAT's "SQL*Net roundtrips to/from client", scoped to this
// session), not inferred from configuration -- wall-clock time is noisy
// and machine-dependent; a server-reported round-trip count isn't.
//
// Three sweeps:
//   A. fetch_batch_size varies, prefetch_rows held large and fixed --
//      isolates client-side per-call overhead from network round trips.
//   B. prefetch_rows varies, fetch_batch_size held fixed -- isolates round
//      trips from everything else; re-verifies, at 1000x the row count,
//      the round-trip-collapse claim this design has relied on since the
//      pre-rewrite version of this file first measured it.
//   C. vector vs. std::map as the fetch destination, at a couple of
//      fetch_batch_size values -- the concrete answer to "does a small
//      batch cost less when the destination doesn't benefit from big
//      chunks anyway", which is why fetch_batch_size and prefetch_rows are
//      two independent parameters in the first place (see oci_client.h).

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_fixed_string.h"

struct BenchRow {
    int id;
    double value;
    binding::FixedString<16> label;
};

struct RoundtripCount {
    long long value;
};

namespace {

long long read_roundtrips(binding::OciConnection& conn) {
    std::vector<RoundtripCount> rows;
    std::function<void(const RoundtripCount*, std::size_t)> on_batch =
        [&](const RoundtripCount* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) rows.push_back(batch[i]);
        };
    binding::select_rows<RoundtripCount>(conn,
        "SELECT ss.VALUE FROM V$SESSTAT ss JOIN V$STATNAME sn ON ss.STATISTIC# = sn.STATISTIC# "
        "WHERE sn.NAME = 'SQL*Net roundtrips to/from client' "
        "AND ss.SID = SYS_CONTEXT('USERENV','SID')",
        10, 10, on_batch);
    return rows.empty() ? -1 : rows[0].value;
}

double elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Runs one fetch of the whole table and reports timing/round-trips. Only
// fills the destination container when fill_container is true -- sweeps A
// and B use false (they're about the fetch loop itself, not container
// insertion cost), sweep C uses true (that comparison is the whole point).
template <typename Container, typename Insert>
void run_one(binding::OciConnection& conn, std::size_t row_count, std::size_t prefetch_rows,
             std::size_t fetch_batch_size, bool fill_container, Container& dest, Insert insert) {
    dest.clear();
    std::size_t rows_seen = 0;
    std::function<void(const BenchRow*, std::size_t)> on_batch =
        [&](const BenchRow* batch, std::size_t count) {
            rows_seen += count;
            if (fill_container) {
                for (std::size_t i = 0; i < count; ++i) insert(dest, batch[i]);
            }
        };

    const long long rt_before = read_roundtrips(conn);
    const auto t0 = std::chrono::steady_clock::now();
    auto result = binding::select_rows<BenchRow>(
        conn, "SELECT id, value, label FROM bench_fetch_test ORDER BY id",
        prefetch_rows, fetch_batch_size, on_batch);
    const auto t1 = std::chrono::steady_clock::now();
    const long long rt_after = read_roundtrips(conn);

    if (result.status != binding::ExecStatus::Success || rows_seen != row_count) {
        std::printf("  FAIL prefetch=%zu batch=%zu status=%d rows_seen=%zu (expected %zu)\n",
                    prefetch_rows, fetch_batch_size, (int)result.status, rows_seen, row_count);
        return;
    }

    const double ms = elapsed_ms(t0, t1);
    const double rows_per_sec = (ms > 0.0) ? (static_cast<double>(row_count) / (ms / 1000.0)) : 0.0;
    std::printf("  %10zu %10zu %12.1f %14.0f %12lld\n",
                prefetch_rows, fetch_batch_size, ms, rows_per_sec, rt_after - rt_before);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <connect_string> <username> <password> [row_count]\n", argv[0]);
        return 2;
    }
    const std::size_t row_count = (argc >= 5) ? static_cast<std::size_t>(std::stoul(argv[4])) : 500000;

    binding::OciConnection conn(argv[1], argv[2], argv[3]);
    if (!conn.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }
    std::printf("connected. generating %zu rows server-side...\n", row_count);

    binding::execute(conn, "DROP TABLE bench_fetch_test"); // ignore failure: may not exist yet
    char create_sql[512];
    std::snprintf(create_sql, sizeof(create_sql),
        "CREATE TABLE bench_fetch_test AS "
        "SELECT rn AS id, rn * 1.5 AS value, 'ROW_' || TO_CHAR(MOD(rn, 1000)) AS label FROM ("
        "  SELECT ROWNUM AS rn FROM "
        "    (SELECT 1 FROM DUAL CONNECT BY LEVEL <= 1000) a, "
        "    (SELECT 1 FROM DUAL CONNECT BY LEVEL <= 1000) b "
        "  WHERE ROWNUM <= %zu"
        ")", row_count);
    const auto gen_t0 = std::chrono::steady_clock::now();
    auto create = binding::execute(conn, create_sql);
    const auto gen_t1 = std::chrono::steady_clock::now();
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "row generation failed (status=%d oci_status=%d)\n",
                     (int)create.status, (int)create.oci_status);
        return 1;
    }
    std::printf("generated in %.0f ms.\n\n", elapsed_ms(gen_t0, gen_t1));

    std::vector<BenchRow> vec_dest;
    auto vec_insert = [](std::vector<BenchRow>& v, const BenchRow& r) { v.push_back(r); };

    std::printf("--- Sweep A: fetch_batch_size varies, prefetch_rows fixed at 20000 ---\n");
    std::printf("  %10s %10s %12s %14s %12s\n", "prefetch", "batch", "elapsed_ms", "rows/sec", "roundtrips");
    for (std::size_t batch : {1u, 100u, 1000u, 5000u, 20000u}) {
        run_one(conn, row_count, 20000, batch, false, vec_dest, vec_insert);
    }

    std::printf("\n--- Sweep B: prefetch_rows varies, fetch_batch_size fixed at 1000 ---\n");
    std::printf("  %10s %10s %12s %14s %12s\n", "prefetch", "batch", "elapsed_ms", "rows/sec", "roundtrips");
    for (std::size_t prefetch : {50u, 500u, 5000u, 50000u}) {
        run_one(conn, row_count, prefetch, 1000, false, vec_dest, vec_insert);
    }

    std::printf("\n--- Sweep C: vector vs. std::map as the fetch destination, prefetch fixed at 20000 ---\n");
    std::printf("  %10s %10s %12s %14s %12s  (container)\n", "prefetch", "batch", "elapsed_ms", "rows/sec", "roundtrips");
    std::map<int, BenchRow> map_dest;
    auto map_insert = [](std::map<int, BenchRow>& m, const BenchRow& r) { m.emplace(r.id, r); };
    for (std::size_t batch : {10u, 1000u}) {
        std::printf(" vector:\n");
        run_one(conn, row_count, 20000, batch, true, vec_dest, vec_insert);
        std::printf(" map:\n");
        run_one(conn, row_count, 20000, batch, true, map_dest, map_insert);
    }

    binding::execute(conn, "DROP TABLE bench_fetch_test");
    conn.disconnect();
    return 0;
}
