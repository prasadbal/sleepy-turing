// A wide-row variant of live_oracle_insert_saturation_benchmark.cpp -- same
// methodology (chunk_size derived from a memory budget, total_rows swept),
// but with a ~3.9KB row instead of BenchRow's ~40 bytes. The row width change
// matters: at a 256MB budget, chunk_size (rows) comes out to roughly
// 68,000 -- comfortably *smaller* than some of the total_rows values tested
// below, unlike the narrow-row saturation benchmark, where a 100MB budget
// gave a chunk_size (~2.6M rows) larger than every total_rows tested, so
// every case ran as a single OCIStmtExecute call. Here, the larger
// total_rows cases genuinely chunk into several real calls -- this is the
// more realistic shape for an actual wide-row table (a row with many
// columns, or a few sizeable ones), where "how many rows fit in a bounded
// buffer" and "how many rows do I actually want to insert" are genuinely
// different numbers, not one always dwarfing the other.
//
// Row width is capped at 3900 bytes of payload, not the requested-ish 4080:
// VARCHAR2 has a hard 4000-byte ceiling by default (MAX_STRING_SIZE=STANDARD,
// the setting on this container, and Oracle's out-of-the-box default) --
// CREATE TABLE with VARCHAR2(4080) fails outright with ORA-00910 ("specified
// length too long for its datatype"), confirmed against the real container
// while building this. Getting past 4000 needs MAX_STRING_SIZE=EXTENDED (a
// one-way, instance-level migration) or a different type (CLOB), neither of
// which this benchmark needed -- 3900 gets close enough to "~4K rows" for
// what this is actually testing (buffer sizing math, not the exact byte
// count), so the smaller, always-valid number was the right call.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the full compile command and Instant-Client-version notes
// (add -O2, same as every other benchmark here):
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_wide_row_benchmark.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_wide_row_benchmark
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_wide_row_benchmark <connect_string> <username> <password> [budget_mb]
//
// budget_mb defaults to 256 (a "wouldn't allocate more than this" ceiling,
// not a target to fill). Edit kTotalRowsToTest directly to change which
// row counts get compared, same convention as the narrow-row version.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_fixed_string.h"

// ~3.9KB per row: a 3900-byte payload plus a 4-byte id and FixedString<N>'s
// own 2-byte length_ field puts sizeof(WideRow) a little under 3908 -- the
// exact number is read back from sizeof() at runtime below, not assumed,
// same as the narrow-row benchmark does with BenchRow. See the file header
// comment for why 3900 and not literally 4080/4096: VARCHAR2's 4000-byte
// default ceiling.
struct WideRow {
    int id;
    binding::FixedString<3900> payload;
};

struct RoundtripCount {
    long long value;
};

struct RowCount {
    long long value;
};

namespace {

// Edit this to compare different row counts. Chosen so at least one value
// sits below the ~65,536-row chunk_size a 256MB budget gives this row
// width (single chunk) and at least one sits above it (genuinely chunks
// into several OCIStmtExecute calls) -- see the file comment above for why
// that split is the point of this variant.
constexpr std::size_t kTotalRowsToTest[] = {20000, 100000, 300000};

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
    binding::select_rows<RowCount>(conn, "SELECT COUNT(*) FROM bench_wide_row_test", 10, 10, on_batch);
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
    const std::size_t budget_mb = (argc >= 5) ? static_cast<std::size_t>(std::stoul(argv[4])) : 256;
    const std::size_t kBufferBudgetBytes = budget_mb * 1024 * 1024;
    const std::size_t chunk_size = kBufferBudgetBytes / sizeof(WideRow);

    binding::OciConnection conn(argv[1], argv[2], argv[3]);
    if (!conn.connect()) {
        std::fprintf(stderr, "connect() failed\n");
        return 1;
    }
    std::printf("connected. sizeof(WideRow)=%zu, budget=%zuMB -> chunk_size=%zu rows "
                "(%.1fMB actually used per chunk)\n\n",
                sizeof(WideRow), budget_mb, chunk_size,
                (chunk_size * sizeof(WideRow)) / (1024.0 * 1024.0));

    binding::execute(conn, "DROP TABLE bench_wide_row_test"); // ignore failure: may not exist yet
    auto create = binding::execute(conn,
        "CREATE TABLE bench_wide_row_test (id NUMBER, payload VARCHAR2(3900))");
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "CREATE TABLE failed\n");
        return 1;
    }

    std::printf("chunk_size fixed at %zu rows (a %zuMB buffer budget, ~4KB/row) -- varying total_rows:\n",
                chunk_size, budget_mb);
    std::printf("  %12s %10s %12s %14s %12s\n", "total_rows", "chunks", "elapsed_ms", "rows/sec", "roundtrips");

    for (std::size_t total_rows : kTotalRowsToTest) {
        binding::execute(conn, "TRUNCATE TABLE bench_wide_row_test");

        std::vector<WideRow> rows;
        rows.reserve(total_rows);
        for (std::size_t i = 0; i < total_rows; ++i) {
            rows.push_back(WideRow{static_cast<int>(i), binding::FixedString<3900>("WIDE_ROW_PAYLOAD")});
        }

        const std::size_t expected_chunks = (total_rows + chunk_size - 1) / chunk_size;
        const long long rt_before = read_roundtrips(conn);
        const auto t0 = std::chrono::steady_clock::now();
        auto result = binding::insert_rows(conn,
            "INSERT INTO bench_wide_row_test VALUES(:id,:payload)", rows, chunk_size);
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

    binding::execute(conn, "DROP TABLE bench_wide_row_test");
    conn.disconnect();
    return 0;
}
