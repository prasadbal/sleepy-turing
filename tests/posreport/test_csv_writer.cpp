// Tests for posreport/data_row.h (DataRow<Struct>: PFR struct + presence
// bitmask) and posreport/csv_writer.h (CsvWriter<Struct>: offset+tag-switch
// CSV formatting, row falls back to defaults falls back to zero-value).
#include <catch2/catch_test_macros.hpp>

#include <posreport/csv_writer.h>
#include <posreport/data_row.h>

#include <boost/pfr.hpp>
#include <string>
#include <string_view>

using namespace posreport;

namespace {
struct Row {
    double      a;
    std::int32_t b;
    std::string c;
};
} // namespace

TEST_CASE("DataRow: nothing present on a fresh row", "[posreport][data_row]") {
    DataRow<Row> row;
    CHECK_FALSE(row.has<0>());
    CHECK_FALSE(row.has<1>());
    CHECK_FALSE(row.has<2>());
}

TEST_CASE("DataRow: set<I>() marks only that field present", "[posreport][data_row]") {
    DataRow<Row> row;
    row.set<0>(3.14);
    CHECK(row.has<0>());
    CHECK_FALSE(row.has<1>()); // setting field 0 doesn't touch field 1's presence
    CHECK(boost::pfr::get<0>(row.raw()) == 3.14);
}

TEST_CASE("DataRow: presence is independent of the value being zero", "[posreport][data_row]") {
    DataRow<Row> row;
    row.set<1>(0); // legitimately zero, not "unset" -- the whole point of the bitmask
    CHECK(row.has<1>());
    CHECK(boost::pfr::get<1>(row.raw()) == 0);
}

TEST_CASE("DataRow: string fields work the same way", "[posreport][data_row]") {
    DataRow<Row> row;
    CHECK_FALSE(row.has<2>());
    row.set<2>(std::string("hello"));
    CHECK(row.has<2>());
    CHECK(boost::pfr::get<2>(row.raw()) == "hello");
}

TEST_CASE("DataRow: has_dyn() matches has<I>() for runtime-indexed access", "[posreport][data_row]") {
    DataRow<Row> row;
    row.set<1>(42);
    CHECK(row.has_dyn(0) == row.has<0>());
    CHECK(row.has_dyn(1) == row.has<1>());
    CHECK(row.has_dyn(2) == row.has<2>());
}

TEST_CASE("DataRow: clear() resets all presence for reuse", "[posreport][data_row]") {
    DataRow<Row> row;
    row.set<0>(1.0);
    row.set<1>(2);
    row.set<2>(std::string("x"));
    row.clear();
    CHECK_FALSE(row.has<0>());
    CHECK_FALSE(row.has<1>());
    CHECK_FALSE(row.has<2>());
}

TEST_CASE("CsvWriter: fully-present row formats all fields from itself", "[posreport][csv_writer]") {
    DataRow<Row> row, defaults;
    row.set<0>(1.50);
    row.set<1>(7);
    row.set<2>(std::string("USD"));

    CsvWriter<Row> writer;
    char buf[256];
    const std::size_t n = writer.write_row(row, defaults, buf);
    const std::string_view line(buf, n);
    CHECK(line == "1.50,7,USD\r\n");
}

TEST_CASE("CsvWriter: a field missing from the row falls back to defaults", "[posreport][csv_writer]") {
    DataRow<Row> row, defaults;
    row.set<0>(1.50);
    // field 1 (b) and field 2 (c) left unset in row
    defaults.set<1>(99);
    defaults.set<2>(std::string("EUR"));

    CsvWriter<Row> writer;
    char buf[256];
    const std::size_t n = writer.write_row(row, defaults, buf);
    CHECK(std::string_view(buf, n) == "1.50,99,EUR\r\n");
}

TEST_CASE("CsvWriter: a field missing from BOTH row and defaults falls back to zero-value", "[posreport][csv_writer]") {
    DataRow<Row> row, defaults; // nothing set anywhere
    CsvWriter<Row> writer;
    char buf[256];
    const std::size_t n = writer.write_row(row, defaults, buf);
    CHECK(std::string_view(buf, n) == "0.00,0,\r\n"); // double 0.00, int32 0, empty string
}

TEST_CASE("CsvWriter: max_line_bytes() is a real upper bound on write_row() output", "[posreport][csv_writer]") {
    DataRow<Row> row, defaults;
    row.set<0>(-12345.67);
    row.set<1>(-2147483647);
    row.set<2>(std::string("a somewhat longer counterparty name"));

    CsvWriter<Row> writer;
    std::string buf(writer.max_line_bytes(), '\0');
    const std::size_t n = writer.write_row(row, defaults, buf.data());
    CHECK(n <= writer.max_line_bytes());
}
