// Demo for query_descriptor.h: define a query's shape once as a
// QueryDescriptor, then execute it and dump its stats (round trips, bytes,
// PGA/UGA, V$SQL) in one call. Two illustrative descriptors below --
// ObjectsByName (no bind parameters) and ObjectsOfType (one bind parameter)
// -- meant as a pattern to copy for the real 5-6 queries this is for: add a
// struct with bind_type/define_type/sql/key_type at namespace scope (must
// be namespace scope, not inside a function -- see query_descriptor.h) and
// one run_and_dump<YourDescriptor>(...) call in main().
//
//   query_descriptor_demo <connect_string> <user> <password>
//
// With no arguments it runs against the mock (canned data, numbers
// meaningless, only proves the plumbing).
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
    static constexpr std::string_view sql =
        "SELECT object_name, object_type FROM all_objects WHERE ROWNUM <= 2000";
    using key_type = std::index_sequence<0>; // keyed on object_name
};

struct TypeFilter { FixedString<32> object_type; };

// One bind parameter (:object_type, matched to TypeFilter's field by name --
// same binding rule as execute()/select_rows() elsewhere in oci_client.h).
struct ObjectsOfType {
    using bind_type   = TypeFilter;
    using define_type = ObjRow;
    static constexpr std::string_view sql =
        "SELECT object_name, object_type FROM all_objects WHERE object_type = :object_type AND ROWNUM <= 2000";
    using key_type = std::index_sequence<0>;
};

// Runs one descriptor and prints its result shape plus everything QueryMeter
// measured about running it. This is the part to call once per query you're
// investigating; add_and_dump<YourDescriptor>(conn, meter, "label", bind...)
// for each of the 5-6.
template<class Descriptor, class... Bind>
void run_and_dump(OciConnection& conn, QueryMeter& meter, std::string_view label, Bind&&... bind) {
    std::printf("\n########## %.*s ##########\n", static_cast<int>(label.size()), label.data());
    std::printf("sql: %.*s\n", static_cast<int>(Descriptor::sql.size()), Descriptor::sql.data());

    const auto r = run_and_measure<Descriptor>(conn, meter, std::forward<Bind>(bind)...);

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

    OciConnection conn(connect_string, user, password);
    if (!conn.connect()) {
        std::fprintf(stderr, "could not connect to %s\n", connect_string.c_str());
        return EXIT_FAILURE;
    }

    QueryMeter meter(conn);

    run_and_dump<ObjectsByName>(conn, meter, "1: ObjectsByName (no bind)");

    TypeFilter filter;
    filter.object_type.assign("TABLE");
    run_and_dump<ObjectsOfType>(conn, meter, "2: ObjectsOfType (bind :object_type)", filter);

    // Add descriptors 3..6 here the same way:
    //   run_and_dump<YourDescriptor>(conn, meter, "3: <label>"[, your_bind_struct]);

    return EXIT_SUCCESS;
}
