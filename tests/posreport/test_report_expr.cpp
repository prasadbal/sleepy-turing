// Tests for posreport/report_expr.h: the one-liner expression language on
// top of path_resolver.h -- numbers, strings, +-*/, comparisons, IF, and
// FUNC.name(args) against a caller-supplied function table.
#include <catch2/catch_test_macros.hpp>

#include <posreport/report_expr.h>

#include <cmath>
#include <map>
#include <string>

using namespace posreport;

namespace {

struct Instrument { std::string isin; double notional; std::string ccy; };
struct Position { std::int64_t sicovam; std::string book; double qty; };

std::optional<double> as_num(const Value& v) {
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* i = std::get_if<std::int64_t>(&v)) return double(*i);
    return std::nullopt;
}

struct Fixture {
    std::map<std::int64_t, Instrument> instruments = {{1, {"FR1", 2'000'000.0, "EUR"}}};
    Resolvers<Position, Instrument> res;
    FuncTable<Position, Instrument> funcs;
    Position pos{1, "BOOK_A", 5.0};

    Fixture() {
        res.instrument_of = [this](const Position& p) -> std::optional<Instrument> {
            auto it = instruments.find(p.sicovam);
            return it != instruments.end() ? std::optional(it->second) : std::nullopt;
        };
        funcs["round"] = [](const std::vector<Value>& a) -> std::optional<Value> {
            if (a.size() != 2) return std::nullopt;
            const auto v = as_num(a[0]), p = as_num(a[1]);
            if (!v || !p) return std::nullopt;
            const double scale = std::pow(10.0, *p);
            return Value{std::round(*v * scale) / scale};
        };
        funcs["max"] = [](const std::vector<Value>& a) -> std::optional<Value> {
            if (a.size() != 2) return std::nullopt;
            const auto x = as_num(a[0]), y = as_num(a[1]);
            if (!x || !y) return std::nullopt;
            return Value{std::max(*x, *y)};
        };
    }

    std::optional<Value> run(std::string_view expr) { return eval(*parse(expr), pos, res, funcs); }
};

bool near(const std::optional<Value>& v, double expect, double eps = 1e-9) {
    if (!v) return false;
    const auto d = as_num(*v);
    return d && std::fabs(*d - expect) < eps;
}

} // namespace

TEST_CASE_METHOD(Fixture, "report_expr: operator precedence and grouping", "[posreport]") {
    CHECK(near(run("2 + 3 * 4"), 14.0));      // not 20: * binds tighter than +
    CHECK(near(run("(2 + 3) * 4"), 20.0));    // parens override precedence
    CHECK(near(run("10 - 2 - 3"), 5.0));      // left-associative
    CHECK(near(run("2 * 3 + 4 * 5"), 26.0));
    CHECK(near(run("-5 + 3"), -2.0));         // unary minus
    CHECK(near(run("-(2 + 3)"), -5.0));       // unary minus over a parenthesized group
    CHECK(near(run("10 / 4"), 2.5));          // division always promotes to double
}

TEST_CASE_METHOD(Fixture, "report_expr: paths inside arithmetic", "[posreport]") {
    CHECK(near(run("ins.notional * 2"), 4'000'000.0)); // resolves through Position -> Instrument
    CHECK(near(run("Position.qty * 100"), 500.0));
    CHECK(near(run("ins.notional / 1000000"), 2.0));
}

TEST_CASE_METHOD(Fixture, "report_expr: FUNC.name(args)", "[posreport]") {
    CHECK(near(run("FUNC.round(ins.notional / 1000000, 2)"), 2.0));
    CHECK(near(run("FUNC.max(Position.qty, 10)"), 10.0));
    CHECK(near(run("FUNC.round(3.14159, 2) + 1"), 4.14));
    CHECK_FALSE(run("FUNC.nosuchfunc(1)").has_value()); // unknown function -> nullopt, not a crash
}

TEST_CASE_METHOD(Fixture, "report_expr: string literals", "[posreport]") {
    CHECK(run("\"EUR\"") == Value{std::string("EUR")});
    CHECK(run("ins.ccy") == Value{std::string("EUR")}); // still a path, unaffected by string literals
}

TEST_CASE_METHOD(Fixture, "report_expr: comparisons", "[posreport]") {
    CHECK(run("2 + 3 == 5") == Value{true});  // comparison binds looser than + -
    CHECK(run("2 == 3") == Value{false});
    CHECK(run("ins.ccy == \"EUR\"") == Value{true});
    CHECK(run("ins.ccy == \"USD\"") == Value{false});
    CHECK(run("ins.notional > 1000000") == Value{true});
    CHECK(run("3 <= 3") == Value{true});
    // Not comparable is nullopt, not false: "can't tell" isn't "not equal".
    CHECK_FALSE(run("2 == \"2\"").has_value());
    CHECK_FALSE(run("ins.ccy < \"USD\"").has_value()); // string ordering is out of scope: nullopt, not a crash
}

TEST_CASE_METHOD(Fixture, "report_expr: IF(cond, then, else)", "[posreport]") {
    CHECK(near(run("IF(1 < 2, 10, 20)"), 10.0));
    CHECK(near(run("IF(1 > 2, 10, 20)"), 20.0));
    CHECK(run("IF(ins.ccy == \"EUR\", \"is euro\", \"not euro\")") == Value{std::string("is euro")});
    CHECK(near(run("IF(1 < 2, 10, 20) + 5"), 15.0));
    CHECK_FALSE(run("IF(2 == \"2\", 10, 20)").has_value()); // unresolvable condition -> nullopt, picks neither branch
}

TEST_CASE_METHOD(Fixture, "report_expr: IF genuinely short-circuits", "[posreport]") {
    // The untaken branch calls a function that always fails to resolve. IF
    // is a language primitive specifically so only the taken branch is ever
    // evaluated (a FUNC call, by contrast, evaluates every argument first) --
    // if that were violated, these would come back nullopt instead of a value.
    CHECK(near(run("IF(1 < 2, 10, FUNC.nosuchfunc(1))"), 10.0));
    CHECK(near(run("IF(1 > 2, FUNC.nosuchfunc(1), 20)"), 20.0));
}

TEST_CASE_METHOD(Fixture, "report_expr: syntax errors are rejected, not silently misparsed", "[posreport]") {
    auto rejects = [](std::string_view expr) {
        try { (void)parse(expr); return false; } catch (const std::exception&) { return true; }
    };
    CHECK(rejects("2 +"));                  // trailing operator
    CHECK(rejects("(2 + 3"));                // unclosed paren
    CHECK(rejects("2 3"));                   // missing operator between operands
    CHECK(rejects("Position.book(1)"));      // a non-FUNC path is not callable
    CHECK(rejects("FUNC.round(1,"));         // unclosed call
    CHECK(rejects("2 $ 3"));                 // unknown character
    CHECK(rejects("2 +   * 3"));             // two operators in a row
    CHECK(rejects("\"unterminated"));        // unterminated string literal
    CHECK(rejects("1 < 2 < 3"));             // chained comparison rejected, not silently reinterpreted
    CHECK(rejects("IF(1 < 2, 10)"));         // IF with too few arguments
    CHECK(rejects("IF 1 < 2, 10, 20)"));     // IF without its opening '('
}
