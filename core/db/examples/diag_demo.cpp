// Demo for oci_diag.h: server info, session stats, and per-query round trip and
// memory cost.
//
// With no arguments it runs against the mock, which returns canned rows, so the
// numbers are meaningless -- that only shows the calls work. Against a real
// database it is the way to check the V$ view SQL in oci_diag.h, which has not
// been run against one yet:
//
//   diag_demo <connect_string> <user> <password>
//
// The user needs a grant on the V$ views (SELECT_CATALOG_ROLE is the easy way;
// oci_diag.h lists the individual V_$ views). Missing ones are listed under
// MISSING and everything else still prints.
//
// The last part runs the same query twice with different prefetch sizes. The
// round trip count is the point: 5000 rows at prefetch 10 is hundreds of round
// trips, at prefetch 1000 it is a handful.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>
#include <db/oracle/oci_diag.h>

using namespace marketlib::db::oracle;

namespace {

struct ObjectRow {
    FixedString<128> object_name;
    FixedString<32>  object_type;
};

void run_and_report(OciConnection& conn, QueryMeter& meter, std::size_t prefetch) {
    std::size_t rows = 0;
    const auto m = meter.measure([&] {
        return select_rows<ObjectRow>(conn, "SELECT object_name, object_type FROM all_objects WHERE ROWNUM <= 5000",
                                      prefetch, 500,
                                      [&](const ObjectRow*, std::size_t n) { rows += n; });
    });
    std::printf("\n--- prefetch %zu: %zu rows, %s ---\n", prefetch, rows,
                m.result.status == ExecStatus::Success ? "ok" : m.result.call.error_text.c_str());
    std::fputs(describe(m.stats).c_str(), stdout);
    for (const auto& p : m.problems) std::printf("(stats incomplete: %s -- %s)\n", p.section.c_str(), p.message.c_str());
}

} // namespace

int main(int argc, char** argv) {
    const std::string connect_string = argc > 1 ? argv[1] : "mockdb";
    const std::string user = argc > 2 ? argv[2] : "user";
    const std::string password = argc > 3 ? argv[3] : "pass";

    OciConnection conn(connect_string, user, password);
    if (!conn.connect()) {
        std::fprintf(stderr, "could not connect to %s\n", connect_string.c_str());
        return EXIT_FAILURE;
    }

    std::puts("=== server_info ===");
    std::fputs(describe(server_info(conn)).c_str(), stdout);

    std::puts("\n=== session_stats ===");
    if (const auto s = session_stats(conn)) std::fputs(describe(*s).c_str(), stdout);
    else std::printf("unavailable: %s\n", s.error().message.c_str());

    std::puts("\n=== per-query cost (QueryMeter) ===");
    QueryMeter meter(conn);
    run_and_report(conn, meter, 10);
    run_and_report(conn, meter, 1000);
    return EXIT_SUCCESS;
}
