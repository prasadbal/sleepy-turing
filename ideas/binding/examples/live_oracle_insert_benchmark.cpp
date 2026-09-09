// Benchmarks insert_rows()'s chunked array bind against a real Oracle
// database -- the write-side counterpart to live_oracle_fetch_benchmark.cpp.
// The two exist together specifically to show they do NOT behave the same
// way: the fetch side decouples round trips (prefetch_rows) from the
// client-side call granularity (fetch_batch_size); the insert side has no
// such decoupling; chunk_size directly is the round-trip granularity.
// See README's "Testing against a real database" for the numbers this
// produced and what they mean.
//
// A timing comparison, not a correctness demo -- compile with
// optimizations on, same as live_oracle_fetch_benchmark.cpp. Not part of
// the normal CMake build; see live_oracle_demo.cpp's header comment for
// the full compile command and Instant-Client-version notes (add -O2):
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_insert_benchmark.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_insert_benchmark
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_insert_benchmark <connect_string> <username> <password> [row_count]
//
// row_count defaults to 50,000, not live_oracle_fetch_benchmark.cpp's
// 500,000 -- deliberately smaller, because chunk_size=1 here means 50,000
// individual OCIStmtExecute round trips (there is no prefetch-style escape
// hatch on this side), and at 500,000 rows that one data point alone would
// dominate the whole benchmark's runtime for not much extra insight. The
// row struct itself is populated client-side (a plain loop, no database
// round trips) before any timing starts.

#include <chrono>
#include <cstdio>
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

struct RowCount {
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

long long read_row_count(binding::OciConnection& conn) {
    std::vector<RowCount> rows;
    std::function<void(const RowCount*, std::size_t)> on_batch =
        [&](const RowCount* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) rows.push_back(batch[i]);
        };
    binding::select_rows<RowCount>(conn, "SELECT COUNT(*) FROM bench_insert_test", 10, 10, on_batch);
    return rows.empty() ? -1 : rows[0].value;
}

double elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <connect_string> <username> <password> [row_count]\n", argv[0]);
        return 2;
    }
    const std::size_t row_count = (argc >= 5) ? static_cast<std::size_t>(std::stoul(argv[4])) : 50000;

    binding::OciConnection conn(argv[1], argv[2], argv[3]);
    if (!conn.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }
    std::printf("connected. row_count=%zu\n", row_count);

    binding::execute(conn, "DROP TABLE bench_insert_test"); // ignore failure: may not exist yet
    auto create = binding::execute(conn,
        "CREATE TABLE bench_insert_test (id NUMBER, value NUMBER, label VARCHAR2(16))");
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "CREATE TABLE failed\n");
        return 1;
    }

    std::vector<BenchRow> rows;
    rows.reserve(row_count);
    for (std::size_t i = 0; i < row_count; ++i) {
        rows.push_back(BenchRow{static_cast<int>(i), 1.5 * static_cast<double>(i),
                                 binding::FixedString<16>("ROW")});
    }
    std::printf("populated %zu rows client-side.\n\n", row_count);

    std::printf("--- chunk_size varies: this is the write side's whole story, no decoupling ---\n");
    std::printf("  %10s %12s %14s %12s\n", "chunk", "elapsed_ms", "rows/sec", "roundtrips");
    for (std::size_t chunk : {1u, 10u, 100u, 1000u, 10000u, 50000u}) {
        if (chunk > row_count) continue;
        binding::execute(conn, "TRUNCATE TABLE bench_insert_test");

        const long long rt_before = read_roundtrips(conn);
        const auto t0 = std::chrono::steady_clock::now();
        auto result = binding::insert_rows(conn,
            "INSERT INTO bench_insert_test VALUES(:id,:value,:label)", rows, chunk);
        const auto t1 = std::chrono::steady_clock::now();
        const long long rt_after = read_roundtrips(conn);

        const long long actual_count = read_row_count(conn);
        if (result.status != binding::ExecStatus::Success || actual_count != static_cast<long long>(row_count)) {
            std::printf("  FAIL chunk=%zu status=%d actual_count=%lld (expected %zu)\n",
                        chunk, (int)result.status, actual_count, row_count);
            continue;
        }

        const double ms = elapsed_ms(t0, t1);
        const double rows_per_sec = (ms > 0.0) ? (static_cast<double>(row_count) / (ms / 1000.0)) : 0.0;
        std::printf("  %10zu %12.1f %14.0f %12lld\n", chunk, ms, rows_per_sec, rt_after - rt_before);
    }

    binding::execute(conn, "DROP TABLE bench_insert_test");
    conn.disconnect();
    return 0;
}
