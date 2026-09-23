// Scratch prototype (not repo): ordered totals over ANY tuple key. The caller
// extracts the key; this only stores, orders and rolls up.
#pragma once
#include <boost/pfr.hpp>
#include <algorithm>
#include <cstddef>
#include <map>
#include <tuple>
#include <utility>

namespace tup {

// Field I of a DB row, whether the row is a plain struct (via Boost.PFR) or a
// tuple-like (std::tuple / pair / array).
template<std::size_t I, class Row>
decltype(auto) field(const Row& r) {
    if constexpr (requires { std::tuple_size<Row>::value; }) return std::get<I>(r);
    else return boost::pfr::get<I>(r);
}

// Key extractor from field indices, coarse -> fine: pick<0, 2, 1>() builds
// std::tuple<F0, F2, F1> (by value) from a row.
template<std::size_t... I>
constexpr auto pick() {
    return [](const auto& r) {
        return std::tuple<std::remove_cvref_t<decltype(field<I>(r))>...>{field<I>(r)...};
    };
}

template<std::size_t N, class Tup>
auto head_ref(const Tup& t) {
    return [&]<std::size_t... J>(std::index_sequence<J...>) { return std::tie(std::get<J>(t)...); }
        (std::make_index_sequence<N>{});
}
template<std::size_t N, class Tup>
auto head_val(const Tup& t) {
    return [&]<std::size_t... J>(std::index_sequence<J...>) {
        return std::tuple<std::remove_cvref_t<std::tuple_element_t<J, Tup>>...>{std::get<J>(t)...};
    }(std::make_index_sequence<N>{});
}

// Orders on the first min(|a|,|b|) elements, so a shorter tuple is a prefix probe.
struct PrefixOrder {
    using is_transparent = void;
    template<class... A, class... B>
    bool operator()(const std::tuple<A...>& a, const std::tuple<B...>& b) const {
        constexpr std::size_t n = std::min(sizeof...(A), sizeof...(B));
        return head_ref<n>(a) < head_ref<n>(b);
    }
};

// Key: any std::tuple whose elements have operator< (std::optional's NULL sorts first).
// Measures: anything with operator+= and a default constructor.
template<class Key, class Measures>
class KeyedTotals {
public:
    using Map = std::map<Key, Measures, PrefixOrder>;

    void add(Key key, const Measures& m) { flat_[std::move(key)] += m; }

    [[nodiscard]] const Map& flat() const { return flat_; }   // leaves, sorted by key

    // Total for any leading run of the key. An empty tuple is the grand total.
    template<class... P>
    [[nodiscard]] Measures subtotal(const std::tuple<P...>& prefix) const {
        Measures t{};
        for (auto it = flat_.lower_bound(prefix), end = flat_.upper_bound(prefix); it != end; ++it) t += it->second;
        return t;
    }
    [[nodiscard]] Measures total() const { return subtotal(std::tuple<>{}); }

    // One level of the hierarchy: L = 1 groups by the first key element, L = 2 by the first two, ...
    template<std::size_t L>
    [[nodiscard]] auto rollup() const {
        std::map<decltype(head_val<L>(std::declval<const Key&>())), Measures, PrefixOrder> out;
        for (const auto& [k, m] : flat_) out[head_val<L>(k)] += m;
        return out;
    }

private:
    Map flat_;
};

} // namespace tup
