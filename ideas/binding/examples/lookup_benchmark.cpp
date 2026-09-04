// Compares the old linear-scan find_field() (O(M*N): re-scans the whole
// FieldList for every one of M lookups) against the new detail::FieldIndex
// (O(N+M): builds a hash index once, then O(1)-average per lookup).
//
// This isolates the actual lookup mechanism from the rest of
// bind_from_fields<T>, since T's field count is fixed at compile time via
// boost::pfr and can't easily be varied to N in a loop -- but the lookup
// itself (what changed) is exactly this: M string-keyed lookups against an
// N-entry FieldList, done M times for M fields being bound.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "binding/config_field.h"
#include "binding/config_bind.h" // pulls in the new detail::FieldIndex

using binding::Field;
using binding::FieldList;

// The old implementation, verbatim, for a fair side-by-side.
bool iequals_old(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}
const Field* find_field_old(const FieldList& fields, std::string_view name) {
    for (const auto& f : fields) {
        if (iequals_old(f.name, name)) return &f;
    }
    return nullptr;
}

FieldList make_field_list(int n, std::mt19937& rng) {
    FieldList fields;
    fields.reserve(n);
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng); // field order shouldn't matter -- shuffle to prove it
    for (int i : order) {
        fields.push_back(Field{"field_" + std::to_string(i), std::string("value_" + std::to_string(i))});
    }
    return fields;
}

int main() {
    std::mt19937 rng(42);
    std::printf("%8s %14s %14s %10s\n", "N", "linear(us)", "indexed(us)", "speedup");

    for (int n : {10, 50, 100, 500, 1000, 5000, 20000, 50000}) {
        FieldList fields = make_field_list(n, rng);

        // Simulate binding a struct with N fields: N lookups, one per field,
        // in a different (also shuffled) order than the FieldList itself --
        // matching the same case-insensitive lookup bind_one_field performs.
        std::vector<int> lookup_order(n);
        for (int i = 0; i < n; ++i) lookup_order[i] = i;
        std::shuffle(lookup_order.begin(), lookup_order.end(), rng);
        std::vector<std::string> names;
        names.reserve(n);
        for (int i : lookup_order) names.push_back("FIELD_" + std::to_string(i)); // deliberately mixed case

        // OLD: linear scan, N times.
        auto t0 = std::chrono::steady_clock::now();
        std::size_t checksum_old = 0;
        for (const auto& name : names) {
            const Field* f = find_field_old(fields, name);
            checksum_old += f ? f->as_leaf().size() : 0;
        }
        auto t1 = std::chrono::steady_clock::now();

        // NEW: build index once, then N O(1)-average lookups.
        auto t2 = std::chrono::steady_clock::now();
        binding::detail::FieldIndex index(fields);
        std::size_t checksum_new = 0;
        for (const auto& name : names) {
            const Field* f = index.first(name);
            checksum_new += f ? f->as_leaf().size() : 0;
        }
        auto t3 = std::chrono::steady_clock::now();

        double old_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        double new_us = std::chrono::duration<double, std::micro>(t3 - t2).count();

        std::printf("%8d %14.1f %14.1f %9.1fx%s\n", n, old_us, new_us, old_us / new_us,
                    checksum_old == checksum_new ? "" : "  MISMATCH!");
    }
}
