// Tests for db/oracle/oci_diag.h, run against the OCI mock.
//
// The mock returns canned rows whatever the SQL says, so these can prove the
// plumbing (which views are queried, in what order, how a missing grant or a
// dead connection is reported, how the before/after arithmetic works) and NOT
// that the SQL is right for a real Oracle. Values like "row0_col0" below are
// the mock's canned strings, asserted only to pin down that a column landed in
// the intended field. The SQL is exercised by core/db/examples/diag_demo.cpp
// against a real database.
#include <catch2/catch_test_macros.hpp>

#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>
#include <db/oracle/oci_diag.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace marketlib::db::oracle;

#if !MARKETLIB_DB_HAS_REAL_OCI

namespace {

struct Connected {
    OciConnection conn{"orcl", "app_user", "secret"};
    Connected() { REQUIRE(conn.connect()); }
    ~Connected() { conn.disconnect(); }
};

// Clears the mock's SQL log and failure switches before and after each test.
struct CleanMock {
    CleanMock() { reset(); }
    ~CleanMock() { reset(); }
    static void reset() {
        mock::reset_sql_hooks();
        mock::set_mode(mock::FailureMode::None);
    }
};

bool issued(std::string_view fragment) {
    return std::any_of(mock::g_sql_log.begin(), mock::g_sql_log.end(),
                       [&](const std::string& s) { return s.find(fragment) != std::string::npos; });
}
std::size_t count_issued(std::string_view fragment) {
    return static_cast<std::size_t>(std::count_if(mock::g_sql_log.begin(), mock::g_sql_log.end(),
        [&](const std::string& s) { return s.find(fragment) != std::string::npos; }));
}

SessionStats stats_of(std::initializer_list<NamedValue> v, std::string sql_id = {}) {
    return SessionStats{std::move(sql_id), std::vector<NamedValue>(v)};
}

} // namespace

// ---------------------------------------------------------------------------
// server_info
// ---------------------------------------------------------------------------

TEST_CASE("server_info: queries every section and maps columns into fields", "[diag]") {
    CleanMock clean;
    Connected c;
    const ServerInfo info = server_info(c.conn);

    CHECK(info.problems.empty());
    for (const char* view : {"v$instance", "v$database", "v$version", "v$osstat", "v$sgainfo", "v$pgastat",
                             "v$parameter", "v$containers"})
        CHECK(issued(view));
    CHECK(issued("SYS_CONTEXT('USERENV','SERVER_HOST')"));

    CHECK(info.identity.db_name == "row0_col0");
    CHECK(info.identity.server_host == "row0_col4");
    CHECK(info.instance.instance_name == "row0_col1");
    CHECK(info.instance.host_name == "row0_col2");
    CHECK(info.database.name == "row0_col0");
    CHECK(info.database.platform_name == "row0_col6");
    CHECK(info.container.multitenant);
    CHECK(info.container.con_name == "row0_col0");

    // The mock returns 3 rows for every query.
    REQUIRE(info.os_stats.size() == 3);
    CHECK(info.os_stats[0].name == "row0_col0");
    CHECK(info.sga.size() == 3);
    CHECK(info.pga.size() == 3);
    CHECK(info.parameters.size() == 3);
    CHECK(info.parameters[1].value == "row1_col1");
    CHECK(info.containers.size() == 3);
    CHECK(info.version_banners.size() == 6); // banner and banner_full, 3 rows each
}

TEST_CASE("server_info: one unreadable view is reported and the rest still fill in", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::g_fail_on_sql = "v$osstat"; // ORA-00942, as a missing grant looks

    const ServerInfo info = server_info(c.conn);

    REQUIRE(info.problems.size() == 1);
    CHECK(info.problems[0].section == "host");
    CHECK(info.problems[0].error_code == 942);
    CHECK(info.problems[0].message.find("ORA-00942") != std::string::npos);
    CHECK_FALSE(info.problems[0].connection_lost);
    CHECK(info.os_stats.empty());
    CHECK_FALSE(info.num_cpus().has_value());
    CHECK(info.sga.size() == 3);                  // later sections were still read
    CHECK(info.identity.db_name == "row0_col0");  // and earlier ones kept

    const std::string text = describe(info);
    CHECK(text.find("MISSING") != std::string::npos);
    CHECK(text.find("host") != std::string::npos);
}

TEST_CASE("server_info: a pre-12c server is not a problem", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::g_fail_on_sql = "CON_NAME";
    mock::g_fail_on_sql_code = 2003; // ORA-02003: invalid USERENV parameter

    const ServerInfo info = server_info(c.conn);

    CHECK(info.problems.empty());
    CHECK_FALSE(info.container.multitenant);
    CHECK_FALSE(issued("v$containers")); // not attempted on a server with no containers
    CHECK(info.instance.instance_name == "row0_col1");
    CHECK(describe(info).find("not multitenant") != std::string::npos);
}

TEST_CASE("server_info: a lost connection stops the report after one problem", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::set_mode(mock::FailureMode::DisconnectThenRecover, 1); // first execute: ORA-03113

    const ServerInfo info = server_info(c.conn);

    REQUIRE(info.problems.size() == 1);
    CHECK(info.problems[0].connection_lost);
    CHECK(info.problems[0].error_code == 3113);
    CHECK(mock::g_sql_log.size() == 1); // nothing further was even prepared
}

TEST_CASE("server_info: every query failing yields an empty report, not a crash", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::set_mode(mock::FailureMode::ExecErrorAlways);

    const ServerInfo info = server_info(c.conn);

    CHECK_FALSE(info.problems.empty());
    CHECK(info.identity.db_name.empty());
    CHECK(info.os_stats.empty());
}

TEST_CASE("server_info: not connected is reported, not attempted", "[diag]") {
    CleanMock clean;
    OciConnection conn{"orcl", "u", "p"}; // never connected
    const ServerInfo info = server_info(conn);
    REQUIRE(info.problems.size() == 1);
    CHECK(info.problems[0].message == "not connected");
    CHECK(mock::g_sql_log.empty());
}

// ---------------------------------------------------------------------------
// session_stats
// ---------------------------------------------------------------------------

TEST_CASE("session_stats: asks V$MYSTAT for the key counters by name", "[diag]") {
    CleanMock clean;
    Connected c;
    const auto s = session_stats(c.conn);

    REQUIRE(s.has_value());
    CHECK(s->values.size() == 3);
    CHECK(s->sql_id.empty());
    CHECK(issued("v$mystat"));
    CHECK(issued("'SQL*Net roundtrips to/from client'"));
    CHECK(issued("'session pga memory'"));
    CHECK_FALSE(issued("v$session")); // sql id not requested
}

TEST_CASE("session_stats: options change the query", "[diag]") {
    CleanMock clean;
    Connected c;

    const auto with_id = session_stats(c.conn, {.all_nonzero = false, .with_sql_id = true});
    REQUIRE(with_id.has_value());
    CHECK(issued("prev_sql_id"));
    CHECK(with_id->sql_id == "row0_col2"); // third column of the canned row

    mock::reset_sql_hooks();
    const auto all = session_stats(c.conn, {.all_nonzero = true, .with_sql_id = false});
    REQUIRE(all.has_value());
    CHECK(issued("s.value <> 0"));
    CHECK_FALSE(issued("IN ("));
}

TEST_CASE("session_stats: an unreadable view is an error carrying the ORA code", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::g_fail_on_sql = "v$mystat";

    const auto s = session_stats(c.conn);

    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().section == "session_stats");
    CHECK(s.error().error_code == 942);
}

TEST_CASE("counter_delta: after minus before, by name, only for names in both", "[diag]") {
    const auto before = stats_of({{"a", 10}, {"b", 5}});
    const auto after  = stats_of({{"a", 25}, {"c", 7}});
    const auto d = counter_delta(after, before);
    REQUIRE(d.size() == 1);
    CHECK(d[0].name == "a");
    CHECK(d[0].value == 15);
}

// ---------------------------------------------------------------------------
// statement_stats
// ---------------------------------------------------------------------------

TEST_CASE("statement_stats: binds the sql_id and reads the V$SQL row", "[diag]") {
    CleanMock clean;
    Connected c;
    const auto s = statement_stats(c.conn, "abc123def4567");

    REQUIRE(s.has_value());
    CHECK(s->found);
    CHECK(s->sql_text == "row0_col14"); // last column of the canned row
    CHECK(issued("FROM v$sql WHERE sql_id = :sql_id"));
    CHECK(issued("SUM(runtime_mem)"));
    CHECK(issued("SUM(persistent_mem)"));
    CHECK(issued("SUM(sharable_mem)"));
}

TEST_CASE("statement_stats: derived ratios", "[diag]") {
    StatementStats s;
    s.executions = 4;
    s.fetches = 8;
    s.rows_processed = 200;
    CHECK(s.rows_per_fetch() == 25.0);
    CHECK(s.fetches_per_execution() == 2.0);

    const StatementStats none; // never executed: no division by zero
    CHECK(none.rows_per_fetch() == 0.0);
    CHECK(none.fetches_per_execution() == 0.0);
    CHECK(describe(none).find("not in the shared pool") != std::string::npos);
}

TEST_CASE("statement_stats: an unreadable V$SQL is an error", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::g_fail_on_sql = "v$sql";
    const auto s = statement_stats(c.conn, "abc");
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().error_code == 942);
}

// ---------------------------------------------------------------------------
// Per-query measurement
// ---------------------------------------------------------------------------

TEST_CASE("make_query_stats: subtracts the measurement's own cost and separates memory", "[diag]") {
    const auto before = stats_of({{std::string(stat_name::round_trips), 10},
                                  {std::string(stat_name::bytes_sent), 1000},
                                  {std::string(stat_name::bytes_received), 500},
                                  {std::string(stat_name::pga), 5'000'000},
                                  {std::string(stat_name::pga_max), 6'000'000},
                                  {std::string(stat_name::uga), 1'000'000},
                                  {std::string(stat_name::uga_max), 1'200'000},
                                  {std::string(stat_name::consistent_gets), 100}});
    const auto after = stats_of({{std::string(stat_name::round_trips), 16},
                                 {std::string(stat_name::bytes_sent), 5000},
                                 {std::string(stat_name::bytes_received), 900},
                                 {std::string(stat_name::pga), 5'300'000},
                                 {std::string(stat_name::pga_max), 6'000'000},
                                 {std::string(stat_name::uga), 1'000'000},
                                 {std::string(stat_name::uga_max), 1'200'000},
                                 {std::string(stat_name::consistent_gets), 250}},
                                "g8mzuc6yv8bq7");
    const std::vector<NamedValue> overhead{{std::string(stat_name::round_trips), 2},
                                           {std::string(stat_name::bytes_sent), 300},
                                           {std::string(stat_name::bytes_received), 100},
                                           {std::string(stat_name::consistent_gets), 5}};

    const QueryStats q = make_query_stats(before, after, overhead, 12.5);

    CHECK(q.elapsed_ms == 12.5);
    CHECK(q.round_trips == 4);          // 16 - 10 - 2
    CHECK(q.overhead_round_trips == 2);
    CHECK(q.bytes_sent == 3700);        // 4000 - 300
    CHECK(q.bytes_received == 300);     // 400 - 100
    CHECK(q.counter(stat_name::consistent_gets) == 145);
    CHECK(q.pga.before == 5'000'000);
    CHECK(q.pga.after == 5'300'000);
    CHECK(q.pga.growth() == 300'000);
    CHECK(q.pga.max_after == 6'000'000);
    CHECK(q.uga.growth() == 0);
    CHECK(q.sql_id == "g8mzuc6yv8bq7");
    // Gauges live in the memory fields, not the counters.
    CHECK_FALSE(find_value(q.counters, stat_name::pga).has_value());
    CHECK_FALSE(find_value(q.counters, stat_name::uga_max).has_value());
}

TEST_CASE("make_query_stats: a correction larger than the delta clamps to zero", "[diag]") {
    const auto before = stats_of({{std::string(stat_name::round_trips), 10}});
    const auto after  = stats_of({{std::string(stat_name::round_trips), 11}});
    const QueryStats q = make_query_stats(before, after, {{std::string(stat_name::round_trips), 3}}, 0);
    CHECK(q.round_trips == 0);
}

TEST_CASE("QueryMeter: brackets the call with snapshots and looks the statement up", "[diag]") {
    CleanMock clean;
    Connected c;
    QueryMeter meter(c.conn);

    const auto m = meter.measure([&] { return execute(c.conn, "DELETE FROM t"); });

    CHECK(m.result.status == ExecStatus::Success);
    CHECK(m.problems.empty());
    CHECK(meter.calibrated());
    CHECK(meter.reads_sql_id());
    CHECK(m.stats.elapsed_ms >= 0.0);
    CHECK(m.stats.round_trips == 0);   // the mock returns identical snapshots
    CHECK(m.stats.sql_id == "row0_col2");
    REQUIRE(m.stats.statement.has_value());
    CHECK(m.stats.statement->sql_text == "row0_col14");

    // Calibration: 1 + 3 snapshots. Then before, the call, after, V$SQL.
    CHECK(count_issued("v$mystat") == 6);
    REQUIRE(mock::g_sql_log.size() >= 4);
    const auto& log = mock::g_sql_log;
    CHECK(log[log.size() - 4].find("v$mystat") != std::string::npos);
    CHECK(log[log.size() - 3] == "DELETE FROM t");
    CHECK(log[log.size() - 2].find("v$mystat") != std::string::npos);
    CHECK(log[log.size() - 1].find("FROM v$sql") != std::string::npos);

    // A second measurement does not calibrate again: before, call, after, V$SQL.
    mock::g_sql_log.clear();
    (void)meter.measure([&] { return execute(c.conn, "DELETE FROM t"); });
    CHECK(mock::g_sql_log.size() == 4);
}

TEST_CASE("QueryMeter: the call still runs when measuring is impossible", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::g_fail_on_sql = "v$mystat";
    QueryMeter meter(c.conn);
    bool ran = false;

    const auto m = meter.measure([&] { ran = true; return ExecResult{}; });

    CHECK(ran);
    CHECK(m.result.status == ExecStatus::Success);
    CHECK_FALSE(m.problems.empty());
    CHECK(m.problems[0].error_code == 942);
    CHECK_FALSE(meter.calibrated());
    CHECK(m.stats.round_trips == 0);
    CHECK(m.stats.elapsed_ms >= 0.0);
}

TEST_CASE("QueryMeter: without a grant on V$SESSION it measures without the sql id", "[diag]") {
    CleanMock clean;
    Connected c;
    mock::g_fail_on_sql = "v$session"; // only the prev_sql_id variant of the snapshot query mentions it
    QueryMeter meter(c.conn);

    const auto m = meter.measure([&] { return execute(c.conn, "DELETE FROM t"); });

    CHECK(m.problems.empty());
    CHECK(meter.calibrated());
    CHECK_FALSE(meter.reads_sql_id());
    CHECK(m.stats.sql_id.empty());
    CHECK_FALSE(m.stats.statement.has_value());
    CHECK_FALSE(issued("FROM v$sql WHERE")); // no sql id, so no statement lookup
}

TEST_CASE("QueryMeter: the call's own failure is passed through untouched", "[diag]") {
    CleanMock clean;
    Connected c;
    QueryMeter meter(c.conn);
    const auto m = meter.measure([] {
        ExecResult r;
        r.status = ExecStatus::QueryError;
        r.call.error_code = 1;
        return r;
    });
    CHECK(m.result.status == ExecStatus::QueryError);
    CHECK(m.result.call.error_code == 1);
}

TEST_CASE("describe(QueryStats): names the round trips and the correction", "[diag]") {
    QueryStats q;
    q.round_trips = 4;
    q.overhead_round_trips = 2;
    q.elapsed_ms = 1.5;
    const std::string s = describe(q);
    CHECK(s.find("4 round trips") != std::string::npos);
    CHECK(s.find("removing 2") != std::string::npos);
}

#endif // !MARKETLIB_DB_HAS_REAL_OCI
