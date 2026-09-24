# posreport

A dotted-path expression language for report columns derived from
`Position`/`Instrument`/db/config objects (`report_expr.h`,
`path_resolver.h`), plus a CSV row writer for the output side
(`data_row.h`, `csv_writer.h`). This file is about the writer's dispatch
design specifically -- why it's shaped the way it is, not a style choice.

## Why a plain `for` loop can't format a heterogeneous struct's fields

The obvious-looking code doesn't compile:

```cpp
for (std::size_t i = 0; i < boost::pfr::tuple_size_v<Struct>; ++i) {
    format_field(boost::pfr::get<i>(s)); // error: i is not usable in a constant expression
}
```

`boost::pfr::get<I>` needs `I` as a genuine `template<std::size_t I>`
non-type parameter, so the compiler can give the call a different static
return type per field (`double` for field 3, `std::string` for field 7).
A `for` loop's `i` can never serve that role, no matter how predictable
its range is -- it's a runtime value as far as the type system is
concerned, even though a human can see exactly what values it takes.

Trying to force it by declaring the loop variable itself `constexpr`
fails even more directly, for a reason that has nothing to do with PFR:

```cpp
for (constexpr std::size_t i = 0; i < N; ++i) { ... }
//                                       ^^^ error: cannot assign to variable
//                                           'i' with const-qualified type
```

`constexpr` means the value is fixed forever at compile time; a loop
needs its counter to *change* every iteration. "Never changes" and "the
thing that changes every iteration" can't be the same variable -- this
isn't a workaround-able syntax limitation, it's a direct contradiction in
what's being asked for.

## What actually gives you a compile-time-varying index

`std::index_sequence<I...>` plus a fold expression, or equivalently
template recursion / `if constexpr` chains. Instead of one variable that's
somehow both constant and iteration-varying (impossible), you get `I` as
a template parameter -- genuinely constant, but *different* at each of N
separately-generated call sites:

```cpp
template<std::size_t I>
char* fold_field(const DataRow<Row>& row, const Row& defaults, std::uint64_t row_idx, char* p) {
    const auto& v = row.has<I>() ? boost::pfr::get<I>(row.raw()) : boost::pfr::get<I>(defaults);
    return format_value(v, row_idx, I, p);
}
template<std::size_t... I>
std::size_t fold_format_row_impl(const DataRow<Row>& row, const Row& defaults, std::uint64_t row_idx,
                                  char* line, std::index_sequence<I...>) {
    char* p = line;
    ((p = fold_field<I>(row, defaults, row_idx, p), *p++ = ','), ...);
    return static_cast<std::size_t>(p - line);
}
std::size_t fold_format_row(const DataRow<Row>& row, const Row& defaults, std::uint64_t row_idx, char* line) {
    return fold_format_row_impl(row, defaults, row_idx, line, std::make_index_sequence<190>{});
}
```

This is real, correct C++, and it's the natural way to write "do this for
every field" once a plain loop is ruled out. It is also the version that
turned out to be badly pathological on this project's actual target
compiler.

## Why the fold-expression version is slow -- measured, not assumed

Getting a compile-time-varying index has a cost: `fold_field<I>` is a
**distinct template instantiation per field**, so the compiler sees 190
separate (if similar-looking) function bodies, not one function called
190 times. Whether it inlines all 190 into the caller or emits them
separately, that's 190x the code footprint of a single shared function --
and MSVC's inliner, given this exact shape, fully inlines everything into
one enormous function.

Confirmed by generating and reading actual assembly (`cl /Fa`), not by
guessing: `fold_format_row_impl<0..189>` compiles to **2,902 machine
instructions**; the offset+tag-switch version below (`tag_format_row`)
compiles to **78**. A ~37x difference, for functionally equivalent
per-row work. Across a million-row loop, the ~2,900-instruction function
can't stay resident in L1 instruction cache; the ~78-instruction one
comfortably does, and stays hot across the whole loop instead of forcing
a cache miss on every row.

## Four strategies, measured on this project's three real compilers

190-field struct, 1M rows, ns/field:

| strategy | GCC 15 | Clang 21 | MSVC 19.51 |
|---|---|---|---|
| A) fold expression (above) | 41.7 ns | 19.5 ns | **123.0 ns** |
| B) function-pointer table (built once, runtime loop, but still one distinct function per field) | 40.6 ns | 23.1 ns | **131.1 ns** |
| C) offset + tag switch (precomputed offset/type table, one shared function, `switch` on a runtime tag) | 18.0 ns | 20.1 ns | 19.0 ns |
| D) `std::variant` + `std::visit` (same precomputed table, typed dispatch instead of a raw `void*`+tag `switch`) | 17.2 ns | 20.5 ns | 20.6 ns |

A and B both instantiate/emit code per field (190 distinct bodies) and
both pay for it specifically on MSVC -- B (a runtime-looped table of
function pointers) is not meaningfully better than A (full inlining)
despite *looking* more like "real runtime dispatch," because the 190
function bodies it points to are still 190 separate compiled blocks. C
and D both build their per-field metadata once (instance-independent,
reused across every row this `CsvWriter` ever formats) and dispatch
through **one shared function**, called from a genuine runtime loop --
that's the property that actually matters, not whether the dispatch
mechanism "looks like" it's doing more or less work per field.

D was chosen over C for the real `csv_writer.h`: it costs nothing
material (within measurement noise on GCC/Clang, ~8% on MSVC) and
replaces hand-written `reinterpret_cast`/`memcpy`-per-tag code with
compiler-enforced typed access -- `std::visit` hands each formatter an
honestly-typed value, so there's no cast left to get wrong the way the
original `Tag::StdString` case did (real bug, found by testing, see
`csv_writer.h`'s own header comment and git history). C and D are not
competing performance strategies at this point; they're the same
performance strategy with and without that safety property.

## The takeaway

"The type is known at compile time" (true, for every field, via PFR) and
"the generated code is one small function reused across a real loop"
(what actually wins on MSVC) are in direct tension once the loop count
gets large. Acting on the first property fully (A, B) costs 6x-7x on
MSVC. Deliberately *not* acting on it at the call-site level -- recovering
the type via a runtime tag or variant inside one shared function instead
-- is what keeps the generated code small enough to stay cache-resident,
and is worth doing even though it looks, at the source level, like
throwing away information the compiler already had.
