// Tests for posreport/path_resolver.h: dotted-path field access
// (Position.book, ins.isin, db.Table.column, Config.key) via boost::pfr
// name reflection, with no hand-written getter for any ordinary field.
#include <catch2/catch_test_macros.hpp>

#include <posreport/path_resolver.h>

#include <map>
#include <string>

using namespace posreport;

namespace {

struct Instrument {
    std::string isin;
    std::string expiry;
    double      notional;
    std::string swap_leg_type;
};
struct Position {
    std::int64_t sicovam;
    std::string  book;
    std::string  contrepartie;
};

struct Fixture {
    std::map<std::int64_t, Instrument> instruments = {
        {12345, {"FR0000131104", "2027-06-30", 1'000'000.0, "PAY_FIXED"}},
    };
    std::map<std::string, std::map<std::string, Value>> db = {
        {"Desks", {{"description", Value{std::string("Rates Trading EU")}}}},
    };
    std::map<std::string, Value> config = {{"ReportingCurrency", Value{std::string("EUR")}}};
    Resolvers<Position, Instrument> res;
    Position pos{12345, "BOOK_A", "CPTY_X"};

    Fixture() {
        res.instrument_of = [this](const Position& p) -> std::optional<Instrument> {
            auto it = instruments.find(p.sicovam);
            return it != instruments.end() ? std::optional(it->second) : std::nullopt;
        };
        res.db_lookup = [this](std::string_view table, std::string_view column, const Position&) -> std::optional<Value> {
            auto t = db.find(std::string(table));
            if (t == db.end()) return std::nullopt;
            auto c = t->second.find(std::string(column));
            return c != t->second.end() ? std::optional(c->second) : std::nullopt;
        };
        res.config_lookup = [this](std::string_view key) -> std::optional<Value> {
            auto it = config.find(std::string(key));
            return it != config.end() ? std::optional(it->second) : std::nullopt;
        };
    }
};

} // namespace

TEST_CASE_METHOD(Fixture, "path_resolver: Position's own fields, no getter written", "[posreport]") {
    CHECK(evaluate("Position.sicovam", pos, res) == Value{std::int64_t{12345}});
    CHECK(evaluate("Position.book", pos, res) == Value{std::string("BOOK_A")});
    CHECK(evaluate("Position.contrepartie", pos, res) == Value{std::string("CPTY_X")});
}

TEST_CASE_METHOD(Fixture, "path_resolver: ins.<field> follows Position -> Instrument", "[posreport]") {
    CHECK(evaluate("ins.isin", pos, res) == Value{std::string("FR0000131104")});
    CHECK(evaluate("ins.expiry", pos, res) == Value{std::string("2027-06-30")});
    CHECK(evaluate("ins.swap_leg_type", pos, res) == Value{std::string("PAY_FIXED")}); // any field, still no getter
    CHECK(evaluate("Position.ins.isin", pos, res) == Value{std::string("FR0000131104")}); // fully qualified form too
}

TEST_CASE_METHOD(Fixture, "path_resolver: db.Table.column and Config.key", "[posreport]") {
    CHECK(evaluate("db.Desks.description", pos, res) == Value{std::string("Rates Trading EU")});
    CHECK(evaluate("Config.ReportingCurrency", pos, res) == Value{std::string("EUR")});
}

TEST_CASE_METHOD(Fixture, "path_resolver: misses resolve to nullopt, not a crash", "[posreport]") {
    CHECK_FALSE(evaluate("Position.nope", pos, res).has_value());
    CHECK_FALSE(evaluate("ins.nope", pos, res).has_value());
    CHECK_FALSE(evaluate("db.NoSuchTable.x", pos, res).has_value());
    CHECK_FALSE(evaluate("Config.NoSuchKey", pos, res).has_value());
    CHECK_FALSE(evaluate("", pos, res).has_value());
    CHECK_FALSE(evaluate("Position..book", pos, res).has_value());       // empty segment
    CHECK_FALSE(evaluate("Position.book.extra", pos, res).has_value());  // extra segment on a terminal field
    CHECK_FALSE(evaluate("db.OnlyOneSegment", pos, res).has_value());    // db.<table> with no column

    Position orphan{99999, "BOOK_B", "CPTY_Y"}; // sicovam with no instrument master data
    CHECK_FALSE(evaluate("ins.isin", orphan, res).has_value());
    CHECK(evaluate("Position.book", orphan, res) == Value{std::string("BOOK_B")}); // Position's own fields still fine
}
