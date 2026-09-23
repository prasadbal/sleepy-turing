// Prototype: aggregate DB rows by a tuple key, ordered, with prefix subtotals.
// Build and run from the repo root (one line; PFR comes from the FetchContent checkout):
// g++-15 -std=c++23 -Wall -Wextra -isystem build/linux-debug/_deps/pfr-src/include ideas/aggregate/keyed_demo.cpp -o /tmp/keyed_demo && /tmp/keyed_demo
#include "keyed.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct Row {
    std::int64_t               portfolio_id;
    std::int64_t               mvtident;
    std::optional<std::string> contrepartie;   // NULL for listed instruments
    std::int64_t               qty;
    std::int64_t               amount;         // minor units
};
struct Agg {
    std::int64_t qty{}, amount{}, n{};
    Agg& operator+=(const Agg& o) { qty += o.qty; amount += o.amount; n += o.n; return *this; }
    static Agg of(const Row& r) { return {r.qty, r.amount, 1}; }
    bool operator==(const Agg&) const = default;
};

static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) std::exit(1);
}

// In-house counterparties (loaded once from reference data).
static const std::set<std::string, std::less<>> kInternal{"DESK_A", "DESK_B"};
static std::string_view kind_of(const Row& r) {
    if (!r.contrepartie) return "LISTED";
    return kInternal.contains(*r.contrepartie) ? "INTERNAL" : "EXTERNAL";
}

// The key sets. Struct fields by index via PFR; derived columns are a plain lambda.
static const auto by_default = tup::pick<0, 1, 2>();   // portfolio, mvtident, contrepartie
static const auto by_cpty    = tup::pick<2, 0>();      // contrepartie, portfolio
static const auto by_kind    = [](const Row& r) { return std::tuple{kind_of(r), r.portfolio_id, r.contrepartie}; };

template<class R, class KeyFn, class MeasureFn = decltype(&Agg::of)>
static auto aggregate(const std::vector<R>& rows, KeyFn key_fn, MeasureFn measure = &Agg::of) {
    using Key = decltype(key_fn(std::declval<const R&>()));
    tup::KeyedTotals<Key, Agg> t;
    for (const auto& r : rows) t.add(key_fn(r), measure(r));
    return t;
}

// Config picks a key set by name; each entry is a compile-time tuple type. The
// visitor is instantiated for every entry, so it may only use operations that
// don't depend on the key type (total(), flat().size(), ...). Prefix queries
// belong with their own key set.
template<class Visitor>
static bool run_named(std::string_view name, const std::vector<Row>& rows, Visitor&& visit) {
    if (name == "default") { visit(aggregate(rows, by_default)); return true; }
    if (name == "by_cpty")  { visit(aggregate(rows, by_cpty));    return true; }
    if (name == "by_kind")  { visit(aggregate(rows, by_kind));    return true; }
    return false;
}

int main() {
    const std::vector<Row> rows = {
        {1, 101, "CPTY_A",     10, 1000},
        {1, 102, "CPTY_A",      5,  520},
        {1, 103, "CPTY_B",      7,  700},
        {1, 104, std::nullopt,  3,  310},   // listed
        {2, 105, "CPTY_A",      4,  400},
        {2, 106, std::nullopt,  1,  100},
        {1, 101, "CPTY_A",      2,  200},   // second row for mvtident 101
    };

    std::puts("default key (portfolio, mvtident, contrepartie):");
    auto t = aggregate(rows, by_default);
    check(t.flat().size() == 6, "7 rows -> 6 groups");
    check(t.subtotal(std::tuple{std::int64_t{1}}) == Agg{27, 2730, 5}, "subtotal(portfolio 1)");
    check(t.subtotal(std::tuple{std::int64_t{1}, std::int64_t{101}}) == Agg{12, 1200, 2}, "subtotal(portfolio 1, mvt 101)");
    check(t.total() == Agg{32, 3230, 7}, "total() == every row");
    check(t.rollup<1>().at(std::tuple{std::int64_t{2}}) == Agg{5, 500, 2}, "rollup<1> agrees with subtotal");
    const auto cp = aggregate(rows, by_cpty);
    check(std::get<0>(cp.flat().begin()->first) == std::nullopt, "by_cpty: NULL counterparty sorts before any value");

    std::puts("\nkey sets chosen by name (config):");
    const std::vector<Row> deals = {
        {1, 201, "CPTY_A",  10,  1000},   // external
        {2, 202, "CPTY_B",   6,   600},   // external
        {1, 203, "DESK_B",  10,  1000},   // internal leg 1
        {2, 203, "DESK_A", -10,  -990},   // internal leg 2: different amount
        {2, 204, std::nullopt, 1,  100},  // listed
    };
    const auto kt = aggregate(deals, by_kind);   // prefix queries stay with their key set
    check(kt.subtotal(std::tuple{std::string_view{"EXTERNAL"}}) == Agg{16, 1600, 2}, "external-only total = one prefix query");
    check(kt.subtotal(std::tuple{std::string_view{"INTERNAL"}}) == Agg{0, 10, 2},
          "internal legs leave a residual of 10 (a break to surface, not to absorb)");

    const Agg grand{17, 1710, 5};
    for (const char* name : {"default", "by_cpty", "by_kind"}) {
        Agg total{};
        std::size_t groups = 0;
        check(run_named(name, deals, [&](const auto& tt) { total = tt.total(); groups = tt.flat().size(); }), name);
        check(total == grand, "  every key set has the same grand total");
        std::printf("      %-8s %zu groups\n", name, groups);
    }
    check(!run_named("nope", deals, [](const auto&) {}), "unknown key-set name is reported, not guessed");

    std::puts("\nDB row as a std::tuple instead of a struct:");
    using TRow = std::tuple<std::int64_t, std::int64_t, std::optional<std::string>, std::int64_t, std::int64_t>;
    std::vector<TRow> trows;
    for (const auto& r : rows) trows.emplace_back(r.portfolio_id, r.mvtident, r.contrepartie, r.qty, r.amount);
    const auto tt = aggregate(trows, tup::pick<0, 1, 2>(),
                              [](const TRow& r) { return Agg{std::get<3>(r), std::get<4>(r), 1}; });
    check(tt.flat() == t.flat(), "same key extractor, tuple rows -> identical result to struct rows");
    return 0;
}
