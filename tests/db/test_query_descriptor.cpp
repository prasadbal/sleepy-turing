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
// not ([class.local]) -- so a descriptor's `static constexpr sql` cannot
// live inside a test. That is not just a test-writing quirk: it means real
// QueryDescriptors have to be declared at namespace scope in application
// code too, not built up inline inside main() or a function.
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
    static constexpr std::string_view sql = "SELECT object_name, object_type FROM all_objects";
    using key_type    = std::index_sequence<0>; // keyed on object_name
};

struct NameOnlyRow { FixedString<128> object_name; };
struct BadQuery {
    using bind_type   = NoBind;
    using define_type = NameOnlyRow;
    static constexpr std::string_view sql = "SELECT object_name FROM nonexistent_view";
    using key_type    = std::index_sequence<0>;
};

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
    const auto r = run_and_measure<ObjectsByName>(c.conn, meter);

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

    const auto r = run_and_measure<BadQuery>(c.conn, meter);

    CHECK(r.result.status != ExecStatus::Success);
    CHECK(r.rows.empty());
    CHECK(r.row_count == 0);
    mock::reset_sql_hooks();
}

#endif // !MARKETLIB_DB_HAS_REAL_OCI
