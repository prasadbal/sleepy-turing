// Live-database verification for OciClob/OciBlob (binding/oci_lob.h) --
// the one thing every other example in this directory sidesteps: a real
// insert-then-select-back round trip, since the mock (oci_mock.h) is a
// call-shape simulator that never actually stores what a bind wrote (see
// examples/main.cpp's Demo 9, which only confirms the LOB locator
// allocate/write/bind and allocate/define/read/free paths run without
// crashing against the mock -- not that the *value* survives a round trip).
//
// Three things this checks that a small/synthetic test wouldn't catch:
//   1. An ordinary short CLOB and BLOB value round-trip byte-for-byte.
//   2. A CLOB well past read_lob_bytes's single 8KB-ish comfort zone
//      (details/oci_client.h sizes its read buffer off OCILobGetLength2,
//      not a fixed chunk, but this is exactly the case that would expose
//      a buffer-sizing mistake if there were one) round-trips intact.
//   3. A BLOB carrying every byte value 0-255 (including embedded NULs,
//      which would truncate a naive C-string-based read) round-trips
//      intact -- binary data has no "safe" bytes to assume.
//   4. Multiple rows with LOB columns fetched in a single batch (not one
//      row at a time) -- select_rows()'s per-row locator array
//      (define_one_column's LOB branch, oci_client.h) actually has to
//      keep several rows' locators distinct, not just the one row every
//      other check here happens to also exercise.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the full compile command and Instant-Client-version notes:
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_lob_demo.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_lob_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_lob_demo <connect_string> <username> <password>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_lob.h"

struct ReportInsert {
    int report_id;
    binding::OciClob body;
    binding::OciBlob attachment;
};

struct ReportRow {
    int report_id;
    binding::OciClob body;
    binding::OciBlob attachment;
};

namespace {

std::string make_large_text(std::size_t n) {
    std::string s;
    s.reserve(n);
    const std::string unit = "FRTB sensitivities-based method delta/vega/curvature report line. ";
    while (s.size() < n) s += unit;
    s.resize(n);
    return s;
}

std::vector<unsigned char> make_all_byte_values(std::size_t repeats) {
    std::vector<unsigned char> v;
    v.reserve(256 * repeats);
    for (std::size_t r = 0; r < repeats; ++r) {
        for (int b = 0; b < 256; ++b) v.push_back(static_cast<unsigned char>(b));
    }
    return v;
}

bool check(bool cond, const char* what, int* failures) {
    std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) ++*failures;
    return cond;
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
    std::printf("connected.\n\n");

    binding::execute(conn, "DROP TABLE lob_demo_reports"); // ignore failure: may not exist yet
    auto create = binding::execute(conn,
        "CREATE TABLE lob_demo_reports (report_id NUMBER, body CLOB, attachment BLOB)");
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "CREATE TABLE failed\n");
        return 1;
    }

    const std::string short_text = "FRTB sensitivities report, desk RATES_LDN, run 2026-09-09";
    const std::vector<unsigned char> short_bytes = {0x01, 0x02, 0x03, 0xFF, 0x00, 0xAB};
    const std::string large_text = make_large_text(60000); // well past a single 8KB read chunk
    const std::vector<unsigned char> all_byte_values = make_all_byte_values(20); // 5120 bytes, every value 0-255, repeated

    std::printf("inserting 3 rows: short CLOB/BLOB, a %zu-byte CLOB, and a BLOB with every byte value 0-255...\n",
                large_text.size());
    ReportInsert row1{1, binding::OciClob(short_text), binding::OciBlob(short_bytes)};
    ReportInsert row2{2, binding::OciClob(large_text), binding::OciBlob(short_bytes)};
    ReportInsert row3{3, binding::OciClob(short_text), binding::OciBlob(all_byte_values)};
    binding::execute(conn, "INSERT INTO lob_demo_reports VALUES(:report_id,:body,:attachment)", row1);
    binding::execute(conn, "INSERT INTO lob_demo_reports VALUES(:report_id,:body,:attachment)", row2);
    binding::execute(conn, "INSERT INTO lob_demo_reports VALUES(:report_id,:body,:attachment)", row3);

    std::vector<ReportRow> rows;
    std::function<void(const ReportRow*, std::size_t)> on_batch =
        [&](const ReportRow* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) rows.push_back(batch[i]);
        };
    // fetch_batch_size=10 for 3 rows: one batch covers all of them, so this
    // also confirms the per-row locator array (not just a single locator)
    // works -- see the file header comment.
    auto result = binding::select_rows<ReportRow>(conn,
        "SELECT report_id, body, attachment FROM lob_demo_reports ORDER BY report_id", 10, 10, on_batch);

    if (result.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "select_rows failed\n");
        return 1;
    }

    std::printf("\nfetched %zu rows, checking round-trip:\n", rows.size());
    int failures = 0;
    check(rows.size() == 3, "fetched exactly 3 rows", &failures);
    if (rows.size() == 3) {
        check(rows[0].body.text_data == short_text, "row 1: short CLOB matches", &failures);
        check(rows[0].attachment.binary_data == short_bytes, "row 1: short BLOB matches", &failures);

        check(rows[1].body.text_data.size() == large_text.size(),
              ("row 2: large CLOB length matches (" + std::to_string(rows[1].body.text_data.size()) +
               " vs " + std::to_string(large_text.size()) + ")").c_str(), &failures);
        check(rows[1].body.text_data == large_text, "row 2: large CLOB content matches byte-for-byte", &failures);
        check(rows[1].attachment.binary_data == short_bytes, "row 2: short BLOB matches", &failures);

        check(rows[2].body.text_data == short_text, "row 3: short CLOB matches", &failures);
        check(rows[2].attachment.binary_data.size() == all_byte_values.size(),
              ("row 3: all-byte-values BLOB length matches (" + std::to_string(rows[2].attachment.binary_data.size()) +
               " vs " + std::to_string(all_byte_values.size()) + ")").c_str(), &failures);
        check(rows[2].attachment.binary_data == all_byte_values,
              "row 3: all-byte-values BLOB matches byte-for-byte (including embedded 0x00)", &failures);
    }

    std::printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");

    // Also confirm QueryError still classifies correctly with a LOB-bearing
    // struct in play (bad table name -> QueryError, not a crash).
    std::vector<ReportRow> dummy_rows;
    std::function<void(const ReportRow*, std::size_t)> dummy_batch =
        [&](const ReportRow* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) dummy_rows.push_back(batch[i]);
        };
    auto bad = binding::select_rows<ReportRow>(conn, "SELECT report_id, body, attachment FROM no_such_table",
                                                10, 10, dummy_batch);
    check(bad.status == binding::ExecStatus::QueryError, "bad table name -> QueryError, not a crash", &failures);

    binding::execute(conn, "DROP TABLE lob_demo_reports");
    conn.disconnect();
    return failures == 0 ? 0 : 1;
}
