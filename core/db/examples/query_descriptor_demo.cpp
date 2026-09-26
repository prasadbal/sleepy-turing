// Demo for query_descriptor.h: define a query's shape once as a
// QueryDescriptor, then execute it and dump its stats (round trips, bytes,
// PGA/UGA, V$SQL) in one call. Two illustrative descriptors below --
// ObjectsByName (no bind parameters) and ObjectsOfType (one bind parameter)
// -- meant as a pattern to copy for the real 5-6 queries this is for: add a
// struct with bind_type/define_type/query_name/key_type at namespace scope
// (must be namespace scope, not inside a function -- see query_descriptor.h)
// and one run_and_dump<YourDescriptor>(...) call in main(). The SQL text
// itself lives in config/db_queries.xml, not in this file -- see
// query_sql_registry.h for why, and add your query's <query name="..."> entry
// there alongside query_name matching it here.
//
//   query_descriptor_demo <connect_string> <user> <password> [queries.xml]
//
// With no arguments it runs against the mock (canned data, numbers
// meaningless, only proves the plumbing); [queries.xml] defaults to
// config/db_queries.xml, resolved relative to the current working directory
// (run from the repo root, same assumption every other demo/example here makes).
#include <cstdio>
#include <cstdlib>
#include <string>

#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>
#include <db/oracle/query_descriptor.h>

using namespace marketlib::db::oracle;

namespace {

struct ObjRow {
    FixedString<128> object_name;
    FixedString<32>  object_type;
};

// No bind parameters: bind_type is NoBind, run_and_measure<>() is called
// with no bind argument.
struct ObjectsByName {
    using bind_type   = NoBind;
    using define_type = ObjRow;
    static constexpr std::string_view query_name = "objects_by_name";
    using key_type = std::index_sequence<0>; // keyed on object_name
};

struct TypeFilter { FixedString<32> object_type; };

// One bind parameter (:object_type, matched to TypeFilter's field by name --
// same binding rule as execute()/select_rows() elsewhere in oci_client.h).
struct ObjectsOfType {
    using bind_type   = TypeFilter;
    using define_type = ObjRow;
    static constexpr std::string_view query_name = "objects_of_type";
    using key_type = std::index_sequence<0>;
};

// Runs one descriptor and prints its result shape plus everything QueryMeter
// measured about running it. This is the part to call once per query you're
// investigating; run_and_dump<YourDescriptor>(conn, meter, sql_registry,
// "label", bind...) for each of the 5-6.
template<class Descriptor, class... Bind>
void run_and_dump(OciConnection& conn, QueryMeter& meter, const QuerySqlRegistry& sql_registry,
                   std::string_view label, Bind&&... bind) {
    std::printf("\n########## %.*s ##########\n", static_cast<int>(label.size()), label.data());
    const std::string& sql = sql_registry.sql_for(Descriptor::query_name);
    std::printf("sql: %s\n", sql.c_str());

    const auto r = run_and_measure<Descriptor>(conn, meter, sql_registry, std::forward<Bind>(bind)...);

    std::printf("result: %s\n",
                r.result.status == ExecStatus::Success ? "ok" : r.result.call.error_text.c_str());
    std::printf("rows fetched: %zu, distinct keys: %zu%s\n", r.row_count, r.rows.size(),
                r.row_count != r.rows.size() ? "  <-- key isn't unique in this result set" : "");
    std::fputs(describe(r.stats).c_str(), stdout);
    for (const auto& p : r.problems)
        std::printf("(stats incomplete: %s -- %s)\n", p.section.c_str(), p.message.c_str());
}

} // namespace

int main(int argc, char** argv) {
    const std::string connect_string = argc > 1 ? argv[1] : "mockdb";
    const std::string user = argc > 2 ? argv[2] : "user";
    const std::string password = argc > 3 ? argv[3] : "pass";
    const std::string queries_path = argc > 4 ? argv[4] : "config/db_queries.xml";

    QuerySqlRegistry sql_registry = [&] {
        try {
            return QuerySqlRegistry::from_file(queries_path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "failed to load '%s': %s\n", queries_path.c_str(), e.what());
            std::exit(EXIT_FAILURE);
        }
    }();

    OciConnection conn(connect_string, user, password);
    if (!conn.connect()) {
        std::fprintf(stderr, "could not connect to %s\n", connect_string.c_str());
        return EXIT_FAILURE;
    }

    QueryMeter meter(conn);

    run_and_dump<ObjectsByName>(conn, meter, sql_registry, "1: ObjectsByName (no bind)");

    TypeFilter filter;
    filter.object_type.assign("TABLE");
    run_and_dump<ObjectsOfType>(conn, meter, sql_registry, "2: ObjectsOfType (bind :object_type)", filter);

    // Add descriptors 3..6 here the same way: a struct in this file (its
    // query_name matching a new <query name="..."> entry in
    // config/db_queries.xml) and one more run_and_dump<YourDescriptor>(...) call.

    // get_map<>() -- when you just want the rows keyed by their first field
    // and don't care about stats, the same ObjectsByName/ObjectsOfType
    // descriptors work directly: no separate struct, no query_name passed
    // twice, no QueryMeter.
    std::printf("\n########## 3: get_map<ObjectsByName>() ##########\n");
    const auto by_name = get_map<ObjectsByName>(conn, sql_registry);
    std::printf("rows: %zu\n", by_name.size());

    std::printf("\n########## 4: get_map<ObjectsOfType>() ##########\n");
    const auto of_type = get_map<ObjectsOfType>(conn, sql_registry, filter);
    std::printf("rows: %zu\n", of_type.size());

    return EXIT_SUCCESS;
}
