// Isolates the write side's per-row server cost from its round-trip cost --
// the two terms in the model discussed alongside this benchmark:
//
//   elapsed_time ~= round_trips * per_round_trip_overhead
//                 + total_rows  * per_row_server_cost
//
// live_oracle_insert_benchmark.cpp holds total_rows fixed and varies
// chunk_size, which moves round_trips while per_row_server_cost * total_rows
// stays constant -- that's what shows chunk_size mattering. This file does
// the opposite: holds chunk_size fixed at a size derived from a memory
// budget (see kBufferBudgetBytes below), not an arbitrary row count, and
// varies total_rows instead.
//
// The point of sizing chunk_size from a memory budget rather than picking a
// round number: it's the more meaningful, portable way to choose it in a
// real system, where what you actually have is a fixed amount of memory
// you're willing to spend on a bind buffer, and row width varies by row
// type -- "50,000 rows" doesn't mean anything until you know how wide a row
// is; "100MB" does. It also has a convenient side effect for *this*
// benchmark specifically: at 100MB, chunk_size (rows) comes out well above
// every total_rows value tested below (BenchRow is a handful of scalars --
// tens of bytes, not kilobytes), so every case runs as a single
// OCIStmtExecute call. Round trips stay ~1 across all three, which isolates
// per_row_server_cost cleanly -- there's no round-trip term left to vary
// and confound the comparison.
//
// What to look for in the output: if rows/sec comes out roughly the same
// across all three total_rows values, that constant IS the server's
// genuine sustained insert throughput ceiling for this row shape --
// independent of chunk_size, prefetch, or anything else this library
// controls. If rows/sec instead keeps changing with total_rows even though
// round trips don't, something other than "round trips" or "steady-state
// per-row cost" is moving (buffer cache warm-up, undo/redo segment growth
// over a bigger single transaction, checkpoint activity, ...) and is worth
// its own investigation.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the full compile command and Instant-Client-version notes
// (add -O2, same as every other benchmark here):
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_insert_saturation_benchmark.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_insert_saturation_benchmark
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_insert_saturation_benchmark <connect_string> <username> <password> [budget_mb]
//
// budget_mb defaults to 100. Edit kTotalRowsToTest directly to change which
// row counts get compared -- kept as a fixed array rather than CLI args
// since this is meant to be adapted at the call site, not parameterized
// generically.

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

// Edit this to compare different row counts -- see the file comment above
// for why this is a fixed array to edit, not a CLI parameter.
constexpr std::size_t kTotalRowsToTest[] = {50000, 200000, 500000};

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
    binding::select_rows<RowCount>(conn, "SELECT COUNT(*) FROM bench_saturation_test", 10, 10, on_batch);
    return rows.empty() ? -1 : rows[0].value;
}

double elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <connect_string> <username> <password> [budget_mb]\n", argv[0]);
        return 2;
    }
    const std::size_t budget_mb = (argc >= 5) ? static_cast<std::size_t>(std::stoul(argv[4])) : 100;
    const std::size_t kBufferBudgetBytes = budget_mb * 1024 * 1024;
    const std::size_t chunk_size = kBufferBudgetBytes / sizeof(BenchRow);

    binding::OciConnection conn(argv[1], argv[2], argv[3]);
    if (!conn.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }
    std::printf("connected. sizeof(BenchRow)=%zu, budget=%zuMB -> chunk_size=%zu rows\n\n",
                sizeof(BenchRow), budget_mb, chunk_size);

    binding::execute(conn, "DROP TABLE bench_saturation_test"); // ignore failure: may not exist yet
    auto create = binding::execute(conn,
        "CREATE TABLE bench_saturation_test (id NUMBER, value NUMBER, label VARCHAR2(16))");
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "CREATE TABLE failed\n");
        return 1;
    }

    std::printf("chunk_size fixed at %zu rows (a %zuMB buffer budget) -- varying total_rows only:\n", chunk_size, budget_mb);
    std::printf("  %12s %10s %12s %14s %12s\n", "total_rows", "chunks", "elapsed_ms", "rows/sec", "roundtrips");

    for (std::size_t total_rows : kTotalRowsToTest) {
        binding::execute(conn, "TRUNCATE TABLE bench_saturation_test");

        std::vector<BenchRow> rows;
        rows.reserve(total_rows);
        for (std::size_t i = 0; i < total_rows; ++i) {
            rows.push_back(BenchRow{static_cast<int>(i), 1.5 * static_cast<double>(i),
                                     binding::FixedString<16>("ROW")});
        }

        const std::size_t expected_chunks = (total_rows + chunk_size - 1) / chunk_size;
        const long long rt_before = read_roundtrips(conn);
        const auto t0 = std::chrono::steady_clock::now();
        auto result = binding::insert_rows(conn,
            "INSERT INTO bench_saturation_test VALUES(:id,:value,:label)", rows, chunk_size);
        const auto t1 = std::chrono::steady_clock::now();
        const long long rt_after = read_roundtrips(conn);

        const long long actual_count = read_row_count(conn);
        if (result.status != binding::ExecStatus::Success || actual_count != static_cast<long long>(total_rows)) {
            std::printf("  FAIL total_rows=%zu status=%d actual_count=%lld (expected %zu)\n",
                        total_rows, (int)result.status, actual_count, total_rows);
            continue;
        }

        const double ms = elapsed_ms(t0, t1);
        const double rows_per_sec = (ms > 0.0) ? (static_cast<double>(total_rows) / (ms / 1000.0)) : 0.0;
        std::printf("  %12zu %10zu %12.1f %14.0f %12lld\n",
                    total_rows, expected_chunks, ms, rows_per_sec, rt_after - rt_before);
    }

    std::printf("\nIf rows/sec above is roughly constant across all three rows, that's the\n"
                "server's steady-state insert throughput ceiling for this row shape --\n"
                "independent of chunk_size, since round trips stayed ~%zu the whole time.\n",
                (kTotalRowsToTest[0] + chunk_size - 1) / chunk_size);

    binding::execute(conn, "DROP TABLE bench_saturation_test");
    conn.disconnect();
    return 0;
}
