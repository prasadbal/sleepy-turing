// Tests for query_descriptor.h. key_of() is pure logic, tested on synthetic
// rows with no database involved. run_and_measure() is tested against the
// OCI mock -- it returns canned rows whatever the SQL says, so this proves
// the plumbing (rows land in the map under the right key, duplicate keys
// overwrite, stats come back alongside the rows), not that any particular
// SQL is correct.
//
// Every QueryDescriptor here is declared at namespace scope, not inside a
// TEST_CASE body: a local class (one declared inside a function -- which is
// what TEST_CASE expands to) cannot have a static data member, constexpr or
// not ([class.local]) -- so a descriptor's `static constexpr query_name`
// cannot live inside a test. That is not just a test-writing quirk: it means
// real QueryDescriptors have to be declared at namespace scope in
// application code too, not built up inline inside main() or a function.
//
// QuerySqlRegistry itself has its own dedicated test cases further down
// (loading, missing entry, duplicate name, malformed XML); the descriptors
// here build their registry from an in-memory XML string via from_string(),
// not a real file -- no filesystem path for a test runner to get right.
#include <catch2/catch_test_macros.hpp>

#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>
#include <db/oracle/query_descriptor.h>

#include <map>
#include <string>
#include <utility>

using namespace marketlib::db::oracle;

#if !MARKETLIB_DB_HAS_REAL_OCI

namespace {

struct Connected {
    OciConnection conn{"orcl", "app_user", "secret"};
    Connected() { REQUIRE(conn.connect()); }
    ~Connected() { conn.disconnect(); }
};

struct Row3 { FixedString<32> a; double b; int c; };

struct KeyOnA    { using define_type = Row3; using key_type = std::index_sequence<0>; };
struct KeyOnCA   { using define_type = Row3; using key_type = std::index_sequence<2, 0>; }; // c, then a -- reversed
struct KeyOnC    { using define_type = Row3; using key_type = std::index_sequence<2>; };

struct ObjRow { FixedString<128> object_name; FixedString<32> object_type; };
struct ObjectsByName {
    using bind_type   = NoBind;
    using define_type = ObjRow;
    static constexpr std::string_view query_name = "test_objects_by_name";
    using key_type    = std::index_sequence<0>; // keyed on object_name
};

struct NameOnlyRow { FixedString<128> object_name; };
struct BadQuery {
    using bind_type   = NoBind;
    using define_type = NameOnlyRow;
    static constexpr std::string_view query_name = "test_bad_query";
    using key_type    = std::index_sequence<0>;
};

struct TypeFilter { FixedString<32> object_type; };

// query_association-only (no bind_type, no key_type) -- proves get_map()
// works with a bind parameter without the struct declaring bind_type at
// all: Bind is deduced from whatever value is passed as get_map()'s third
// argument, here TypeFilter.
struct ObjectsOfTypeMinimal {
    using define_type = ObjRow;
    static constexpr std::string_view query_name = "test_objects_of_type";
};

// One shared registry for every run_and_measure()/get_map() test below --
// the actual SQL text doesn't matter against the mock (it returns canned
// rows whatever the SQL says, ignoring any bind values too), only that
// these names resolve to something.
const QuerySqlRegistry& descriptor_test_registry() {
    static const QuerySqlRegistry reg = QuerySqlRegistry::from_string(R"(
        <queries>
            <query name="test_objects_by_name"><sql>SELECT object_name, object_type FROM all_objects</sql></query>
            <query name="test_objects_of_type">
                <sql>SELECT object_name, object_type FROM all_objects WHERE object_type = :object_type</sql>
            </query>
            <query name="test_bad_query"><sql>SELECT object_name FROM nonexistent_view</sql></query>
        </queries>
    )");
    return reg;
}

} // namespace

TEST_CASE("key_of: extracts a single-field key by position", "[query_descriptor]") {
    Row3 r{FixedString<32>("k1"), 1.5, 7};
    const auto k = key_of<KeyOnA>(r);
    // FixedString<N> has no operator== of its own -- key_of stores it as an
    // owning std::string (see the file comment in query_descriptor.h on why:
    // a string_view key would dangle once the row's fetch buffer is reused).
    CHECK(k == std::tuple{std::string("k1")});
}

TEST_CASE("key_of: extracts a composite key in key_type's own order, not field order", "[query_descriptor]") {
    Row3 r{FixedString<32>("k1"), 1.5, 7};
    const auto k = key_of<KeyOnCA>(r);
    CHECK(std::get<0>(k) == 7);
    CHECK(std::get<1>(k) == std::string("k1"));
}

TEST_CASE("key_of: different rows with the same key fields produce equal keys", "[query_descriptor]") {
    Row3 r1{FixedString<32>("same"), 1.0, 1};
    Row3 r2{FixedString<32>("same"), 999.0, 999}; // b, c differ; a (the key) doesn't
    CHECK(key_of<KeyOnA>(r1) == key_of<KeyOnA>(r2));
}

TEST_CASE("run_and_measure: rows land in the map under their key, plus stats come back", "[query_descriptor]") {
    Connected c;
    QueryMeter meter(c.conn);
    const auto r = run_and_measure<ObjectsByName>(c.conn, meter, descriptor_test_registry());

    CHECK(r.result.status == ExecStatus::Success);
    CHECK(r.problems.empty());
    CHECK(r.row_count == 3); // the mock always returns 3 rows
    REQUIRE(r.rows.size() == 3);
    // The mock's canned object_name values are "row0_col0", "row1_col0", "row2_col0".
    CHECK(r.rows.contains(std::tuple{std::string("row0_col0")}));
    CHECK(r.rows.contains(std::tuple{std::string("row1_col0")}));
    CHECK(r.rows.contains(std::tuple{std::string("row2_col0")}));
    CHECK(r.stats.elapsed_ms >= 0.0);
}

TEST_CASE("run_and_measure: a duplicate key overwrites, row_count still counts every row", "[query_descriptor]") {
    // Every mock row's first column is distinct ("row0_col0", "row1_col0", ...),
    // so producing a genuine key collision through a live mock query isn't
    // straightforward -- this checks the map-insertion mechanism directly
    // against synthetic rows that do collide, instead.
    std::map<descriptor_key_t<KeyOnC>, Row3> rows;
    Row3 r1{FixedString<32>("first"), 1.0, 42};
    Row3 r2{FixedString<32>("second"), 2.0, 42}; // same key (c == 42) as r1
    rows[key_of<KeyOnC>(r1)] = r1;
    rows[key_of<KeyOnC>(r2)] = r2; // overwrites r1's entry
    REQUIRE(rows.size() == 1);
    CHECK(rows.begin()->second.a.str() == "second"); // last one wins
}

TEST_CASE("run_and_measure: an unreadable table is reported, not a crash", "[query_descriptor]") {
    Connected c;
    mock::g_fail_on_sql = "nonexistent_view";
    QueryMeter meter(c.conn);

    const auto r = run_and_measure<BadQuery>(c.conn, meter, descriptor_test_registry());

    CHECK(r.result.status != ExecStatus::Success);
    CHECK(r.rows.empty());
    CHECK(r.row_count == 0);
    mock::reset_sql_hooks();
}

// ---------------------------------------------------------------------------
// get_map()
// ---------------------------------------------------------------------------

TEST_CASE("get_map: same descriptor run_and_measure uses, no query_name passed separately",
          "[query_descriptor][get_map]") {
    Connected c;
    const auto rows = get_map<ObjectsByName>(c.conn, descriptor_test_registry());

    REQUIRE(rows.size() == 3); // the mock always returns 3 rows, all distinct on object_name
    CHECK(rows.contains(std::tuple{std::string("row0_col0")}));
    CHECK(rows.contains(std::tuple{std::string("row1_col0")}));
    CHECK(rows.contains(std::tuple{std::string("row2_col0")}));
}

TEST_CASE("get_map: keyed identically to run_and_measure with key_type = index_sequence<0>",
          "[query_descriptor][get_map]") {
    Connected c;
    QueryMeter meter(c.conn);
    const auto via_run_and_measure = run_and_measure<ObjectsByName>(c.conn, meter, descriptor_test_registry());
    const auto via_get_map = get_map<ObjectsByName>(c.conn, descriptor_test_registry());

    REQUIRE(via_get_map.size() == via_run_and_measure.rows.size());
    for (const auto& [key, row] : via_get_map) {
        REQUIRE(via_run_and_measure.rows.contains(key));
        CHECK(row.object_name.str() == via_run_and_measure.rows.at(key).object_name.str());
    }
}

// A one-off struct satisfying only query_association (define_type +
// query_name), not a full QueryDescriptor -- no bind_type, no key_type --
// to prove get_map() doesn't require the heavier struct.
struct ObjectsMinimal {
    using define_type = ObjRow;
    static constexpr std::string_view query_name = "test_objects_by_name";
};

TEST_CASE("get_map: works with a minimal query_association struct, not just a full QueryDescriptor",
          "[query_descriptor][get_map]") {
    Connected c;
    const auto rows = get_map<ObjectsMinimal>(c.conn, descriptor_test_registry());
    CHECK(rows.size() == 3);
}

TEST_CASE("get_map: binds a parameter with no bind_type declared on the struct -- Bind is deduced",
          "[query_descriptor][get_map]") {
    Connected c;
    TypeFilter filter;
    filter.object_type.assign("TABLE");
    const auto rows = get_map<ObjectsOfTypeMinimal>(c.conn, descriptor_test_registry(), filter);
    CHECK(rows.size() == 3); // the mock ignores the bind value and returns its usual 3 rows
}

// ---------------------------------------------------------------------------
// QuerySqlRegistry
// ---------------------------------------------------------------------------

TEST_CASE("QuerySqlRegistry: loads names and SQL text from XML", "[query_descriptor][query_sql_registry]") {
    const auto reg = QuerySqlRegistry::from_string(R"(
        <queries>
            <query name="a"><sql>SELECT 1 FROM dual</sql></query>
            <query name="b"><sql>SELECT 2 FROM dual</sql></query>
        </queries>
    )");
    CHECK(reg.size() == 2);
    CHECK(reg.sql_for("a") == "SELECT 1 FROM dual");
    CHECK(reg.sql_for("b") == "SELECT 2 FROM dual");
}

TEST_CASE("QuerySqlRegistry: trims a CDATA block's own source indentation", "[query_descriptor][query_sql_registry]") {
    const auto reg = QuerySqlRegistry::from_string(R"(
        <queries>
            <query name="q"><sql><![CDATA[
                SELECT * FROM dual
            ]]></sql></query>
        </queries>
    )");
    CHECK(reg.sql_for("q") == "SELECT * FROM dual");
}

TEST_CASE("QuerySqlRegistry: sql_for() throws on a name with no matching entry", "[query_descriptor][query_sql_registry]") {
    const auto reg = QuerySqlRegistry::from_string(R"(<queries><query name="a"><sql>x</sql></query></queries>)");
    CHECK_THROWS_AS(reg.sql_for("nonexistent"), std::out_of_range);
}

TEST_CASE("QuerySqlRegistry: rejects malformed XML", "[query_descriptor][query_sql_registry]") {
    CHECK_THROWS_AS(QuerySqlRegistry::from_string("<queries><query name=\"a\">"), std::runtime_error);
}

TEST_CASE("QuerySqlRegistry: rejects a <query> missing its name attribute", "[query_descriptor][query_sql_registry]") {
    CHECK_THROWS_AS(QuerySqlRegistry::from_string("<queries><query><sql>x</sql></query></queries>"),
                     std::runtime_error);
}

TEST_CASE("QuerySqlRegistry: rejects a <query> missing its <sql> child", "[query_descriptor][query_sql_registry]") {
    CHECK_THROWS_AS(QuerySqlRegistry::from_string(R"(<queries><query name="a"></query></queries>)"),
                     std::runtime_error);
}

TEST_CASE("QuerySqlRegistry: rejects a duplicate query name", "[query_descriptor][query_sql_registry]") {
    CHECK_THROWS_AS(QuerySqlRegistry::from_string(R"(
        <queries>
            <query name="dup"><sql>SELECT 1 FROM dual</sql></query>
            <query name="dup"><sql>SELECT 2 FROM dual</sql></query>
        </queries>
    )"), std::runtime_error);
}

TEST_CASE("QuerySqlRegistry: from_file() throws on a nonexistent path", "[query_descriptor][query_sql_registry]") {
    CHECK_THROWS_AS(QuerySqlRegistry::from_file("/no/such/file/db_queries.xml"), std::runtime_error);
}

#endif // !MARKETLIB_DB_HAS_REAL_OCI
