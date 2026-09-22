// Prints server_info() for a list of servers -- built for comparing two or
// more environments side by side (e.g. "why is this query 40 min on test and
// 8 min on prod" -- start with whether pga_aggregate_target/SGA/host specs
// actually differ before chasing anything else).
//
//   server_stats_demo <servers_file>
//
// servers_file: one server per line, '|'-delimited:
//   name|connect_string|username|password
// Blank lines and lines starting with # are skipped. With no argument, runs
// against the mock under two made-up names, just to show the shape --
// every mock "server" returns identical canned data, so the comparison
// table is meaningless there; it only proves the plumbing works.
//
// A server that fails to connect is reported and skipped -- one bad
// connection string doesn't stop the rest of the list, same as server_info()
// itself not letting one missing grant stop the rest of a report.
//
// Runs one thread per server, each owning its own OciConnection -- never
// shared across threads, which is what OciConnection::connect() now
// requires (OCI_THREADED) and the only model this project threads the db
// layer under. Each thread writes only into its own pre-sized slot in
// per_server (index == its position in `servers`), so there is no shared
// mutable state and no mutex needed for the results themselves; the
// per-server report text is collected into a string per thread and only
// printed after every thread has joined, so concurrent output from
// different servers can't interleave into garbled text on the terminal.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <db/oracle/oci_connection.h>
#include <db/oracle/oci_diag.h>

using namespace marketlib::db::oracle;

namespace {

struct ServerSpec {
    std::string name, connect_string, username, password;
};

std::vector<ServerSpec> parse_servers_file(const std::string& path) {
    std::vector<ServerSpec> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string name, conn, user, pass;
        if (!std::getline(ls, name, '|') || !std::getline(ls, conn, '|') ||
            !std::getline(ls, user, '|') || !std::getline(ls, pass, '|')) {
            std::fprintf(stderr, "skipping malformed line: %s\n", line.c_str());
            continue;
        }
        out.push_back({name, conn, user, pass});
    }
    return out;
}

struct Summary {
    std::string name;
    bool        connected = false;
    std::string connect_error;
    std::optional<double> cpus, host_ram_gib, sga_max_gib, pga_alloc_mib;
    std::string           pga_aggregate_target, pga_aggregate_limit, cpu_count_param;
    std::size_t           problem_count = 0;
};

// One thread's whole output: everything it would otherwise have printed
// directly, deferred so main can print every server's report in list order
// after all threads finish, instead of whichever server's printf happened
// to interleave first.
struct PerServerResult {
    std::string report_text;
    Summary     summary;
};

std::optional<std::string> find_param(const ServerInfo& info, std::string_view name) {
    for (const auto& p : info.parameters)
        if (p.name == name) return p.value;
    return std::nullopt;
}

void print_summary_table(const std::vector<Summary>& rows) {
    std::puts("\n=== summary (for a quick side-by-side; see each server's full report above for detail) ===");
    std::printf("%-16s %6s %10s %6s %10s %6s %10s %10s %5s\n", "server", "cpus", "host RAM", "SGA", "PGA alloc",
                "", "pga_target", "pga_limit", "warn?");
    std::printf("%-16s %6s %10s %6s %10s %6s %10s %10s %5s\n", "", "", "(GiB)", "(GiB)", "(MiB)", "", "", "", "");
    for (const auto& s : rows) {
        if (!s.connected) {
            std::printf("%-16s  CONNECT FAILED: %s\n", s.name.c_str(), s.connect_error.c_str());
            continue;
        }
        auto fmt = [](std::optional<double> v, const char* suffix = "") {
            char buf[32];
            if (v) std::snprintf(buf, sizeof buf, "%.1f%s", *v, suffix);
            else std::snprintf(buf, sizeof buf, "?");
            return std::string(buf);
        };
        std::printf("%-16s %6s %10s %6s %10s %6s %10s %10s %5zu\n", s.name.c_str(), fmt(s.cpus).c_str(),
                    fmt(s.host_ram_gib).c_str(), fmt(s.sga_max_gib).c_str(), fmt(s.pga_alloc_mib).c_str(), "",
                    s.pga_aggregate_target.empty() ? "?" : s.pga_aggregate_target.c_str(),
                    s.pga_aggregate_limit.empty() ? "?" : s.pga_aggregate_limit.c_str(), s.problem_count);
    }
}

// Runs entirely on the calling thread: its own OciConnection, its own
// server_info() call, nothing touched that any other thread also touches.
// Writes into `out` (this thread's own, pre-assigned slot) rather than
// printing directly.
void collect_one_server(const ServerSpec& sv, PerServerResult& out) {
    std::ostringstream rep;
    rep << "\n########## " << sv.name << " (" << sv.connect_string << ") ##########\n";
    out.summary.name = sv.name;

    OciConnection conn(sv.connect_string, sv.username, sv.password);
    if (!conn.connect()) {
        out.summary.connect_error = "could not connect";
        rep << "could not connect to " << sv.connect_string << "\n";
        out.report_text = rep.str();
        return;
    }
    out.summary.connected = true;

    const ServerInfo info = server_info(conn);
    rep << describe(info);
    out.report_text = rep.str();

    auto& sum = out.summary;
    sum.cpus = info.num_cpus();
    if (const auto ram = info.physical_memory_bytes()) sum.host_ram_gib = *ram / (1024.0 * 1024.0 * 1024.0);
    if (const auto sga = info.sga_max_bytes()) sum.sga_max_gib = *sga / (1024.0 * 1024.0 * 1024.0);
    if (const auto pga = info.pga_allocated_bytes()) sum.pga_alloc_mib = *pga / (1024.0 * 1024.0);
    if (const auto v = find_param(info, "pga_aggregate_target")) sum.pga_aggregate_target = *v;
    if (const auto v = find_param(info, "pga_aggregate_limit")) sum.pga_aggregate_limit = *v;
    sum.problem_count = info.problems.size();

    conn.disconnect();
}

} // namespace

int main(int argc, char** argv) {
    std::vector<ServerSpec> servers;
    if (argc > 1) {
        servers = parse_servers_file(argv[1]);
        if (servers.empty()) {
            std::fprintf(stderr, "no servers parsed from %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    } else {
        std::puts("(no servers file given -- running against the mock under two placeholder names;");
        std::puts(" every mock \"server\" returns identical canned data, so the table below is meaningless.");
        std::puts(" Real use: server_stats_demo <servers_file>, one 'name|connect_string|user|password' per line)");
        servers = {{"env_a", "mockdb", "user", "pass"}, {"env_b", "mockdb", "user", "pass"}};
    }

    // Pre-sized before any thread starts, and never resized afterward: each
    // thread writes only to results[i], its own element, so this needs no
    // synchronization even though several threads touch the vector at once.
    std::vector<PerServerResult> results(servers.size());
    {
        std::vector<std::jthread> threads;
        threads.reserve(servers.size());
        for (std::size_t i = 0; i < servers.size(); ++i)
            threads.emplace_back(collect_one_server, std::cref(servers[i]), std::ref(results[i]));
        // jthreads join automatically when `threads` goes out of scope, but
        // the block ends (and that join happens) before anything below reads
        // `results`, which is the actual ordering guarantee this relies on.
    }

    std::vector<Summary> summaries;
    summaries.reserve(results.size());
    for (const auto& r : results) {
        std::fputs(r.report_text.c_str(), stdout);
        summaries.push_back(r.summary);
    }

    print_summary_table(summaries);
    return EXIT_SUCCESS;
}
