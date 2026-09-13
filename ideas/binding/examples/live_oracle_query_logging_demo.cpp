// Verifies set_query_logger() against a real database: OciDate rendering
// via the real OCIDateToText path (not the mock's format interpreter),
// LOB values rendered as a byte count rather than dumped content, and
// insert_rows()'s array-bind path logging only the SQL text and row
// count, never per-row values.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the compile command and Instant-Client-version notes.
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_query_logging_demo.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_query_logging_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_query_logging_demo <connect_string> <username> <password>

#include <cstdio>
#include <string>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_datetime.h"
#include "binding/oci_lob.h"

struct ReportInsert {
    int report_id;
    binding::OciDate run_date;
    binding::OciClob body;
};

struct BulkRow {
    int id;
    double value;
};

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

    std::vector<std::string> logged;
    binding::set_query_logger([&](std::string_view line) { logged.emplace_back(line); });

    binding::execute(conn, "DROP TABLE query_logging_test");
    binding::execute(conn, "CREATE TABLE query_logging_test (report_id NUMBER, run_date DATE, body CLOB)");

    ReportInsert report{1, binding::OciDate(2026, 9, 13),
                         binding::OciClob(std::string(500, 'x'))}; // 500-byte CLOB
    binding::execute(conn, "INSERT INTO query_logging_test VALUES(:report_id,:run_date,:body)", report);

    std::vector<BulkRow> rows;
    for (int i = 0; i < 1000; ++i) rows.push_back(BulkRow{i, i * 1.5});
    binding::execute(conn, "CREATE TABLE query_logging_bulk (id NUMBER, value NUMBER)");
    binding::insert_rows(conn, "INSERT INTO query_logging_bulk VALUES(:id,:value)", rows, 100);

    binding::set_query_logger(nullptr);

    std::printf("logged %zu lines:\n", logged.size());
    for (auto& line : logged) std::printf("  %s\n", line.c_str());

    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
        if (!cond) ++failures;
    };

    // DROP/CREATE (x2) + the params INSERT + the bulk-table CREATE + the
    // array-bind INSERT -- 5 lines total, not 1000+ despite inserting 1000
    // rows via insert_rows().
    check(logged.size() == 5, "exactly 5 log lines, not 1000+ despite the 1000-row insert_rows() call");
    if (logged.size() >= 3) {
        check(logged[2].find("run_date='13-SEP-26'") != std::string::npos,
              "OciDate rendered via real OCIDateToText, not the mock's format interpreter");
        check(logged[2].find("<CLOB, 500 bytes>") != std::string::npos,
              "CLOB rendered as a byte count, not its 500 bytes of content");
    }
    if (logged.size() >= 5) {
        check(logged[4].find("1000 rows, values omitted") != std::string::npos,
              "insert_rows() logged row count with values omitted, not any of the 1000 rows' data");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");

    binding::execute(conn, "DROP TABLE query_logging_test");
    binding::execute(conn, "DROP TABLE query_logging_bulk");
    conn.disconnect();
    return failures == 0 ? 0 : 1;
}
