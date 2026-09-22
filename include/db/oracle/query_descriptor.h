#pragma once
// A QueryDescriptor bundles everything needed to run one query generically
// and index its results, so investigating "get stats for query N" is a type
// definition, not a new hand-written function each time:
//
//   struct ObjectsByName {
//       using bind_type   = NoBind;                    // no :placeholders
//       using define_type = ObjectRow;                  // the output row shape
//       static constexpr std::string_view sql =
//           "SELECT object_name, object_type FROM all_objects WHERE ROWNUM <= 5000";
//       using key_type    = std::index_sequence<0>;      // key on define_type's field 0
//   };
//
// Declare descriptors at namespace scope, not inside a function. A class
// declared inside a function ([class.local]) cannot have a static data
// member, constexpr or not, so a descriptor's `static constexpr sql` cannot
// be defined inside main() or any other function body.
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

#include <boost/pfr.hpp>
#include <cstddef>
#include <map>
#include <string>
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

// Runs Descriptor::sql through QueryMeter, binding `bind` (pass a NoBind{}
// for a query with no parameters) and folding every fetched row into `rows`
// keyed by key_of<Descriptor>. A duplicate key overwrites -- last row wins --
// same "last one seen" semantics as a plain std::map::operator[] would give;
// if a descriptor's key isn't actually unique in the result set, row_count
// vs rows.size() tells you so.
template<class Descriptor>
[[nodiscard]] QueryRunResult<Descriptor> run_and_measure(
    OciConnection& conn, QueryMeter& meter, typename Descriptor::bind_type bind = {},
    std::size_t prefetch_rows = 500, std::size_t fetch_batch_size = 200)
{
    QueryRunResult<Descriptor> out;
    const auto m = meter.measure([&] {
        return select_rows<typename Descriptor::bind_type, typename Descriptor::define_type>(
            conn, std::string(Descriptor::sql), bind, prefetch_rows, fetch_batch_size,
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

} // namespace marketlib::db::oracle
