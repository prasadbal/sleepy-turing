// DataRow<Struct>: a PFR-reflectable Struct plus a compact presence bitmask
// -- one bit per field, not one std::optional<T> per field. Why a bitmask
// and not std::optional<T> fields on Struct itself: std::optional<T> adds a
// bool (often padded to alignof(T)) PER FIELD; a bitset packs all N
// presence bits into ceil(N/64) words total. For a 190-field row that's
// ~190 bytes of padding-inflated bools vs 24 bytes of bitset.
//
// Why presence needs to be explicit at all, not inferred from "is the field
// zero": for a numeric field, zero-initialized-and-never-set is
// indistinguishable from genuinely-set-to-zero. There is no way to tell
// them apart after the fact, memset or not -- this is a correctness
// requirement, not a performance one. (And for POD numeric fields, the
// memset/default-construct itself is not where the cost is anyway --
// measured, not assumed: see csv_writer.h's header comment.)
#pragma once
#include <boost/pfr.hpp>
#include <bitset>
#include <cstddef>
#include <type_traits>

namespace posreport {

template<class Struct>
class DataRow {
public:
    static constexpr std::size_t field_count = boost::pfr::tuple_size_v<Struct>;

    DataRow() = default;

    template<std::size_t I>
    void set(boost::pfr::tuple_element_t<I, Struct> value) {
        boost::pfr::get<I>(data_) = std::move(value);
        present_.set(I);
    }

    // No get<I>() returning std::optional<T>: that would copy T out into a
    // fresh optional on every call (real cost for a std::string field) when
    // the caller already has has<I>()/has_dyn() + raw() to check presence
    // and read the field in place, with no copy. "Presence" and "the field
    // itself" are two separate, cheap operations -- don't bundle them back
    // into one that forces a copy.
    template<std::size_t I>
    [[nodiscard]] bool has() const noexcept { return present_.test(I); }

    // Runtime-indexed presence check, for a writer that iterates fields via
    // a runtime loop (offset/tag table) rather than compile-time unrolling --
    // std::bitset::test() already takes a runtime index natively.
    [[nodiscard]] bool has_dyn(std::size_t i) const { return present_.test(i); }

    void clear() noexcept { present_.reset(); } // reuse a DataRow for the next row without reconstructing it

    // Raw access for a writer that already knows a field is present (or
    // doesn't care -- e.g. it's about to check has<I>() itself right after).
    [[nodiscard]] const Struct& raw() const noexcept { return data_; }

private:
    Struct                     data_{};
    std::bitset<field_count>   present_;
};

} // namespace posreport
