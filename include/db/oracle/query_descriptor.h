#pragma once
// A QueryDescriptor bundles everything needed to run one query generically
// and index its results, so investigating "get stats for query N" is a type
// definition, not a new hand-written function each time:
//
//   struct ObjectsByName {
//       using bind_type   = NoBind;                    // no :placeholders
//       using define_type = ObjectRow;                  // the output row shape
//       static constexpr std::string_view query_name = "objects_by_name";
//       using key_type    = std::index_sequence<0>;      // key on define_type's field 0
//   };
//
// query_name names this descriptor's entry in a QuerySqlRegistry
// (query_sql_registry.h) -- the actual SQL text lives in an external XML
// file, not in this struct, so it can be edited without a C++ rebuild.
// bind_type/define_type/key_type stay here: they're structural (what a row
// looks like, how to bind/key it), not something a config file should own.
// run_and_measure() below takes the registry to resolve query_name against
// explicitly, the same way it already takes OciConnection/QueryMeter
// explicitly rather than reaching for global state.
//
// Declare descriptors at namespace scope, not inside a function. A class
// declared inside a function ([class.local]) cannot have a static data
// member, constexpr or not, so a descriptor's `static constexpr query_name`
// cannot be defined inside main() or any other function body.
//
// key_type names which define_type fields (by position, boost::pfr order)
// form the map key that run_and_measure() indexes its results by -- e.g.
// index_sequence<0> keys on the first field alone, index_sequence<0,2> on a
// composite of the first and third. This is the same field-position-based
// key extraction as the aggregation prototype's tup::pick (ideas/aggregate),
// applied here to db result rows instead of in-memory rows.
//
// Built on oci_diag.h's QueryMeter, so running a descriptor doesn't just get
// you the rows -- it gets you the round trips, bytes, PGA/UGA delta and
// V$SQL stats for that run, which is the actual point: figuring out where a
// slow query's time and memory are actually going, not just its result set.
//
// bind_type for a query with no bind parameters: a struct with zero fields.
// scalar_bindable requires boost::pfr::tuple_size_v to be well-formed, which
// an empty struct satisfies (tuple_size_v == 0), and execute()/select_rows()
// with such a struct binds nothing -- see NoBind below.
#include <db/oracle/oci_diag.h>
#include <db/oracle/query_sql_registry.h>

#include <boost/pfr.hpp>
#include <concepts>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace marketlib::db::oracle {

// Bind type for a QueryDescriptor whose SQL has no :placeholders.
struct NoBind {};

namespace detail::qd {
// A key field's stored type. FixedString<N> has neither operator== nor
// operator< (so it can't be a std::map key as-is), and more importantly its
// natural cheap accessor (view()/the implicit string_view conversion) is
// non-owning: the row it came from is the fetch buffer OciStatement reuses
// for the next batch, so a string_view key would dangle the moment more
// rows are fetched. str() copies, which is the only safe choice for a value
// that has to outlive the row it was read from. Every other field type used
// here (arithmetic, OciDate) is already owned by value, so it passes through.
template<class T>
auto to_key_value(const T& v) {
    if constexpr (is_fixed_string_v<T>) return v.str();
    else return v;
}
template<class Define, std::size_t... I>
auto key_of_impl(const Define& row, std::index_sequence<I...>) {
    return std::tuple{to_key_value(boost::pfr::get<I>(row))...};
}
} // namespace detail::qd

// Extracts a descriptor's key tuple from one of its output rows, using its
// key_type's field positions.
template<class Descriptor>
auto key_of(const typename Descriptor::define_type& row) {
    return detail::qd::key_of_impl(row, typename Descriptor::key_type{});
}

// Not named key_t: that collides with the POSIX typedef of the same name
// (<sys/types.h>, System V IPC keys) -- with `using namespace
// marketlib::db::oracle;` in scope, ordinary unqualified lookup finds the
// global ::key_t first and hides this one entirely, silently turning
// `key_t<Descriptor>` into nonsense (::key_t is not a template) rather than
// a clean error. Confirmed the collision is real, not hypothetical, by
// hitting exactly this failure while writing this file's own tests.
template<class Descriptor>
using descriptor_key_t = decltype(key_of<Descriptor>(std::declval<const typename Descriptor::define_type&>()));

template<class Descriptor>
struct QueryRunResult {
    ExecResult                              result;    // whether the query itself succeeded
    QueryStats                              stats;      // round trips / bytes / PGA-UGA / V$SQL, via QueryMeter
    std::vector<DiagProblem>                problems;   // why `stats` may be incomplete
    std::map<descriptor_key_t<Descriptor>, typename Descriptor::define_type> rows; // keyed by Descriptor::key_type
    std::size_t                             row_count = 0; // rows fetched, before dedup by key
};

// Runs Descriptor::query_name's SQL (resolved via `sql_registry`) through
// QueryMeter, binding `bind` (pass a NoBind{} for a query with no
// parameters) and folding every fetched row into `rows` keyed by
// key_of<Descriptor>. A duplicate key overwrites -- last row wins -- same
// "last one seen" semantics as a plain std::map::operator[] would give; if
// a descriptor's key isn't actually unique in the result set, row_count vs
// rows.size() tells you so.
template<class Descriptor>
[[nodiscard]] QueryRunResult<Descriptor> run_and_measure(
    OciConnection& conn, QueryMeter& meter, const QuerySqlRegistry& sql_registry,
    typename Descriptor::bind_type bind = {},
    std::size_t prefetch_rows = 500, std::size_t fetch_batch_size = 200)
{
    QueryRunResult<Descriptor> out;
    const auto m = meter.measure([&] {
        return select_rows<typename Descriptor::bind_type, typename Descriptor::define_type>(
            conn, sql_registry.sql_for(Descriptor::query_name), bind, prefetch_rows, fetch_batch_size,
            [&](const typename Descriptor::define_type* rows, std::size_t n) {
                out.row_count += n;
                for (std::size_t i = 0; i < n; ++i) out.rows[key_of<Descriptor>(rows[i])] = rows[i];
            });
    });
    out.result = m.result;
    out.stats = m.stats;
    out.problems = m.problems;
    return out;
}

// Key type get_map() below uses: the same tuple-of-one shape key_of<>
// produces for a single-field key_type, so a get_map() result and a
// run_and_measure() result with key_type = std::index_sequence<0> are
// keyed identically and comparable.
template<class Define>
using first_field_key_t = decltype(detail::qd::key_of_impl(std::declval<const Define&>(), std::index_sequence<0>{}));

// The subset of QueryDescriptor that get_map() actually needs: an
// association between a row shape and a query name. Every real
// QueryDescriptor already satisfies this (define_type + query_name are two
// of its four members) -- get_map<ObjectsByName>(...) works with the exact
// same struct run_and_measure<ObjectsByName>(...) uses, no new declaration
// required. A one-off query that has no need for run_and_measure()'s
// key_type/bind_type can instead declare just the two members this concept
// asks for.
template<class Query>
concept query_association = requires {
    typename Query::define_type;
    { Query::query_name } -> std::convertible_to<std::string_view>;
};

// A lighter-weight sibling of run_and_measure() for when a query's name is
// already tied to its row shape via `Query` (a QueryDescriptor, or anything
// smaller satisfying query_association) -- so unlike an earlier version of
// this function, the query's name is never a second, separately-typed-out
// runtime argument that could drift from Query::define_type:
//
//   auto rows  = get_map<ObjectsByName>(conn, sql_registry);
//   auto rows2 = get_map<ObjectsOfType>(conn, sql_registry, TypeFilter{.object_type = "TABLE"});
//
// `Bind` is deduced from whatever value is actually passed for `bind` (or
// defaults to NoBind) rather than being a member Query has to declare --
// its type already exists at the call site as the argument itself, so
// naming it a second time on Query would be redundant. The map is keyed on
// Query::define_type's first field only (boost::pfr position 0) -- the
// same key every QueryDescriptor declared so far actually uses (key_type =
// std::index_sequence<0>) -- with no way to ask for a composite key. No
// QueryMeter/stats either: this is for "get me the data", not "investigate
// this query's performance". Reach for run_and_measure() instead when you
// need either.
template<query_association Query, class Bind = NoBind>
[[nodiscard]] std::map<first_field_key_t<typename Query::define_type>, typename Query::define_type>
get_map(OciConnection& conn, const QuerySqlRegistry& sql_registry,
        Bind bind = {}, std::size_t prefetch_rows = 500, std::size_t fetch_batch_size = 200)
{
    using Define = typename Query::define_type;
    std::map<first_field_key_t<Define>, Define> out;
    select_rows<Bind, Define>(conn, sql_registry.sql_for(Query::query_name), bind, prefetch_rows, fetch_batch_size,
                               [&](const Define* rows, std::size_t n) {
                                   for (std::size_t i = 0; i < n; ++i)
                                       out[detail::qd::key_of_impl(rows[i], std::index_sequence<0>{})] = rows[i];
                               });
    return out;
}

} // namespace marketlib::db::oracle
