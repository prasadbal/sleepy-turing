// Real-database verification for select_generic() -- the raw,
// type-erased fetch path (no struct, no reflection, no type
// conversion -- oci_client.h). Prints exactly what Oracle describes and
// hands back, unconverted.
//
// Builds as the db_live_oracle_generic_demo target; needs a real Oracle client
// configured (see live_oracle_demo.cpp for the cmake -D options), then:
//
//   cmake --build build/linux-release --target db_live_oracle_generic_demo
//   LD_LIBRARY_PATH=<INSTANT_CLIENT> build/linux-release/core/db/db_live_oracle_generic_demo <connect_string> <user> <password>
#include <cstdio>
#include <cstring>

#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>

using namespace marketlib::db::oracle;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
    if (!ok) ++g_failures;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: %s <connect_string> <username> <password>\n", argv[0]);
        return 1;
    }
    OciConnection conn(argv[1], argv[2], argv[3]);
    check(conn.connect(), "connect() succeeded");

    execute(conn, "DROP TABLE new_arch_generic_test");
    check(execute(conn, "CREATE TABLE new_arch_generic_test (id NUMBER, name VARCHAR2(16), notional NUMBER(10,2))")
              .status == ExecStatus::Success,
          "CREATE TABLE succeeded");

    struct Row { int id; FixedString<16> name; double notional; };
    Row r1{1, FixedString<16>("Alpha"), 2.5};
    execute(conn, "INSERT INTO new_arch_generic_test VALUES(:id, :name, :notional)", r1);
    Row r2{2, FixedString<16>("BB"), 99.75};
    execute(conn, "INSERT INTO new_arch_generic_test VALUES(:id, :name, :notional)", r2);

    std::printf("\n--- select_generic() -- raw described columns, no struct ---\n");
    std::size_t total_rows = 0;
    ub2 id_type = 0, name_type = 0, notional_type = 0;
    ub4 id_size = 0, name_size = 0;
    bool saw_id_1_name_alpha = false;

    auto r = select_generic(conn, "SELECT id, name, notional FROM new_arch_generic_test ORDER BY id", 10, 10,
        [&](const GenericBatch& batch) {
            check(batch.columns.size() == 3, "describeColumns() found exactly 3 columns");
            if (batch.columns.size() == 3) {
                check(batch.columns[0].name == "ID", "column 0 name is 'ID'");
                check(batch.columns[1].name == "NAME", "column 1 name is 'NAME'");
                check(batch.columns[2].name == "NOTIONAL", "column 2 name is 'NOTIONAL'");
                id_type = batch.columns[0].oracle_type;
                name_type = batch.columns[1].oracle_type;
                notional_type = batch.columns[2].oracle_type;
                id_size = batch.columns[0].data_size;
                name_size = batch.columns[1].data_size;
            }
            std::printf("  batch: %zu rows, described types: id=%u(size=%u) name=%u(size=%u) notional=%u\n",
                       batch.row_count, id_type, id_size, name_type, name_size, notional_type);

            for (std::size_t row = 0; row < batch.row_count; ++row) {
                const auto& name_col = batch.columns[1];
                const unsigned char* name_bytes = batch.column_data[1].data() + row * name_col.data_size;
                const ub2 name_len = batch.lengths[1][row];
                std::string name_str(reinterpret_cast<const char*>(name_bytes), name_len);

                const auto& id_col = batch.columns[0];
                std::printf("  row %zu: name='%s' (len=%u) id_bytes=%zu id_indicator=%d\n",
                           row, name_str.c_str(), name_len, static_cast<std::size_t>(id_col.data_size),
                           batch.indicators[0][row]);
                if (name_str == "Alpha") saw_id_1_name_alpha = true;
                ++total_rows;
            }
        });

    check(r.status == ExecStatus::Success, "select_generic() succeeded");
    check(total_rows == 2, "fetched exactly 2 rows");
    check(saw_id_1_name_alpha, "row with name='Alpha' (real length 5, not blank-padded to 16) was fetched");
    check(name_type == SQLT_CHR, "NAME column described as SQLT_CHR (native VARCHAR2 type)");
    check(id_type == SQLT_NUM, "ID column described as SQLT_NUM (Oracle's native NUMBER type, unconverted)");

    std::printf("\n--- select_generic() -- LOB column is rejected, not silently fetched wrong ---\n");
    execute(conn, "DROP TABLE new_arch_generic_lob_test");
    execute(conn, "CREATE TABLE new_arch_generic_lob_test (id NUMBER, body CLOB)");
    auto lob_result = select_generic(conn, "SELECT id, body FROM new_arch_generic_lob_test", 10, 10,
                                     [](const GenericBatch&) {});
    check(lob_result.status == ExecStatus::QueryError, "a query selecting a CLOB column returns QueryError");
    check(lob_result.call.error_text.find("LOB") != std::string::npos, "error text explains why (mentions LOB)");
    std::printf("  error_text: %s\n", lob_result.call.error_text.c_str());

    execute(conn, "DROP TABLE new_arch_generic_test");
    execute(conn, "DROP TABLE new_arch_generic_lob_test");

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
