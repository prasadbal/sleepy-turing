// Measures whether an std::optional<U> field's staging/copy path
// (define_t<T>'s per-field side array, apply_one_column's copy into the
// row) costs anything real, versus a plain field that OCI writes straight
// into the row struct -- the question raised while discussing why
// define_t<T> exists at all instead of one contiguous vector<T>.
//
// Two otherwise-identical row types, fetched from the same table:
//   PlainRow    { int id; double value; double extra; }
//   OptionalRow { int id; double value; std::optional<double> extra; }
// `extra` is never actually NULL in this run (every row has a real value)
// -- deliberately the *strictest* test of the mechanism's overhead: if
// going through define_t<T>'s staging vector and a copy costs nothing
// measurable even when the value is present on every single row (the
// common case), it certainly costs nothing when some rows are genuinely
// NULL, which only makes the plain path's job easier, not the optional
// path's job harder.
//
// prefetch_rows is fixed large (20000) across every run -- large enough
// that round_trips ~= total_rows / prefetch_rows regardless of
// fetch_batch_size, per the round_trips ~= total_rows / max(prefetch_rows,
// fetch_batch_size) model in README.md -- so every (row type,
// fetch_batch_size) combination below does the *same* number of round
// trips. Any timing difference is therefore client-side batch-processing
// cost, not network/round-trip cost -- exactly what's in question.
// fetch_batch_size=32 is tested directly since that's the specific small
// value proposed in discussion; 100 and 1000 are included for a wider
// picture against values used elsewhere in this directory's benchmarks.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the full compile command and Instant-Client-version notes
// (add -O2, same as every other benchmark here):
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_optional_fetch_benchmark.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_optional_fetch_benchmark
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_optional_fetch_benchmark <connect_string> <username> <password>

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"

namespace {

constexpr int kTotalRows = 500000;
constexpr std::size_t kPrefetchRows = 20000; // fixed, larger than every fetch_batch_size tested below
constexpr std::size_t kFetchBatchSizes[] = {32, 100, 1000, 20000};

struct PlainRow {
    int id;
    double value;
    double extra;
};

struct OptionalRow {
    int id;
    double value;
    std::optional<double> extra;
};

struct RoundtripCount {
    long long value;
};

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

template <typename RowT>
void run_one(binding::OciConnection& conn, const char* label, std::size_t fetch_batch_size, long long expected_sum_id) {
    long long rows_seen = 0;
    long long sum_id = 0;
    std::function<void(const RowT*, std::size_t)> on_batch =
        [&](const RowT* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                sum_id += batch[i].id;
                ++rows_seen;
            }
        };

    const long long rt_before = read_roundtrips(conn);
    const auto t0 = std::chrono::steady_clock::now();
    auto result = binding::select_rows<RowT>(conn, "SELECT id, value, extra FROM optional_fetch_bench ORDER BY id",
                                              kPrefetchRows, fetch_batch_size, on_batch);
    const auto t1 = std::chrono::steady_clock::now();
    const long long rt_after = read_roundtrips(conn);

    if (result.status != binding::ExecStatus::Success || rows_seen != kTotalRows || sum_id != expected_sum_id) {
        std::printf("  %-10s batch=%5zu FAIL (status=%d rows_seen=%lld sum_id=%lld expected=%lld)\n",
                    label, fetch_batch_size, (int)result.status, rows_seen, sum_id, expected_sum_id);
        return;
    }

    const double ms = elapsed_ms(t0, t1);
    const double rows_per_sec = (ms > 0.0) ? (kTotalRows / (ms / 1000.0)) : 0.0;
    std::printf("  %-10s batch=%5zu %10.1fms %14.0f rows/s  roundtrips=%lld\n",
                label, fetch_batch_size, ms, rows_per_sec, rt_after - rt_before);
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
    std::printf("connected. building a %d-row table (no CLOB this time -- pure scalar comparison)...\n", kTotalRows);

    binding::execute(conn, "DROP TABLE optional_fetch_bench"); // ignore failure: may not exist yet
    auto create = binding::execute(conn,
        "CREATE TABLE optional_fetch_bench AS "
        "SELECT ROWNUM AS id, ROWNUM * 1.5 AS value, ROWNUM * 2.5 AS extra "
        "FROM (SELECT 1 FROM dual CONNECT BY LEVEL <= 1000) a, "
        "     (SELECT 1 FROM dual CONNECT BY LEVEL <= 500) b "
        "WHERE ROWNUM <= " + std::to_string(kTotalRows));
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "CREATE TABLE failed\n");
        return 1;
    }

    // sum(1..kTotalRows) -- used to verify every row was actually seen and
    // in the right order, not just that the row count matched.
    const long long expected_sum_id = static_cast<long long>(kTotalRows) * (kTotalRows + 1) / 2;

    std::printf("\nprefetch_rows=%zu (fixed, larger than every fetch_batch_size below --\n"
                "round trips should come out identical across every row within one column):\n\n",
                kPrefetchRows);
    std::printf("  %-10s %-11s %12s %16s  %s\n", "row type", "batch", "elapsed", "throughput", "");

    for (std::size_t batch : kFetchBatchSizes) {
        run_one<PlainRow>(conn, "PlainRow", batch, expected_sum_id);
        run_one<OptionalRow>(conn, "Optional", batch, expected_sum_id);
        std::printf("\n");
    }

    binding::execute(conn, "DROP TABLE optional_fetch_bench");
    conn.disconnect();
    return 0;
}
