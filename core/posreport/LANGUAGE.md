# Report column expression language

A one-line expression language for defining report columns as derived
values: numbers, strings, arithmetic, comparisons, an `IF`, and calls into
a caller-supplied function table -- built on top of `Position`/`Instrument`/
database/config field access. This is the language itself, for anyone
writing column expressions in a config file; see
[`report_expr.h`](../../include/posreport/report_expr.h) for the C++
parser/evaluator this file's grammar corresponds to, and
[`path_resolver.h`](../../include/posreport/path_resolver.h) for how a
dotted path actually resolves to a value.

## Grammar

EBNF, `"..."` for literal text, `[...]` optional, `{...}` zero-or-more,
`|` alternatives:

```ebnf
expr        = comparison ;

comparison  = additive [ comparison-op additive ] ;
comparison-op
            = "==" | "!=" | "<=" | ">=" | "<" | ">" ;
            (* exactly zero or one -- "a < b < c" is a syntax error, not
               chained left-to-right the way some languages read it *)

additive    = term { ( "+" | "-" ) term } ;
term        = unary { ( "*" | "/" ) unary } ;
unary       = { "-" } primary ;

primary     = NUMBER
            | STRING
            | path
            | "(" expr ")"
            | "IF" "(" expr "," expr "," expr ")"
            ;

path        = IDENT { "." IDENT } [ "(" [ arg-list ] ")" ] ;
arg-list    = expr { "," expr } ;

NUMBER      = digit { digit } [ "." digit { digit } ] ;
STRING      = '"' { ESCAPE | any-char-except('"', '\') } '"' ;
ESCAPE      = "\" ( '"' | "\" ) ;
IDENT       = ( letter | "_" ) { letter | digit | "_" } ;

digit       = "0".."9" ;
letter      = "a".."z" | "A".."Z" ;
```

Whitespace (space and tab, not newline) is insignificant between tokens.

**Operator precedence, tightest to loosest:** unary `-` › `*` `/` › `+` `-`
› comparison. Parentheses override precedence as usual.

**A `path` is a function call** exactly when it has exactly two segments,
the first is literally `FUNC`, and it's followed by `(...)` -- e.g.
`FUNC.round(x, 2)`. Any other path followed by `(` is a syntax error
(paths aren't callable; `FUNC` is the one reserved first segment that
means "look this up in the function table" rather than "resolve this as a
field").

## What a path resolves against

A dotted path's first segment picks which root it's read from (see
`Resolvers<Position, Instrument>` in `path_resolver.h` for how each root
is wired up by the caller). `Position.` is optional, not a required root
keyword the way the other three are -- it's stripped if present and falls
through to the exact same "plain field name" case a bare, unprefixed name
already uses, so `Position.book` and `book` name the same value:

| First segment | Resolves against | Example |
|---|---|---|
| *(none)* or `Position.` | a field on the position struct directly, by name | `book`, `Position.book` -- identical |
| `ins` | the position's instrument (via `Resolvers::instrument_of`), then one field on it by name | `ins.notional`, `ins.ccy` |
| `db.<Table>.<column>` | a database lookup (via `Resolvers::db_lookup`) -- `Table`/`column` are literal strings, not struct field names | `db.Desks.description` |
| `Config.<key>` | a config lookup (via `Resolvers::config_lookup`) | `Config.ReportingCurrency` |
| `FUNC.<name>(args...)` | the caller-supplied function table | `FUNC.round(x, 2)` |

Field access on `Position`/the resolved `Instrument` never needs a
hand-written getter -- it goes through `boost::pfr`'s compile-time name
reflection, so any field declared on either struct is automatically
addressable by its real C++ name, nothing to register or keep in sync.

Any resolution step that fails (unknown field name, no instrument found
for this position, no such config key, unknown function, a path shaped
like `ins.notional.extra` that goes one level deeper than that root
supports) makes the *whole expression* evaluate to "no value"
(`std::nullopt`), not a crash and not a default -- a report column with no
answer for a given row is a real, representable outcome, not an error
condition to special-case at every call site.

## `IF` is a language keyword, not a function

```
IF(condition, then-expr, else-expr)
```

Only the taken branch is ever evaluated -- `IF(1 < 2, 10, FUNC.some_call_that_would_fail(1))`
evaluates to `10` without ever attempting the `FUNC` call in the untaken
branch. This is real short-circuit evaluation, which is exactly why `IF`
is built into the grammar instead of being just another entry in the
function table: a function call's arguments are always evaluated eagerly
before the function runs, so a function-based `if` would evaluate *both*
branches on every row regardless of the condition -- wrong whenever the
untaken branch isn't even valid unless the condition doesn't hold, not
just wasteful.

## Types and comparisons

A resolved value is one of: a 64-bit integer, a double, a string, a bool,
or nothing (`monostate`, meaning no value). Arithmetic promotes
integer-with-integer to integer (except `/`, which always promotes to
double -- silent integer-division truncation is a footgun a report column
shouldn't hit without asking for it). `==`/`!=` compare like-kind values
(number-vs-number with int/double promotion, string-vs-string exact
match, bool-vs-bool); comparing across kinds (a number against a string,
say) is "not comparable" and evaluates to no value, **not** `false` --
"can't tell" and "not equal" are different answers, and only one of them
is true here. `<`/`<=`/`>`/`>=` are numeric-only; there's no string or
bool ordering in this language (yet).

## Examples

```
ins.notional * 2
ins.notional / 1000000
FUNC.round(ins.notional / 1000000, 2)
FUNC.max(Position.qty, 10)
ins.ccy == "EUR"
IF(ins.ccy == "EUR", "is euro", "not euro")
IF(ins.notional > 1000000, FUNC.round(ins.notional / 1000000, 1), ins.notional) + 0
db.Desks.description
Config.ReportingCurrency
```

## Syntax errors

`parse()` throws `std::runtime_error` for anything that doesn't match the
grammar above -- a trailing operator, an unclosed paren, two operators in
a row, a chained comparison (`1 < 2 < 3`), calling a non-`FUNC` path,
an unterminated string, an unknown character, `IF` with the wrong number
of arguments. This is a startup-time / config-load-time failure (a
malformed column expression in a config file), not something propagated
as a value through every row -- see `report_expr.h`'s own comment on why
it throws instead of returning `std::expected` the way the rest of this
project's hot-path code does.

## Using this from C++

```cpp
#include <posreport/report_expr.h>

auto ast = posreport::parse("ins.notional * FUNC.fx_rate(ins.ccy)");
// ast is parsed ONCE, at config-load time -- keep it, don't reparse per row

posreport::Resolvers<Position, Instrument> resolvers = /* wire up instrument_of/db_lookup/config_lookup */;
posreport::FuncTable<Position, Instrument> funcs;
funcs["fx_rate"] = [](const std::vector<posreport::Value>& args) -> std::optional<posreport::Value> {
    /* ... */
};

for (const auto& pos : positions) {
    std::optional<posreport::Value> v = posreport::eval(*ast, pos, resolvers, funcs);
    // v is nullopt if any part of the expression couldn't resolve for this row
}
```
