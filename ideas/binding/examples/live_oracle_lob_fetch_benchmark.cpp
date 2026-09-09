// Fetches ~1000 CLOBs of ~256KB each (XML-shaped payloads, the kind of
// thing an FRTB run might produce one per desk/risk-class) and verifies
// every one round-trips correctly -- the scale test for OciClob/OciBlob
// (see oci_lob.h and examples/live_oracle_lob_demo.cpp, which checks
// correctness at n=3 rows but says nothing about n=1000).
//
// The one thing this scale actually surfaces that n=3 can't: a LOB column
// fetches in *two* phases, not one -- OCIDefineByPos/OCIStmtFetch2 only
// ever transfers the locator (a small handle) for a LOB column, never the
// LOB's own bytes; the real content only moves over the wire later, inside
// apply_one_column's OCILobGetLength2 + OCILobRead2 calls (details/
// oci_client.h), one pair *per row, per LOB column*, regardless of
// fetch_batch_size or prefetch_rows. Those two knobs only ever governed the
// row-level (locator-only) fetch for every other type in this library --
// this is the first case where they visibly don't tell the whole story,
// and the round-trip count measured below (via the same V$SESSTAT query
// every other benchmark here uses) is what actually shows that, rather
// than asserting it from reading the code.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the full compile command and Instant-Client-version notes
// (add -O2, same as every other benchmark here):
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_lob_fetch_benchmark.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_lob_fetch_benchmark
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_lob_fetch_benchmark <connect_string> <username> <password>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_lob.h"

namespace {

constexpr int kRowCount = 1000;
constexpr std::size_t kLobSizeBytes = 256 * 1024;
// fetch_batch_size stays modest even though prefetch_rows (the row/locator
// side) could be large for free: each fetched row's OciClob::text_data
// holds a real 256KB in memory until on_batch/verification is done with
// it, and the batch buffer is reused (not accumulated) across calls -- so
// peak LOB memory here is fetch_batch_size * kLobSizeBytes, not
// kRowCount * kLobSizeBytes. Verification below deliberately checks each
// row inside on_batch rather than collecting all 1000 into a vector for
// the same reason (~250MB resident at once for content this library's own
// benchmarks have already treated as worth budgeting around -- see
// live_oracle_wide_row_benchmark.cpp's 256MB cap).
constexpr std::size_t kPrefetchRows = 200;
constexpr std::size_t kFetchBatchSize = 50;

struct ReportRow {
    int id;
    binding::OciClob xml_payload;
};

struct RoundtripCount {
    long long value;
};

// Deterministic given (id, target_size): called once to generate what gets
// inserted, and again per fetched row to check what came back -- no need
// to keep 1000 pre-generated ~256KB strings resident just to compare
// against later.
std::string generate_xml_payload(int id, std::size_t target_size) {
    const std::string prefix = "<Report id=\"" + std::to_string(id) +
        "\"><Header><Desk>RATES_LDN</Desk><RunDate>2026-09-09</RunDate></Header><Sensitivities>";
    const std::string suffix = "</Sensitivities></Report>";
    const std::string unit = "<Delta bucket=\"1\" tenor=\"3M\" value=\"0.000123\"/>";

    const std::size_t body_target =
        target_size > prefix.size() + suffix.size() ? target_size - prefix.size() - suffix.size() : 0;
    std::string body;
    body.reserve(body_target + unit.size());
    while (body.size() < body_target) body += unit;
    body.resize(body_target); // exact byte count, reproducible from (id, target_size) alone

    std::string result;
    result.reserve(target_size);
    result += prefix;
    result += body;
    result += suffix;
    return result;
}

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
    std::printf("connected. inserting %d rows of ~%zuKB each (~%.0fMB total)...\n",
                kRowCount, kLobSizeBytes / 1024, (kRowCount * kLobSizeBytes) / (1024.0 * 1024.0));

    binding::execute(conn, "DROP TABLE lob_fetch_bench"); // ignore failure: may not exist yet
    auto create = binding::execute(conn, "CREATE TABLE lob_fetch_bench (id NUMBER, xml_payload CLOB)");
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "CREATE TABLE failed\n");
        return 1;
    }

    const long long rt_before_insert = read_roundtrips(conn);
    const auto insert_start = std::chrono::steady_clock::now();
    for (int id = 0; id < kRowCount; ++id) {
        ReportRow row{id, binding::OciClob(generate_xml_payload(id, kLobSizeBytes))};
        auto r = binding::execute(conn, "INSERT INTO lob_fetch_bench VALUES(:id,:xml_payload)", row);
        if (r.status != binding::ExecStatus::Success) {
            std::fprintf(stderr, "insert failed at id=%d\n", id);
            return 1;
        }
    }
    const auto insert_end = std::chrono::steady_clock::now();
    const long long rt_after_insert = read_roundtrips(conn);
    const double insert_ms = elapsed_ms(insert_start, insert_end);
    std::printf("insert: %.1fms, %lld round trips (%.2f/row)\n\n",
                insert_ms, rt_after_insert - rt_before_insert,
                static_cast<double>(rt_after_insert - rt_before_insert) / kRowCount);

    std::printf("fetching %d rows back (prefetch_rows=%zu, fetch_batch_size=%zu), verifying each one...\n",
                kRowCount, kPrefetchRows, kFetchBatchSize);

    int rows_seen = 0;
    int mismatches = 0;
    std::function<void(const ReportRow*, std::size_t)> on_batch =
        [&](const ReportRow* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                const ReportRow& row = batch[i];
                const std::string expected = generate_xml_payload(row.id, kLobSizeBytes);
                if (row.xml_payload.text_data != expected) {
                    ++mismatches;
                    if (mismatches <= 5) {
                        std::fprintf(stderr, "  MISMATCH id=%d: got %zu bytes, expected %zu bytes\n",
                                     row.id, row.xml_payload.text_data.size(), expected.size());
                    }
                }
                ++rows_seen;
            }
        };

    const long long rt_before_fetch = read_roundtrips(conn);
    const auto fetch_start = std::chrono::steady_clock::now();
    auto result = binding::select_rows<ReportRow>(conn, "SELECT id, xml_payload FROM lob_fetch_bench ORDER BY id",
                                                   kPrefetchRows, kFetchBatchSize, on_batch);
    const auto fetch_end = std::chrono::steady_clock::now();
    const long long rt_after_fetch = read_roundtrips(conn);

    if (result.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "select_rows failed\n");
        return 1;
    }

    const double fetch_ms = elapsed_ms(fetch_start, fetch_end);
    const double total_mb = (rows_seen * kLobSizeBytes) / (1024.0 * 1024.0);
    const long long fetch_roundtrips = rt_after_fetch - rt_before_fetch;
    std::printf("\nfetch: %.1fms, %lld round trips (%.2f/row), %.1fMB, %.1fMB/s\n",
                fetch_ms, fetch_roundtrips, static_cast<double>(fetch_roundtrips) / kRowCount,
                total_mb, total_mb / (fetch_ms / 1000.0));

    std::printf("\nrows seen: %d, mismatches: %d -> %s\n",
                rows_seen, mismatches,
                (rows_seen == kRowCount && mismatches == 0) ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");

    binding::execute(conn, "DROP TABLE lob_fetch_bench");
    conn.disconnect();
    return (rows_seen == kRowCount && mismatches == 0) ? 0 : 1;
}
