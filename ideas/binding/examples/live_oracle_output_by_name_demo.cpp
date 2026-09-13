// Proves select_rows()'s column matching is genuinely by name now, not by
// declaration order -- see resolve_column_positions in details/
// oci_client.h. Two things a positional scheme could never get right:
//
//   1. A struct whose field order does NOT match the SELECT list's column
//      order at all (reversed, in this test).
//   2. A SELECT list with an *extra* column the struct doesn't care about,
//      sitting in between the two columns the struct actually wants.
//
// If matching were still position-based, this would either silently read
// the wrong column into the wrong field, or fetch garbage from the extra
// middle column -- there is no positional arrangement that makes this
// query correct against MismatchedRow's declared field order. Passing
// here is only possible if the column name (from OCIParamGet/
// OCIAttrGet(OCI_ATTR_NAME)), not position, decided where each field's
// data came from.
//
// Not part of the normal CMake build; see live_oracle_demo.cpp's header
// comment for the compile command and Instant-Client-version notes.
//
//   g++ -std=c++20 -O2 -I <repo>/ideas/binding/include -I <BOOST_ROOT>
//       -I <INSTANT_CLIENT>/sdk/include ideas/binding/examples/live_oracle_output_by_name_demo.cpp
//       -L <INSTANT_CLIENT> -Wl,-rpath-link,<INSTANT_CLIENT> -Wl,-rpath,<INSTANT_CLIENT>
//       -lclntsh -o live_oracle_output_by_name_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> ./live_oracle_output_by_name_demo <connect_string> <username> <password>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"

// Declared in the OPPOSITE order from the SELECT list below on purpose:
// the query selects (id, middle_extra, name) but this struct declares
// (name, id) -- position 1 in the struct is "name", but "name" is
// actually column 3 in the query. A positional scheme would read column
// 1 (id, a NUMBER) into `name` (a FixedString) and column 2
// (middle_extra) into `id` -- both wrong types, both wrong columns.
struct MismatchedRow {
    binding::FixedString<16> name;
    int id;
};

namespace {
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
    std::printf("connected.\n");

    binding::execute(conn, "DROP TABLE output_by_name_test"); // ignore failure: may not exist yet
    auto create = binding::execute(conn,
        "CREATE TABLE output_by_name_test (id NUMBER, name VARCHAR2(16), middle_extra NUMBER)");
    if (create.status != binding::ExecStatus::Success) {
        std::fprintf(stderr, "CREATE TABLE failed\n");
        return 1;
    }
    binding::execute(conn, "INSERT INTO output_by_name_test VALUES (7, 'SEVEN', 999)");
    binding::execute(conn, "INSERT INTO output_by_name_test VALUES (8, 'EIGHT', 888)");

    std::vector<MismatchedRow> rows;
    std::function<void(const MismatchedRow*, std::size_t)> on_batch =
        [&](const MismatchedRow* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) rows.push_back(batch[i]);
        };

    // Column order: id, middle_extra, name -- none of it matches
    // MismatchedRow's declared order (name, id), and middle_extra isn't
    // in the struct at all.
    auto result = binding::select_rows<MismatchedRow>(conn,
        "SELECT id, middle_extra, name FROM output_by_name_test ORDER BY id", 10, 10, on_batch);

    int failures = 0;
    check(result.status == binding::ExecStatus::Success, "select_rows succeeded", &failures);
    check(rows.size() == 2, "fetched exactly 2 rows", &failures);
    if (rows.size() == 2) {
        check(rows[0].id == 7 && rows[0].name.str() == "SEVEN",
              "row 1: id=7, name=SEVEN despite reversed field order + extra middle column", &failures);
        check(rows[1].id == 8 && rows[1].name.str() == "EIGHT",
              "row 2: id=8, name=EIGHT despite reversed field order + extra middle column", &failures);
    }

    // A field with no matching column at all must fail loudly, not
    // silently grab whatever's at some position.
    struct NoSuchColumnRow { int id; double totally_made_up_field; };
    std::vector<NoSuchColumnRow> bad_rows;
    std::function<void(const NoSuchColumnRow*, std::size_t)> bad_batch =
        [&](const NoSuchColumnRow* batch, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) bad_rows.push_back(batch[i]);
        };
    auto bad_result = binding::select_rows<NoSuchColumnRow>(conn,
        "SELECT id, middle_extra FROM output_by_name_test", 10, 10, bad_batch);
    check(bad_result.status == binding::ExecStatus::QueryError,
          "a field with no matching column name -> QueryError, not silent garbage", &failures);

    std::printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");

    binding::execute(conn, "DROP TABLE output_by_name_test");
    conn.disconnect();
    return failures == 0 ? 0 : 1;
}
