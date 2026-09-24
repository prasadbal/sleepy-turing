#pragma once
// Diagnostics on top of the reflection layer (oci_client.h): what server is
// this, what has this session cost so far, and what did this one query cost.
//
//   server_info(conn)               host/instance/database/PDB identity and
//                                   version, host CPU and memory (V$OSSTAT),
//                                   SGA and PGA, memory-related parameters
//   session_stats(conn)             this session's V$MYSTAT counters, notably
//                                   SQL*Net round trips, bytes, PGA/UGA memory
//   statement_stats(conn, sql_id)   V$SQL totals for one statement: executions,
//                                   fetches, buffer gets, shared-pool memory
//   QueryMeter::measure(fn)         runs fn and reports what *it* cost: round
//                                   trips, bytes, memory growth, plus the
//                                   V$SQL row for the statement it ran
//
// Everything reads V$ views, which need a grant (SELECT_CATALOG_ROLE, or
// SELECT on V_$INSTANCE, V_$DATABASE, V_$VERSION, V_$OSSTAT, V_$SGAINFO,
// V_$PGASTAT, V_$PARAMETER, V_$CONTAINERS, V_$MYSTAT, V_$STATNAME, V_$SESSION,
// V_$SQL). server_info() is a report of independent sections: a section the
// user may not read lands in ServerInfo::problems and the rest still fill in.
// The single-query functions return std::expected instead.
//
// Nothing here throws. Every query goes through select_rows(), so a failed one
// comes back as a DiagProblem carrying the ORA- code and text.
//
// STATUS: written against the documented V$ view and column names and tested
// only against the OCI mock (tests/db/test_diag.cpp), which returns canned rows
// whatever the SQL says. The SQL itself has NOT been run against a real Oracle.
// Run core/db/examples/diag_demo.cpp against one before relying on it.
#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace marketlib::db::oracle {

// ----------------------------------------------------------------------------
// Shared vocabulary
// ----------------------------------------------------------------------------

// Why one part of a report is missing. error_code is the ORA- number (942 = no
// such view, which for a V$ view usually means a missing grant).
struct DiagProblem {
    std::string section;
    std::string message;
    sb4 error_code = 0;
    bool connection_lost = false;
};

struct NamedValue {
    std::string name;
    double value = 0;
};
struct NamedText {
    std::string name;
    std::string value;
};

[[nodiscard]] inline std::optional<double> find_value(const std::vector<NamedValue>& values, std::string_view name) {
    for (const auto& v : values)
        if (v.name == name) return v.value;
    return std::nullopt;
}

// V$STATNAME names used below. The spelling is Oracle's, not ours.
namespace stat_name {
inline constexpr std::string_view round_trips     = "SQL*Net roundtrips to/from client";
inline constexpr std::string_view bytes_sent      = "bytes sent via SQL*Net to client";
inline constexpr std::string_view bytes_received  = "bytes received via SQL*Net from client";
inline constexpr std::string_view pga             = "session pga memory";
inline constexpr std::string_view pga_max         = "session pga memory max";
inline constexpr std::string_view uga             = "session uga memory";
inline constexpr std::string_view uga_max         = "session uga memory max";
inline constexpr std::string_view user_calls      = "user calls";
inline constexpr std::string_view recursive_calls = "recursive calls";
inline constexpr std::string_view parse_total     = "parse count (total)";
inline constexpr std::string_view parse_hard      = "parse count (hard)";
inline constexpr std::string_view execute_count   = "execute count";
inline constexpr std::string_view logical_reads   = "session logical reads";
inline constexpr std::string_view consistent_gets = "consistent gets";
inline constexpr std::string_view db_block_gets   = "db block gets";
inline constexpr std::string_view physical_reads  = "physical reads";
inline constexpr std::string_view cpu             = "CPU used by this session"; // centiseconds
inline constexpr std::string_view db_time         = "DB time";
inline constexpr std::string_view sorts_memory    = "sorts (memory)";
inline constexpr std::string_view sorts_disk      = "sorts (disk)";
inline constexpr std::string_view redo_size       = "redo size";
inline constexpr std::string_view user_commits    = "user commits";

inline constexpr std::array<std::string_view, 22> key_set{
    round_trips, bytes_sent, bytes_received, pga, pga_max, uga, uga_max, user_calls, recursive_calls,
    parse_total, parse_hard, execute_count, logical_reads, consistent_gets, db_block_gets, physical_reads,
    cpu, db_time, sorts_memory, sorts_disk, redo_size, user_commits};

// Memory statistics are gauges (a level, not a running total), so a before/after
// difference is "growth" rather than "work done", and the *_max ones don't
// subtract meaningfully at all.
[[nodiscard]] constexpr bool is_gauge(std::string_view n) noexcept {
    return n == pga || n == pga_max || n == uga || n == uga_max;
}
} // namespace stat_name

// ----------------------------------------------------------------------------
// Internals: row shapes, a query runner that turns failures into DiagProblems.
// Field order equals SELECT-list order (the mock, which cannot describe
// columns, maps by position; real Oracle maps by the aliases, case-blind).
// ----------------------------------------------------------------------------
namespace detail::diag {

struct NameValueRow { FixedString<128> name; double value{}; };
struct NameTextRow  { FixedString<128> name; FixedString<512> value; };
struct TextRow      { FixedString<256> text; };
struct StatRowSql   { FixedString<128> name; double value{}; FixedString<16> prev_sql_id; };

struct IdentityRow {
    FixedString<128> db_name;
    FixedString<128> db_unique_name;
    FixedString<128> instance_name;
    FixedString<128> service_name;
    FixedString<256> server_host;
    FixedString<128> login_user;
    FixedString<128> current_schema;
    double           sid{};
};
struct ContainerCtxRow { FixedString<128> con_name; double con_id{}; };
struct ContainerRow    { double con_id{}; FixedString<128> name; FixedString<32> open_mode; };
struct InstanceRow {
    double           instance_number{};
    FixedString<64>  instance_name;
    FixedString<128> host_name;
    FixedString<32>  version;
    FixedString<32>  startup_time;
    FixedString<32>  status;
    FixedString<32>  database_status;
    FixedString<32>  instance_role;
    FixedString<16>  logins;
    FixedString<16>  parallel;
    FixedString<16>  archiver;
};
struct DatabaseRow {
    FixedString<64>  name;
    double           dbid{};
    FixedString<32>  created;
    FixedString<32>  log_mode;
    FixedString<32>  open_mode;
    FixedString<32>  database_role;
    FixedString<128> platform_name;
};
struct StatementRow {
    FixedString<16>   sql_id;
    double            child_cursors{};
    double            executions{};
    double            fetches{};
    double            end_of_fetch_count{};
    double            parse_calls{};
    double            rows_processed{};
    double            buffer_gets{};
    double            disk_reads{};
    double            cpu_time_us{};
    double            elapsed_time_us{};
    double            sharable_mem{};
    double            persistent_mem{};
    double            runtime_mem{};
    FixedString<1000> sql_text;
};
struct SqlIdParam { FixedString<16> sql_id; };

inline constexpr std::size_t kPrefetchRows = 500;
inline constexpr std::size_t kBatchRows    = 256;

[[nodiscard]] inline DiagProblem problem_from(std::string_view section, const ExecResult& r) {
    std::string msg = r.call.error_text;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return DiagProblem{std::string(section), std::move(msg), r.call.error_code, r.status == ExecStatus::ConnectionLost};
}

[[nodiscard]] inline DiagProblem not_connected(std::string_view section) {
    return DiagProblem{std::string(section), "not connected", 0, false};
}

// Runs queries for one report. A failure is recorded and the runner carries
// on, except after a lost connection, where every further query would fail too.
class Runner {
public:
    Runner(OciConnection& conn, std::vector<DiagProblem>& problems) : conn_(conn), problems_(problems) {}

    // `quiet` lists ORA codes that mean "this server version doesn't have that"
    // (ORA-00904 invalid identifier, ORA-02003 invalid USERENV parameter) and
    // are not worth reporting.
    template <scalar_bindable Row>
    std::optional<std::vector<Row>> query(std::string_view section, const std::string& sql,
                                          std::initializer_list<sb4> quiet = {}) {
        if (lost_) return std::nullopt;
        std::vector<Row> rows;
        const ExecResult r = select_rows<Row>(conn_, sql, kPrefetchRows, kBatchRows,
            [&rows](const Row* p, std::size_t n) { rows.insert(rows.end(), p, p + n); });
        if (r.status == ExecStatus::Success) return rows;
        lost_ = r.status == ExecStatus::ConnectionLost;
        if (std::find(quiet.begin(), quiet.end(), r.call.error_code) == quiet.end())
            problems_.push_back(problem_from(section, r));
        return std::nullopt;
    }

private:
    OciConnection&            conn_;
    std::vector<DiagProblem>& problems_;
    bool                      lost_ = false;
};

inline constexpr auto to_named = [](const auto& rows) {
    std::vector<NamedValue> out;
    out.reserve(rows.size());
    for (const auto& r : rows) out.push_back({r.name.str(), r.value});
    return out;
};

} // namespace detail::diag

// ----------------------------------------------------------------------------
// server_info()
// ----------------------------------------------------------------------------
struct ServerInfo {
    // Where this session landed. SYS_CONTEXT('USERENV', ...): no grants needed.
    struct Identity {
        std::string db_name, db_unique_name, instance_name, service_name, server_host, login_user, current_schema;
        long long   sid = 0;
    } identity;

    // Multitenant. multitenant == false on a pre-12c server (no CON_NAME).
    struct Container {
        bool        multitenant = false;
        std::string con_name;   // the container this session is in (CDB$ROOT or a PDB)
        long long   con_id = 0;
        std::string cdb_name;   // 12.2+
    } container;
    struct ContainerInfo { long long con_id = 0; std::string name, open_mode; };
    std::vector<ContainerInfo> containers; // from a PDB this is normally just itself

    struct Instance {
        long long   instance_number = 0;
        std::string instance_name, host_name, version, version_full /* 18c+ */, startup_time, status,
                    database_status, instance_role, logins, parallel, archiver;
    } instance;

    struct Database {
        std::string name, created, log_mode, open_mode, database_role, platform_name;
        long long   dbid = 0;
        std::string cdb; // "YES"/"NO", 12c+
    } database;

    std::vector<std::string> version_banners; // V$VERSION (banner, and banner_full on 18c+)

    // Host as the instance sees it. V$OSSTAT's set of names varies by platform,
    // so this is every row it returns; the accessors below pick out common ones.
    std::vector<NamedValue> os_stats;
    std::vector<NamedValue> sga;         // V$SGAINFO, bytes
    std::vector<NamedValue> pga;         // V$PGASTAT
    std::vector<NamedText>  parameters;  // cpu_count, sga_target, pga_aggregate_target, ...

    std::vector<DiagProblem> problems;   // sections that could not be read

    [[nodiscard]] std::optional<double> os_stat(std::string_view name) const { return find_value(os_stats, name); }
    [[nodiscard]] std::optional<double> num_cpus() const { return os_stat("NUM_CPUS"); }
    [[nodiscard]] std::optional<double> num_cpu_cores() const { return os_stat("NUM_CPU_CORES"); }
    [[nodiscard]] std::optional<double> physical_memory_bytes() const { return os_stat("PHYSICAL_MEMORY_BYTES"); }
    [[nodiscard]] std::optional<double> sga_max_bytes() const { return find_value(sga, "Maximum SGA Size"); }
    [[nodiscard]] std::optional<double> pga_allocated_bytes() const { return find_value(pga, "total PGA allocated"); }
};

[[nodiscard]] inline ServerInfo server_info(OciConnection& conn) {
    using namespace detail::diag;
    ServerInfo info;
    if (!conn.connected()) {
        info.problems.push_back(not_connected("server_info"));
        return info;
    }
    Runner run(conn, info.problems);

    if (auto r = run.query<IdentityRow>("identity",
            "SELECT SYS_CONTEXT('USERENV','DB_NAME') AS db_name, "
            "SYS_CONTEXT('USERENV','DB_UNIQUE_NAME') AS db_unique_name, "
            "SYS_CONTEXT('USERENV','INSTANCE_NAME') AS instance_name, "
            "SYS_CONTEXT('USERENV','SERVICE_NAME') AS service_name, "
            "SYS_CONTEXT('USERENV','SERVER_HOST') AS server_host, "
            "SYS_CONTEXT('USERENV','SESSION_USER') AS login_user, "
            "SYS_CONTEXT('USERENV','CURRENT_SCHEMA') AS current_schema, "
            "TO_NUMBER(SYS_CONTEXT('USERENV','SID')) AS sid FROM dual");
        r && !r->empty()) {
        const auto& x = r->front();
        info.identity = {x.db_name.str(), x.db_unique_name.str(), x.instance_name.str(), x.service_name.str(),
                         x.server_host.str(), x.login_user.str(), x.current_schema.str(),
                         static_cast<long long>(x.sid)};
    }

    // ORA-02003 (invalid USERENV parameter) = pre-12c: no containers, not a fault.
    if (auto r = run.query<ContainerCtxRow>("container",
            "SELECT SYS_CONTEXT('USERENV','CON_NAME') AS con_name, "
            "TO_NUMBER(SYS_CONTEXT('USERENV','CON_ID')) AS con_id FROM dual", {2003});
        r && !r->empty()) {
        info.container.multitenant = true;
        info.container.con_name    = r->front().con_name.str();
        info.container.con_id      = static_cast<long long>(r->front().con_id);
        if (auto c = run.query<TextRow>("container",
                "SELECT SYS_CONTEXT('USERENV','CDB_NAME') AS text FROM dual", {2003}); c && !c->empty())
            info.container.cdb_name = c->front().text.str();
        if (auto c = run.query<ContainerRow>("containers",
                "SELECT con_id, name, open_mode FROM v$containers ORDER BY con_id"))
            for (const auto& x : *c)
                info.containers.push_back({static_cast<long long>(x.con_id), x.name.str(), x.open_mode.str()});
    }

    if (auto r = run.query<InstanceRow>("instance",
            "SELECT instance_number, instance_name, host_name, version, "
            "TO_CHAR(startup_time,'YYYY-MM-DD HH24:MI:SS') AS startup_time, status, database_status, "
            "instance_role, logins, parallel, archiver FROM v$instance");
        r && !r->empty()) {
        const auto& x = r->front();
        auto& i = info.instance;
        i.instance_number = static_cast<long long>(x.instance_number);
        i.instance_name = x.instance_name.str();  i.host_name = x.host_name.str();
        i.version = x.version.str();              i.startup_time = x.startup_time.str();
        i.status = x.status.str();                i.database_status = x.database_status.str();
        i.instance_role = x.instance_role.str();  i.logins = x.logins.str();
        i.parallel = x.parallel.str();            i.archiver = x.archiver.str();
    }
    if (auto r = run.query<TextRow>("instance", "SELECT version_full AS text FROM v$instance", {904});
        r && !r->empty())
        info.instance.version_full = r->front().text.str();

    if (auto r = run.query<DatabaseRow>("database",
            "SELECT name, dbid, TO_CHAR(created,'YYYY-MM-DD HH24:MI:SS') AS created, log_mode, open_mode, "
            "database_role, platform_name FROM v$database");
        r && !r->empty()) {
        const auto& x = r->front();
        auto& d = info.database;
        d.name = x.name.str();  d.dbid = static_cast<long long>(x.dbid);  d.created = x.created.str();
        d.log_mode = x.log_mode.str();  d.open_mode = x.open_mode.str();
        d.database_role = x.database_role.str();  d.platform_name = x.platform_name.str();
    }
    if (auto r = run.query<TextRow>("database", "SELECT cdb AS text FROM v$database", {904}); r && !r->empty())
        info.database.cdb = r->front().text.str();

    for (const char* sql : {"SELECT banner AS text FROM v$version", "SELECT banner_full AS text FROM v$version"})
        if (auto r = run.query<TextRow>("version", sql, {904}))   // banner_full is 18c+
            for (const auto& x : *r) info.version_banners.push_back(x.text.str());

    if (auto r = run.query<NameValueRow>("host", "SELECT stat_name AS name, value FROM v$osstat ORDER BY stat_name"))
        info.os_stats = to_named(*r);
    if (auto r = run.query<NameValueRow>("sga", "SELECT name, bytes AS value FROM v$sgainfo ORDER BY name"))
        info.sga = to_named(*r);
    if (auto r = run.query<NameValueRow>("pga", "SELECT name, value FROM v$pgastat ORDER BY name"))
        info.pga = to_named(*r);
    if (auto r = run.query<NameTextRow>("parameters",
            "SELECT name, value FROM v$parameter WHERE name IN ('cpu_count','sga_target','sga_max_size',"
            "'pga_aggregate_target','pga_aggregate_limit','memory_target','memory_max_target','inmemory_size',"
            "'shared_pool_size','db_cache_size','result_cache_max_size','db_block_size','processes','sessions',"
            "'use_large_pages','enable_pluggable_database') ORDER BY name"))
        for (const auto& x : *r) info.parameters.push_back({x.name.str(), x.value.str()});

    return info;
}

// ----------------------------------------------------------------------------
// session_stats()
// ----------------------------------------------------------------------------
struct SessionStatsOptions {
    bool all_nonzero = false; // every non-zero V$MYSTAT counter, not just stat_name::key_set (~1000 rows)
    bool with_sql_id = false; // also read V$SESSION.PREV_SQL_ID (needs a grant on V_$SESSION)
};

struct SessionStats {
    // PREV_SQL_ID as of the snapshot: the last statement this session ran
    // before the snapshot query itself. Empty unless with_sql_id was set.
    std::string             sql_id;
    std::vector<NamedValue> values;

    [[nodiscard]] std::optional<double> find(std::string_view name) const { return find_value(values, name); }
    [[nodiscard]] double get(std::string_view name) const { return find(name).value_or(0.0); }

    [[nodiscard]] double round_trips() const { return get(stat_name::round_trips); }
    [[nodiscard]] double bytes_sent() const { return get(stat_name::bytes_sent); }
    [[nodiscard]] double bytes_received() const { return get(stat_name::bytes_received); }
    [[nodiscard]] double pga_bytes() const { return get(stat_name::pga); }
    [[nodiscard]] double pga_max_bytes() const { return get(stat_name::pga_max); }
    [[nodiscard]] double uga_bytes() const { return get(stat_name::uga); }
    [[nodiscard]] double uga_max_bytes() const { return get(stat_name::uga_max); }
};

[[nodiscard]] inline std::expected<SessionStats, DiagProblem> session_stats(OciConnection& conn,
                                                                            SessionStatsOptions opts = {}) {
    using namespace detail::diag;
    if (!conn.connected()) return std::unexpected(not_connected("session_stats"));

    std::string sql = "SELECT n.name AS name, s.value AS value";
    if (opts.with_sql_id)
        sql += ", (SELECT prev_sql_id FROM v$session WHERE sid = TO_NUMBER(SYS_CONTEXT('USERENV','SID'))) "
               "AS prev_sql_id";
    sql += " FROM v$mystat s JOIN v$statname n ON n.statistic# = s.statistic#";
    if (opts.all_nonzero) {
        sql += " WHERE s.value <> 0";
    } else {
        sql += " WHERE n.name IN (";
        for (std::size_t i = 0; i < stat_name::key_set.size(); ++i) {
            if (i) sql += ", ";
            sql += '\'';
            sql += stat_name::key_set[i];
            sql += '\'';
        }
        sql += ')';
    }
    sql += " ORDER BY n.name";

    SessionStats out;
    ExecResult r;
    if (opts.with_sql_id) {
        r = select_rows<StatRowSql>(conn, sql, kPrefetchRows, kBatchRows,
            [&out](const StatRowSql* p, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) {
                    out.values.push_back({p[i].name.str(), p[i].value});
                    if (out.sql_id.empty()) out.sql_id = p[i].prev_sql_id.str();
                }
            });
    } else {
        r = select_rows<NameValueRow>(conn, sql, kPrefetchRows, kBatchRows,
            [&out](const NameValueRow* p, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) out.values.push_back({p[i].name.str(), p[i].value});
            });
    }
    if (r.status != ExecStatus::Success) return std::unexpected(problem_from("session_stats", r));
    return out;
}

// after - before for every counter present in both, in `after`'s order.
[[nodiscard]] inline std::vector<NamedValue> counter_delta(const SessionStats& after, const SessionStats& before) {
    std::vector<NamedValue> out;
    for (const auto& a : after.values)
        if (const auto b = before.find(a.name)) out.push_back({a.name, a.value - *b});
    return out;
}

// ----------------------------------------------------------------------------
// statement_stats()
// ----------------------------------------------------------------------------
// V$SQL totals for one sql_id, summed over its child cursors. These are
// cumulative over every execution by every session since the cursor entered
// the shared pool, so they describe the statement, not one run of it; for one
// run use QueryMeter. The memory columns are per cursor, in bytes.
struct StatementStats {
    bool        found = false; // false: not in the shared pool (aged out, or never run)
    std::string sql_id, sql_text;
    double child_cursors = 0, executions = 0, fetches = 0, end_of_fetch_count = 0, parse_calls = 0,
           rows_processed = 0, buffer_gets = 0, disk_reads = 0, cpu_time_us = 0, elapsed_time_us = 0,
           sharable_mem_bytes = 0, persistent_mem_bytes = 0, runtime_mem_bytes = 0;

    // Rows returned per server fetch. Low with many rows means a small prefetch
    // is costing round trips.
    [[nodiscard]] double rows_per_fetch() const { return fetches > 0 ? rows_processed / fetches : 0.0; }
    [[nodiscard]] double fetches_per_execution() const { return executions > 0 ? fetches / executions : 0.0; }
};

[[nodiscard]] inline std::expected<StatementStats, DiagProblem> statement_stats(OciConnection& conn,
                                                                                std::string_view sql_id) {
    using namespace detail::diag;
    if (!conn.connected()) return std::unexpected(not_connected("statement_stats"));

    SqlIdParam param;
    param.sql_id.assign(sql_id);
    StatementStats out;
    const ExecResult r = select_rows<SqlIdParam, StatementRow>(conn,
        "SELECT sql_id, COUNT(*) AS child_cursors, SUM(executions) AS executions, SUM(fetches) AS fetches, "
        "SUM(end_of_fetch_count) AS end_of_fetch_count, SUM(parse_calls) AS parse_calls, "
        "SUM(rows_processed) AS rows_processed, SUM(buffer_gets) AS buffer_gets, SUM(disk_reads) AS disk_reads, "
        "SUM(cpu_time) AS cpu_time_us, SUM(elapsed_time) AS elapsed_time_us, SUM(sharable_mem) AS sharable_mem, "
        "SUM(persistent_mem) AS persistent_mem, SUM(runtime_mem) AS runtime_mem, MAX(sql_text) AS sql_text "
        "FROM v$sql WHERE sql_id = :sql_id GROUP BY sql_id",
        param, kPrefetchRows, kBatchRows,
        [&out](const StatementRow* p, std::size_t n) {
            if (n == 0 || out.found) return;
            const StatementRow& x = p[0];
            out = StatementStats{true, x.sql_id.str(), x.sql_text.str(), x.child_cursors, x.executions, x.fetches,
                                 x.end_of_fetch_count, x.parse_calls, x.rows_processed, x.buffer_gets,
                                 x.disk_reads, x.cpu_time_us, x.elapsed_time_us, x.sharable_mem,
                                 x.persistent_mem, x.runtime_mem};
        });
    if (r.status != ExecStatus::Success) return std::unexpected(problem_from("statement_stats", r));
    return out;
}

// Convenience overload: reads sql_id directly off the statement handle
// (OciStatement::sql_id(), an exact per-statement id) instead of a caller
// having to thread it through manually -- and unlike session_stats()'s
// with_sql_id option (V$SESSION.PREV_SQL_ID, "whatever this session ran
// last"), this is unambiguously the query V$SQL is being asked about,
// even if other statements ran on the same connection in between. `stmt`
// must already be past execute() -- see OciStatement::sql_id()'s own
// comment for why.
[[nodiscard]] inline std::expected<StatementStats, DiagProblem> statement_stats(OciConnection& conn,
                                                                                const OciStatement& stmt) {
    return statement_stats(conn, stmt.sql_id());
}

// ----------------------------------------------------------------------------
// QueryMeter: what did this call cost?
// ----------------------------------------------------------------------------
struct MemoryUse {
    double before = 0, after = 0, max_after = 0; // bytes
    [[nodiscard]] double growth() const { return after - before; }
};

struct QueryStats {
    double elapsed_ms = 0;        // client wall clock around the call
    double round_trips = 0;       // server-counted SQL*Net round trips, measurement cost removed
    double bytes_sent = 0;        // server -> client, measurement cost removed
    double bytes_received = 0;    // client -> server, measurement cost removed
    MemoryUse pga, uga;           // this session's memory around the call
    std::vector<NamedValue> counters; // every other key counter (gets, parses, sorts, ...), cost removed
    std::string sql_id;           // the last statement the call ran; empty if it could not be read
    std::optional<StatementStats> statement; // its V$SQL totals (whole cursor, not just this run)
    double overhead_round_trips = 0; // what was subtracted, so the correction is visible

    [[nodiscard]] double counter(std::string_view name) const { return find_value(counters, name).value_or(0.0); }
};

// The pure arithmetic of a measurement, split out so it can be tested without a
// database. `overhead` is what a before/after pair costs with nothing in between.
[[nodiscard]] inline QueryStats make_query_stats(const SessionStats& before, const SessionStats& after,
                                                 const std::vector<NamedValue>& overhead, double elapsed_ms) {
    QueryStats q;
    q.elapsed_ms = elapsed_ms;
    q.sql_id = after.sql_id;
    for (const auto& d : counter_delta(after, before)) {
        if (stat_name::is_gauge(d.name)) continue;
        const double cost = find_value(overhead, d.name).value_or(0.0);
        q.counters.push_back({d.name, std::max(0.0, d.value - cost)});
    }
    q.round_trips = q.counter(stat_name::round_trips);
    q.bytes_sent = q.counter(stat_name::bytes_sent);
    q.bytes_received = q.counter(stat_name::bytes_received);
    q.overhead_round_trips = find_value(overhead, stat_name::round_trips).value_or(0.0);
    q.pga = {before.pga_bytes(), after.pga_bytes(), after.pga_max_bytes()};
    q.uga = {before.uga_bytes(), after.uga_bytes(), after.uga_max_bytes()};
    return q;
}

// Measures a call by taking a V$MYSTAT snapshot before and after it. The
// snapshots are queries too and count as round trips themselves, so the meter
// first learns what an empty before/after pair costs and subtracts that.
// Two extra queries per measurement (three with the V$SQL lookup): use it on
// chosen calls or a sample, not blanket on a hot path. One meter per connection;
// not thread-safe.
class QueryMeter {
public:
    explicit QueryMeter(OciConnection& conn) : conn_(conn) {}

    struct Measured {
        ExecResult               result;   // what the call returned
        QueryStats               stats;    // zero-filled if the snapshots could not be taken
        std::vector<DiagProblem> problems; // why stats may be incomplete
    };

    // Learns the cost of a bare before/after pair (the minimum over `samples`
    // pairs, to shed noise). measure() does this itself on first use.
    [[nodiscard]] std::expected<void, DiagProblem> calibrate(int samples = 3) {
        auto prev = snapshot();
        if (!prev && opts_.with_sql_id) { // no grant on V_$SESSION: carry on without sql ids
            opts_.with_sql_id = false;
            prev = snapshot();
        }
        if (!prev) return std::unexpected(prev.error());
        std::vector<NamedValue> best;
        for (int i = 0; i < samples; ++i) {
            auto next = snapshot();
            if (!next) return std::unexpected(next.error());
            for (const auto& d : counter_delta(*next, *prev)) {
                auto it = std::find_if(best.begin(), best.end(), [&](const NamedValue& b) { return b.name == d.name; });
                if (it == best.end()) best.push_back(d);
                else it->value = std::min(it->value, d.value);
            }
            prev = std::move(next);
        }
        overhead_ = std::move(best);
        calibrated_ = true;
        return {};
    }

    // Runs `run` (which must return the ExecResult of whatever it did) and
    // reports what it cost. `run` always executes, even if measuring fails.
    template <class F>
        requires std::is_invocable_r_v<ExecResult, F&>
    [[nodiscard]] Measured measure(F&& run) {
        Measured m;
        if (!calibrated_)
            if (auto c = calibrate(); !c) m.problems.push_back(c.error());

        std::optional<SessionStats> before;
        if (calibrated_) {
            if (auto b = snapshot()) before = std::move(*b);
            else m.problems.push_back(b.error());
        }

        const auto t0 = std::chrono::steady_clock::now();
        m.result = run();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        if (before) {
            if (auto a = snapshot()) {
                m.stats = make_query_stats(*before, *a, overhead_, ms);
                if (!a->sql_id.empty()) {
                    if (auto st = statement_stats(conn_, a->sql_id)) {
                        if (st->found) m.stats.statement = std::move(*st);
                    } else {
                        m.problems.push_back(st.error());
                    }
                }
            } else {
                m.problems.push_back(a.error());
            }
        }
        m.stats.elapsed_ms = ms;
        return m;
    }

    [[nodiscard]] bool calibrated() const noexcept { return calibrated_; }
    [[nodiscard]] bool reads_sql_id() const noexcept { return opts_.with_sql_id; }

private:
    std::expected<SessionStats, DiagProblem> snapshot() { return session_stats(conn_, opts_); }

    OciConnection&          conn_;
    SessionStatsOptions     opts_{.all_nonzero = false, .with_sql_id = true};
    std::vector<NamedValue> overhead_;
    bool                    calibrated_ = false;
};

// ----------------------------------------------------------------------------
// Plain-text rendering, for logs and the demo.
// ----------------------------------------------------------------------------
namespace detail::diag {
[[nodiscard]] inline std::string mib(double bytes) { return std::format("{:.1f} MiB", bytes / (1024.0 * 1024.0)); }
} // namespace detail::diag

[[nodiscard]] inline std::string describe(const ServerInfo& s) {
    using detail::diag::mib;
    std::string o;
    o += std::format("session    : {} as {} (schema {}), sid {}, service '{}'\n", s.identity.db_name,
                     s.identity.login_user, s.identity.current_schema, s.identity.sid, s.identity.service_name);
    o += std::format("server host: {}\n", s.identity.server_host.empty() ? s.instance.host_name : s.identity.server_host);
    o += std::format("instance   : {} #{} v{}{} {} since {}, role {}\n", s.instance.instance_name,
                     s.instance.instance_number, s.instance.version,
                     s.instance.version_full.empty() ? "" : " (" + s.instance.version_full + ")", s.instance.status,
                     s.instance.startup_time, s.instance.instance_role);
    o += std::format("database   : {} dbid {} {} {} on {}\n", s.database.name, s.database.dbid, s.database.open_mode,
                     s.database.log_mode, s.database.platform_name);
    if (s.container.multitenant) {
        o += std::format("container  : {} (con_id {}) of CDB {}; cdb={}\n", s.container.con_name, s.container.con_id,
                         s.container.cdb_name.empty() ? "?" : s.container.cdb_name,
                         s.database.cdb.empty() ? "?" : s.database.cdb);
        for (const auto& c : s.containers) o += std::format("  con {} {} {}\n", c.con_id, c.name, c.open_mode);
    } else {
        o += "container  : not multitenant (pre-12c)\n";
    }
    for (const auto& b : s.version_banners) o += "banner     : " + b + "\n";
    o += "host       :";
    if (const auto n = s.num_cpus()) o += std::format(" {} cpus", *n);
    if (const auto n = s.num_cpu_cores()) o += std::format(", {} cores", *n);
    if (const auto n = s.physical_memory_bytes()) o += ", " + mib(*n) + " RAM";
    o += "\n";
    for (const auto& v : s.os_stats) o += std::format("  os   {:<34} {}\n", v.name, v.value);
    for (const auto& v : s.sga) o += std::format("  sga  {:<34} {}\n", v.name, mib(v.value));
    for (const auto& v : s.pga) o += std::format("  pga  {:<34} {}\n", v.name, v.value);
    for (const auto& v : s.parameters) o += std::format("  parm {:<34} {}\n", v.name, v.value);
    for (const auto& p : s.problems)
        o += std::format("MISSING    : {} -- {}{}\n", p.section, p.message, p.connection_lost ? " [connection lost]" : "");
    return o;
}

[[nodiscard]] inline std::string describe(const SessionStats& s) {
    std::string o;
    for (const auto& v : s.values) o += std::format("  {:<40} {}\n", v.name, v.value);
    return o;
}

[[nodiscard]] inline std::string describe(const StatementStats& s) {
    if (!s.found) return std::format("sql_id {}: not in the shared pool\n", s.sql_id);
    return std::format(
        "sql_id {}: {} execs, {} fetches ({:.1f} rows/fetch), {} parses, {} buffer gets, {} disk reads,\n"
        "  cpu {:.1f} ms, elapsed {:.1f} ms, memory sharable {:.0f} B, persistent {:.0f} B, runtime {:.0f} B\n"
        "  {}\n",
        s.sql_id, s.executions, s.fetches, s.rows_per_fetch(), s.parse_calls, s.buffer_gets, s.disk_reads,
        s.cpu_time_us / 1000.0, s.elapsed_time_us / 1000.0, s.sharable_mem_bytes, s.persistent_mem_bytes,
        s.runtime_mem_bytes, s.sql_text);
}

[[nodiscard]] inline std::string describe(const QueryStats& q) {
    using detail::diag::mib;
    std::string o = std::format(
        "elapsed {:.2f} ms, {} round trips (after removing {} for measuring), {} B sent, {} B received\n"
        "pga {} -> {} (max {}), uga {} -> {} (max {})\n",
        q.elapsed_ms, q.round_trips, q.overhead_round_trips, q.bytes_sent, q.bytes_received, mib(q.pga.before),
        mib(q.pga.after), mib(q.pga.max_after), mib(q.uga.before), mib(q.uga.after), mib(q.uga.max_after));
    if (q.statement) o += describe(*q.statement);
    else if (!q.sql_id.empty()) o += std::format("sql_id {}: no V$SQL row\n", q.sql_id);
    return o;
}

} // namespace marketlib::db::oracle
