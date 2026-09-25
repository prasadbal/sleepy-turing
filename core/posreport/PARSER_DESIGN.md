# Parser implementation: two approaches

Two complete, working implementations of `report_expr.h`'s parser exist in
this project's history: a hand-rolled tokenizer + recursive-descent parser
(the original, `7ba9096`) and a `boost::parser` PEG-combinator grammar (the
current one, `d71be31` onward). Same grammar (see
[`LANGUAGE.md`](LANGUAGE.md) for what it accepts), same `Node` AST, same
`eval()` semantics -- verified identical via the exact same, unmodified
test suite growing from 82 to 100 assertions only because of later,
unrelated additions (`compile()`), not because behavior changed. This
document is for whoever next maintains this file, chooses between these
two strategies for a similar grammar elsewhere, or wants to know why the
code looks the way it does.

## Approach 1: hand-rolled tokenizer + recursive-descent parser

### Structure

A `Token`/`tokenize()` pair does one linear character scan into a flat
`std::vector<Token>`. A `Parser` class then has one method per precedence
level -- `parse_comparison` → `parse_expr` (additive) → `parse_term` →
`parse_unary` → `parse_primary` → `parse_if` / `parse_path_or_call` -- each
manually managing `peek()`/`take()`/`expect()`/`fail()` and directly
constructing `Node` objects as it recognizes them. Parsing and AST
construction are the same pass, same as the current approach -- that part
didn't change between the two.

### Code shape

```cpp
NodePtr parse_expr() { // + -
    auto n = parse_term();
    for (;;) {
        if (peek().kind == Token::Kind::Plus || peek().kind == Token::Kind::Minus) {
            const Op op = take().kind == Token::Kind::Plus ? Op::Add : Op::Sub;
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::BinOp; node->op = op; node->lhs = std::move(n); node->rhs = parse_term();
            n = std::move(node);
        } else break;
    }
    return n;
}
```

### Tradeoffs

- **Zero external dependencies.** Compiles against nothing beyond the
  standard library.
- **Full, direct control.** Every character consumed and every token
  produced is explicit, and the whole thing steps through an ordinary
  debugger with no framework machinery in between -- a breakpoint in
  `parse_term` is exactly where you'd expect it to be.
- **More code.** The tokenizer (`Token`/`tokenize()`) is 61 lines; the
  `Parser` class is 135 lines -- 196 lines of real parsing logic (comments
  and blank lines excluded from all counts in this document).
- **Manual, error-prone bookkeeping.** Precedence climbing, token peeking,
  and string-escape handling are all hand-written, and have to be gotten
  right by hand again for every new construct added -- nothing here
  generalizes automatically.
- **Grammar and code can silently drift apart.** The informal grammar
  comment at the top of the file and the actual parsing methods are two
  separate artifacts; nothing enforces they still describe the same
  language once either one changes.

## Approach 2: `boost::parser` PEG combinator grammar

### Structure

Each grammar production (`comparison`, `additive`, `term`, `unary`,
`primary`, `if_expr`, `path_or_call`, `ident`, `string_lit`) is a
`bp::rule<Tag, Attr>` plus a `_def` object built from combinators (`>>`
sequence, `|` alternation, `*` repetition, `-` optional, `%` delimited
list) with semantic actions (`[...]`) building `Node` objects as each rule
matches. The grammar comment and the code are close enough to be nearly
interchangeable.

### Code shape

```cpp
constexpr auto additive_def =
    term[([](auto& ctx) { _val(ctx) = std::move(_attr(ctx)); })]
    >> *(
        ('+' >> term)[([](auto& ctx) { _val(ctx) = grammar::make_binop(Op::Add, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | ('-' >> term)[([](auto& ctx) { _val(ctx) = grammar::make_binop(Op::Sub, std::move(_val(ctx)), std::move(_attr(ctx))); })]
    );
```

### Tradeoffs

- **Real dependency footprint.** `boost::parser` is not a standalone,
  near-zero-dependency library the way Boost.PFR (already used elsewhere
  in this project) is -- it transitively needs 13 more Boost libraries
  (`hana`, `charconv`, `assert`, `type_index`, `core`, `fusion`, `mpl`,
  `tuple`, `config`, `container_hash`, `throw_exception`, `describe`,
  `mp11`), found by actually grepping every `#include <boost/...>` those
  headers reach and confirmed by real build failures, not assumed
  complete on the first pass. Wired into this project via the full
  `boostorg/boost` super-repo with a restricted `GIT_SUBMODULES` list --
  see `cmake/dependencies.cmake`'s own comment for the exact set.
- **Real, ongoing compile-time cost.** `hana`'s template metaprogramming is
  slow to compile on every compiler, and dramatically slower on MSVC
  specifically -- a single translation unit including these headers took
  20+ minutes on MSVC during this work, versus low tens of seconds on GCC
  and Clang. This is a per-build cost for anyone touching this file, or
  anything that includes it, not a one-time setup cost paid once and
  forgotten.
- **Genuinely less code, once built correctly.** 81 lines of lexical +
  grammar-rule code, versus 196 for the hand-rolled tokenizer + parser --
  about 59% smaller. "Once built correctly" is doing real work in that
  sentence; see the next point.
- **Real, non-obvious debugging cost.** Three separate, library-version-
  specific limitations were found and had to be worked around while
  building this (each documented in `report_expr.h`'s own header comment,
  which is the authoritative, most current source for these -- summarized
  here, not duplicated in full):
  1. A rule directly referencing itself as the first parser in its own
     alternative (a naive `'-' >> unary | primary` for unary minus)
     silently returns a placeholder ("nope"), not the real attribute, on
     the nested self-call -- a real, documented behavior of this
     library's recursion handling, not a mistake in the grammar as
     written.
  2. A `%` list (1-or-more) combined with `|` or `-(...)` to express
     "zero or more" hits a copy-not-move bug merging a move-only AST
     node vector across the alternation/optional boundary, specific to
     this library version.
  3. Every rule and action needs `constexpr`, or the grammar crashes on
     any *recursive* rule -- while still producing **correct results for
     entirely flat, non-recursive input**. That specific failure shape
     (simplest test cases pass, building false confidence, until the
     grammar recurses even once) makes it a particularly easy trap to
     walk into and a slow one to diagnose from symptoms alone.

  A fourth, *believed* limitation -- that an action nested inside a
  repetition, reading and writing the enclosing rule's value across
  iterations, doesn't work -- turned out on retest to be a misdiagnosis:
  it was caused by a mismatched `boost::parser` header version in the
  build environment, not a real limitation of the pattern at all (see
  `term_def`'s own comment for the retest that overturned it). Worth
  remembering as a general caution with this class of library: a
  confusing template error can come from the build environment as easily
  as from the grammar itself, and a belief formed while chasing one
  should be retested once the real cause is found, not assumed
  permanently true.
- **Grammar and code stay closer together going forward.** Adding a new
  operator or construct means adding one rule/action in the same shape as
  every existing one, not a new hand-written recursive method with its
  own bespoke token bookkeeping.

## Side by side

| | hand-rolled | `boost::parser` |
|---|---|---|
| Runtime behavior | identical | identical |
| Parsing-specific code (tokenizer/lexical + parser/grammar) | 196 lines | 81 lines (~59% less) |
| Total file size | 419 lines | 447 lines (more comments documenting the gotchas above, not more logic) |
| External dependencies | none | 14 transitive Boost libraries |
| Compile time added to this translation unit | negligible | seconds (GCC/Clang) to 20+ minutes (MSVC) |
| Diagnosing a grammar bug | ordinary step debugging | can require reading library internals or isolating against a minimal reproduction; three real, version-specific gotchas found here, one false one ruled out |
| Adding a new grammar construct | a new hand-written recursive method | a new declarative rule, same shape as the existing ones |

## Which to choose

For a grammar this size, expected to stay roughly this size, where
zero-dependency builds and ordinary step-through debugging matter more
than line count -- the hand-rolled version is a completely reasonable,
lower-risk choice, and was the right call under the original time
pressure it was built under (see `7ba9096`'s own commit message).

For a grammar expected to *grow* -- more operators, more constructs added
over time -- the combinator version's tighter grammar-to-code
correspondence and smaller per-construct footprint pay for themselves
faster the more the grammar changes, provided the compile-time cost
(especially on MSVC) and the dependency footprint are acceptable for the
project as a whole, not just for this one file.

Both are real, tested, and available in git history; this rewrite replaced
the hand-rolled version rather than keeping both, since keeping two
parsers for the one language would itself be a maintenance cost with no
corresponding benefit -- unlike `eval()`/`compile()` (see below), where
keeping both is deliberate because they answer genuinely different
questions at different call sites.

## See also

- [`LANGUAGE.md`](LANGUAGE.md) -- the grammar itself, for anyone writing
  expressions, not implementing the parser.
- `report_expr.h`'s own header comment -- the authoritative, most
  up-to-date list of the library-specific gotchas summarized above.
- `eval()` vs. `compile()` in `report_expr.h` -- a separate,
  *evaluation*-strategy decision (tree-walking interpreter vs.
  closure-compiled evaluator), not a parsing decision, with its own
  independently measured tradeoffs documented at that function's
  definition. Deliberately not covered in this document, which is about
  building the parser specifically.
