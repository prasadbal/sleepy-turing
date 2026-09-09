# binding (idea)

One `boost::pfr`-based reflection core, shared between two binding jobs that
turn out to be the same problem: mapping a flat struct (and `vector<S>` of
them) onto an external, name/position-addressed record format. Config
sections are one instance of that; Oracle OCI bind/row structs are another.

Not wired into the top-level build yet -- `boost::pfr` isn't on the
project's approved-libraries list. This is scaffolding to decide whether
that's worth doing, not a committed dependency.

**The OCI client (`oci_client.h`/`oci_connection.h`) was rewritten from a
considerably larger version of itself** -- collections, dynamic IN-lists,
LOB, and an automatic reconnect-and-retry wrapper have all been removed in
favor of a small core plus one classified result instead of automatic
retry. All of the removed code is still in git history. `FixedString<N>`
and `OciDate` have since been added back on top of the rewrite, live-verified
against a real database (see "Testing against a real database" below);
`OciTimestamp`, LOB, and `std::string` output columns have not, and
`oci_lob.h`/the `OciTimestamp` half of `oci_datetime.h` exist only as
standalone types today -- wiring one in is a deliberate, separate decision
for each, not an oversight.

## Layout

- `include/binding/reflect.h` -- the shared engine: `struct_field_auditor<T, Predicate>`
  walks `T`'s fields via `boost::pfr::tuple_size` / `tuple_element_t` and checks
  each one against a `Predicate`. `flat_schema<T>` is this engine with a
  leaf-only predicate (arithmetic or string-convertible, no pointers/const,
  optionally wrapped in `std::optional<U>` to mark it nullable) -- use it for
  config-section structs.
- `include/binding/oci_compat.h` -- picks the real `<oci.h>` if it's on the
  include path, otherwise falls back to `oci_mock.h`. Never both at once.
- `include/binding/oci_mock.h` -- a compile-only OCI stand-in (not a real
  client) so this can be exercised without an Oracle install. Fixes several
  bugs from an earlier draft of this file: an undefined `ub1` typedef, a
  mistyped `ODI_DTYPE_LOB` instead of `OCI_DTYPE_LOB`, a malformed lambda,
  and a missing `OCIErrorGet` (the connection class needs it to tell a
  dropped session apart from an ordinary SQL error).
- `include/binding/oci_lob.h` -- `OciClob`/`OciXml` wrapper types.
- `include/binding/oci_fixed_string.h` -- `FixedString<N>` (`OciChar<N>` /
  `OciVarchar2<N>`), a fixed-capacity character buffer. This is the type a
  CHAR/VARCHAR2 column binds *and* defines through: `N` is the maximum output
  buffer size OCI needs before it knows how long a value is, and the
  characters sit inline in the row struct at a fixed stride, which is what
  both `OCIDefineArrayOfStruct` and `OCIBindArrayOfStruct` need. `std::string`
  can satisfy neither, which is why it stays an input-only bind type.
  `oci_client.h`'s `scalar_bindable<T>` recognizes `FixedString<N>` (plain
  or `std::optional<FixedString<N>>`) as a bindable field, both directions --
  live-verified against a real database, see "Testing against a real
  database" below.
- `include/binding/oci_datetime.h` -- `OciDate` (a DATE column, `SQLT_ODT`)
  and `OciTimestamp` (a TIMESTAMP column, `SQLT_TIMESTAMP`). `OciDate` wraps
  the real 7-byte `::OCIDate` struct directly, with no descriptor or
  allocation, so (like an arithmetic field or `FixedString<N>`) it needs no
  special-casing at all in `details/oci_client.h` beyond its `OciTypeBinder`
  entry -- `scalar_bindable<T>` recognizes it (plain or
  `std::optional<OciDate>`), live-verified against a real database the same
  way `FixedString<N>` was.
  `OciTimestamp` is different, and is **not** wired in: its `OCIDateTime*`
  is a per-value descriptor (allocated via `OCIDescriptorAlloc`, populated via
  `OCIDateTimeConstruct`), the same shape as `OciClob`/`OciXml`'s locator
  -- so it's bind-side only (`execute()`/`insert()`, single-row), not
  select()-able and not bulk-bindable, exactly like a LOB field. Use
  `OciDate` for a plain calendar date (e.g. a COB/business date, which
  has no meaningful time-of-day); reach for `OciTimestamp` only when
  sub-day precision genuinely matters. Neither models fractional seconds
  or timezones yet.

  Getting a value from/to text goes through `from_text(conn, value,
  format = {}, language = {})` and `to_text(conn, format = {}, language = {})`
  on both types, against an *arbitrary* Oracle format model (`"YYYYMMDD"`,
  `"DD/MM/YYYY HH24:MI:SS"`, ...) via `OCIDateFromText`/`OCIDateToText`
  (`OciDate`) or `OCIDateTimeFromText`/`OCIDateTimeToText`/
  `OCIDateTimeGetDate`/`GetTime` (`OciTimestamp`) -- Oracle's own
  client-side format-model interpreter. `format` left empty falls back to
  `set_default_format()`'s value; each type has its own, independent
  default, seeded with Oracle's real out-of-the-box format (`"DD-MON-RR"`
  for `OciDate`, `"DD-MON-RR HH.MI.SS AM"` for `OciTimestamp`) so an
  unconfigured call behaves the same as a bare `TO_DATE(text)`/
  `TO_CHAR(date_col)` would against a fresh session -- a call after
  `set_default_format("")` (or any other empty override) throws rather than
  silently guessing a shape. `OciTimestamp::from_text`/`to_text` build a
  throwaway `OCIDateTime` descriptor to parse into or render from, since
  `OciTimestamp` itself stores plain fields, not a live descriptor, same as
  its numeric constructor. **Does not need a live connection/session** in
  the network sense -- these OCI calls take only `OCIError*` (`OCIEnv*` too,
  for the `DateTime` pair), never `OCISvcCtx*`, so nothing round-trips to
  the server; `conn` just has to be `connect()`ed already, since that is
  the only place this codebase allocates those handles. Does *not* change
  `OciTimestamp`'s bulk-insert restriction below -- a format-string-parsed
  value is exactly as locator-based as any other `OciTimestamp`.

  There used to be a second, parallel way to do this: a hand-rolled
  constructor (`OciDate(std::string_view)`/`OciTimestamp(std::string_view)`)
  and `to_string()`, parsing/rendering Oracle's default formats in plain
  C++ with no connection involved at all. It has been removed -- it
  duplicated logic the real OCI client already implements correctly
  (including the RR century-rollover rule) and diverged from it the moment
  a site's actual `NLS_DATE_FORMAT` wasn't the out-of-the-box default. One
  path now handles every format, default or explicit, correct or not:
  `from_text()`/`to_text()` above. A caller that needs a hardcoded literal
  value with no text involved at all still has the numeric constructor
  (`OciDate(year, month, day, ...)`), unaffected by any of this.
- `include/binding/oci_connection.h` -- `OciConnection`: owns the OCI
  handles, `connect()`/`disconnect()`, `is_disconnect_error()`, and
  `execute(stmt, iters)` -- runs an already-prepared, already-bound
  statement against this session and classifies the result as `Success`,
  `ConnectionLost`, or `QueryError` (see "Running a statement" below). No
  retry logic lives here or anywhere else in this file.
- `include/binding/oci_client.h` -- free functions, not a class: `execute()`
  (no bind, or a bind-parameter struct), `select_rows()` (with or without
  an input struct), and `insert_rows()` (a chunked array bind of a
  `vector<T>`), all built on `scalar_bindable<T>` -- every field arithmetic,
  `FixedString<N>`, or `OciDate`, each optionally wrapped in `std::optional<U>`
  to mark it nullable (except in `insert_rows()`, which rejects `optional<U>`
  fields entirely -- see "Running a statement" below). Still no `std::string`,
  LOB, `OciTimestamp`, collections, or IN-lists.
- `examples/main.cpp` -- execute() with no bind struct, execute() with a
  bind struct (one field `std::optional`), select_rows() with and without
  an input struct, a NULL output column coming back as `nullopt`,
  `prefetch_rows` and `fetch_batch_size` as independent numbers, `FixedString<N>`/
  `OciDate` bind and select (plain and `std::optional`), and the two
  failure classifications -- including the caller reconnecting and calling
  execute() again by hand after a `ConnectionLost` result, since nothing
  here does that automatically.
- `include/binding/config_field.h` -- `Field`/`FieldList`: a parser-independent
  (name, value) tree that config parsing converts into, before it ever meets
  a user struct.
- `include/binding/ptree_bridge.h` -- `from_ptree()`, the one place
  `boost::property_tree` is named: converts a parsed `ptree` (e.g. from
  `read_xml`) into a `FieldList`.
- `include/binding/config_bind.h` -- `bind_from_fields<T>()`: binds a
  `FieldList` onto a `config_schema<T>` struct by field *name* (see below),
  matched case-insensitively.
- `examples/config_demo.cpp` -- parses a small XML config with attributes,
  a nested attribute-only element, three repeated elements, mixed-case keys,
  and a field absent from the XML; a self-referential tree (a `Node` whose
  children are more `Node`s, 4 levels deep); then shows the
  missing-required-field error path.
- `examples/lookup_benchmark.cpp` -- timing comparison of the old
  linear-scan field lookup against the current indexed one (see "Field
  lookup" below); always built with optimizations on regardless of overall
  build type.

## Running a statement

`oci_client.h`'s free functions are named as SQL verbs:

- **`execute(conn, sql)`** -- runs any statement that returns no rows and
  needs no bind parameters: DDL, or DML that's fully literal in the text.
- **`execute(conn, sql, params)`** -- same, binding `params`'s fields by
  name (see below). A field declared `std::optional<U>` binds SQL NULL
  when it's empty.
- **`select_rows(conn, sql, prefetch_rows, fetch_batch_size, on_batch)`** --
  runs a `SELECT` with no bind parameters, fetches its rows in batches of
  up to `fetch_batch_size`, and calls `on_batch(rows_pointer, count)` once
  per batch. Column order in `sql`'s `SELECT` list must match `OutT`'s
  declared field order -- `OCIDefineByPos` is the only column-output bind
  API in raw OCI, so this side stays positional regardless of the bind
  side's by-name binding.
- **`select_rows(conn, sql, input, prefetch_rows, fetch_batch_size, on_batch)`**
  -- same, but also binds `input`'s fields as named parameters first (e.g. a
  `WHERE` clause) -- the read-side counterpart to `execute(conn, sql, params)`.
- **`insert_rows(conn, sql, rows, chunk_size)`** -- a real Oracle array bind
  of `rows` (`std::vector<T>&`), executed in chunks of at most `chunk_size`
  rows per `OCIStmtExecute` call rather than one call for the whole vector.
  `optional<U>` fields are rejected at compile time here -- every row's
  optional would need to be engaged, with no per-row NULL indicator
  tracked in this path at all; use `execute(conn, sql, row)` in a loop for
  a row type with a nullable field. Unlike `select_rows()`'s
  `prefetch_rows`/`fetch_batch_size`, there is no server-side write-ahead
  mechanism to decouple round trips from `chunk_size` -- see "Bottom line"
  below for what that costs, measured.

`prefetch_rows` and `fetch_batch_size` are two independent numbers, not one:
`prefetch_rows` sets `OCI_ATTR_PREFETCH_ROWS`, Oracle's own client-side
round-trip batching -- this is what actually keeps network round trips low.
`fetch_batch_size` sets how many rows this code processes per
`OCIStmtFetch2`/`OCIAttrGet` call, and how big the batch buffer (plus any
optional field's staging/indicator arrays, sized to `fetch_batch_size`) is.
They don't have to match. A large `prefetch_rows` with a small
`fetch_batch_size` is a reasonable choice when the batch's destination
doesn't benefit from being handed large chunks at once -- inserting into a
`std::map`, say, where each element is its own O(log n) insertion
regardless of batch size, unlike a `std::vector`'s amortized bulk insert.
"Insert into a container" isn't a separate code path here at all -- it's
just what `on_batch` does; `examples/main.cpp`'s Demo 3 collects into a
`std::vector` purely inside its own callback.

There is no `insert(vector<T>&)` bulk array-bind and no batch-fetch-into-
`vector<T>` convenience wrapper in this rewrite. Both existed in the
earlier, larger version of this file (verified live against a real
database: 2000 rows array-inserted in one call, a 20,000-row batch-fetched
result checksum-matched) and are straightforward to bring back on top of
what's here now -- `select_rows`'s `on_batch` callback is exactly the seam
a `vector`-filling wrapper would sit behind.

## Binding: by name for parameters, by position for result columns

`execute()`'s parameters bind **by name** (`OCIBindByName`): each field
binds to a `:field_name` placeholder using its own (compiler-derived) name,
e.g. field `bonus_pct` binds `:bonus_pct` wherever that placeholder occurs
in the SQL text -- in any order, and even if it occurs more than once (a
bind placeholder's "position" is the Nth *distinct* placeholder in order of
first appearance, not the Nth occurrence -- `WHERE a = :1 OR b = :1` is one
bind, reused, not two).

`select_rows()`'s result columns bind **by position** (`OCIDefineByPos`):
column order in the `SELECT` list must match `OutT`'s declared field order.
This isn't a design choice made here -- raw OCI simply has no
`OCIDefineByName`; binding an output column is only ever positional in
classic OCI, regardless of what `boost::pfr` can do.

## Running a statement, and why there's no retry loop

`OciConnection::execute(stmt, iters)` runs an already-prepared,
already-bound statement once and classifies the result:

- `Success` -- `OCIStmtExecute` returned `OCI_SUCCESS`.
- `ConnectionLost` -- it didn't, and `is_disconnect_error()` (reads the
  ORA-code off the error handle via `OCIErrorGet`, checks it against a
  small table of known "session is gone" codes -- ORA-03113, ORA-01012,
  ORA-00028, ...) says the session is gone.
- `QueryError` -- anything else: bad SQL, a constraint violation, no data
  found. Retrying this reproduces the exact same failure, so nothing here
  tries.

That's the entire policy. There is no automatic reconnect, no sleep, no
retry count -- a caller that gets `ConnectionLost` back decides for itself
whether and how to reconnect (`disconnect()`/`connect()` are still exactly
what it would call) and whether to call the same `execute()`/`select_rows()`
again. `examples/main.cpp`'s Demo 7 shows this explicitly: a `ConnectionLost`
result, then the caller reconnecting and calling `execute()` a second time
by hand.

The earlier version of this file had `OciConnection::run_with_reconnect()`,
a lambda-based wrapper doing all of the above automatically, with a fixed
retry count and sleep interval. It's gone -- the policy question ("how many
times, how long to wait, log it how") belongs to the caller, not baked into
the library as one fixed answer that was never going to be right for every
caller.

`select_rows()`'s fetch loop gets the same treatment: a failing
`OCIStmtFetch2` mid-batch is classified exactly like a failing execute --
`is_disconnect_error()` doesn't care which OCI call actually failed.

### connect() uses OCILogon2, and checks every call

`connect()` uses `OCILogon2` -- one call that replaces the long-hand
`OCIHandleAlloc(SERVER)` + `OCIServerAttach` + `OCIHandleAlloc(SVCCTX)` +
... + `OCISessionBegin` sequence for the plain username/password case this
class needs (no connection pooling, no external authentication), and hands
back a ready `OCISvcCtx*` in one round trip. Every call it makes
(`OCIEnvCreate`, the error handle's `OCIHandleAlloc`, `OCILogon2`) is
status-checked, tearing down via `disconnect()` on the first failure
instead of continuing on with a handle from a call that never happened.
`disconnect()` is correspondingly just `OCILogoff` plus freeing the env and
error handles -- the `OCIServer`/`OCISession` handles `OCILogon2` manages
internally never need to be held or freed here at all.

`OCIEnvCreate` uses `OCI_DEFAULT`, not `OCI_OBJECT` -- the earlier version
needed `OCI_OBJECT` for the collection-bind feature's object cache
(`OCIType`/`OCIObjectNew`/`OCICollAppend`); with that feature gone, so is
the need for it.

## NULL handling (std::optional<U>)

A field declared `std::optional<U>` (U arithmetic) maps to a nullable
column:

- **`execute()`**: an empty optional binds SQL NULL (indicator
  `OCI_IND_NULL`); a set one binds `*field` with indicator `OCI_IND_NOTNULL`.
- **`select_rows()`**: a NULL column comes back as `std::nullopt`;
  otherwise the fetched value is wrapped in the optional.

Neither direction can bind/define straight into the optional's own storage:
dereferencing an empty `std::optional` to get `&*opt` is undefined behavior,
and there's no standard-sanctioned way to get the address of its unset
storage either. So each optional field gets a real, addressable staging `U`
(`detail::in_staging_t<T>` on the bind side, `detail::out_staging_t<T>` --
one `std::vector<U>` sized to the batch -- on the fetch side) that OCI
actually binds/defines against, plus an `sb2` indicator (`OCI_IND_NULL` /
`OCI_IND_NOTNULL`), one per row for a batch fetch.

**Only `std::optional` fields get an indicator at all.** A plain
(non-optional) field defines with `indp = nullptr` -- legal in OCI, and it
means exactly what it sounds like: no NULL detection for that column. If
the database unexpectedly returns NULL for a field the struct declared as
non-optional, OCI leaves that row's slot holding whatever was already
there (the previous fetch's value, or a default-constructed value on the
very first row) -- silently, no error.

This is a real, deliberate trade, not an oversight, and it's worth being
clear about which way it cuts: an earlier version of this file gave
*every* field a real indicator regardless of nullability, specifically so
a NULL landing on a "shouldn't be nullable" column would throw instead of
silently propagating a stale/garbage value -- a genuine schema-mismatch
safety net. That's gone here. What it buys back: the indicator array for a
batch fetch only needs to be sized to the number of `optional` fields, not
every field -- for a wide row with few nullable columns, a real reduction
in the bookkeeping `select_rows()` carries per batch. Restoring the safety
net later is a small, separate change (give every plain field a real, if
throwaway, indicator too, checked once and discarded) -- not a different
design, just more of this one.

## Config binding (FieldList -> struct)

`config_field.h` defines the shape parsed config data takes before it meets a
struct:

```cpp
struct Field {
    std::string name;
    std::variant<std::string, FieldList> value;   // leaf, or a nested struct
};
using FieldList = std::vector<Field>;
```

`ptree_bridge.h`'s `from_ptree()` converts a `boost::property_tree::ptree`
(e.g. from `read_xml`) into this shape -- it's the only file that names
`boost::property_tree` at all, so a TOML/INI/JSON source could produce the
same `FieldList` without anything downstream caring. Two things it does on
XML's behalf: attributes come back from `xml_parser` nested one level down
under a synthetic `<xmlattr>` child, which `from_ptree()` flattens into the
parent level; and an attribute-only self-closing element like
`<pool size="10"/>` becomes a one-field nested struct
(`{pool: {size: "10"}}`), not a bare leaf, since collapsing it further would
lose the attribute's own name.

Two things `from_ptree()` used to get wrong, both fixed:

- **An element with attributes *and* its own text** (`<host port="5432">db1</host>`)
  lost the text entirely: the node isn't `empty()`, so only the nested-struct
  branch ran and `db1` was silently dropped. The text is now kept inside the
  nested struct under the reserved name `binding::kTextFieldKey` (`"#text"`,
  which no XML element can be named). Nothing binds it to a struct field yet
  -- deciding that convention is a separate call -- but it is no longer lost.
- **Whitespace.** `read_xml` keeps a value's surrounding indentation, and
  `std::from_chars` rejects leading whitespace outright, so a config
  pretty-printed as `<threads>\n    8\n  </threads>` failed as "not a valid
  number" and only single-line values happened to work. Leaf text is trimmed
  in the bridge (an XML document's indentation is formatting, not data), and
  `parse_leaf_value` trims again before `from_chars` so any other `FieldList`
  source gets the same treatment.

`reflect.h`'s `config_schema<T>` extends `flat_schema` to also allow a field
that's a nested struct, or a `std::vector<U>` of one (the repeated-element
case) -- including a genuinely self-referential tree, where `U` is `T`
itself:

```cpp
struct Node {
    std::string name;
    int id;
    std::vector<Node> node; // same type -- a tree node whose children are more nodes
};
static_assert(binding::config_schema<Node>); // holds
```

This only works because the per-field check is deliberately *shallow*: it
verifies a nested-struct/`vector<U>` field's element type is *some* plain
aggregate (`is_bindable_struct_v<U>`), without recursively re-verifying
*that* type's own fields as part of computing `T`'s value. An eager,
fully-recursive version of this check was tried first and is a real
compiler error for a self-referential `T`, not a slow-but-working one --
confirmed directly: `struct_field_auditor<Node, config_field_predicate>::value`
needs its own already-computed value to compute itself
(`error: 'value' used in its own initializer`), because checking whether
`Node` is valid requires already knowing whether `Node` is valid. There's
no way to eagerly compute one compile-time boolean that validates every
level of a self-referential tree up front.

So the actual per-level verification is deferred to where it happens
naturally: `bind_from_fields<T>()` is templated on `config_schema<T>`, and
its nested-struct/`vector<U>` handling recursively calls `bind_from_fields<U>`.
For `U == T` (the self-referential case), that's the *same* function
template instantiation calling itself -- ordinary runtime recursion bounded
by however deep the actual `FieldList` tree is, not a second compile-time
instantiation of anything. The cost: a malformed *nested* struct is now
only caught when `bind_from_fields` actually recurses into it, not
immediately at a `static_assert(config_schema<Outer>)` on the containing
type -- still a compile error, just one level of indirection further from
where you'd see it with a fully eager check.

The field is named `node`, not `children` -- but there's nothing special
about that spelling, and no reserved name anywhere in this binder. A
repeated child element isn't a distinct concept in the `FieldList` model
(see above), it's just several `Field` entries sharing a name, and
`bind_from_fields` matches a field to entries by that field's own
(compiler-reflected) name. So the field just has to be named the same as
whatever tag is actually repeated -- here that tag happens to be `<node>`
nested under `<node>`, so the field is `node`; a tree using `<item>` tags
instead would need a field named `item`, and so on. Confirmed directly:
renaming both the tag and the field to `item` in an otherwise-identical
tree reproduces the exact same binding result.

`config_bind.h`'s `bind_from_fields<T>(fields, out)` walks `T`'s fields by
name (see "Binding: by name for parameters, by position for result
columns" above -- name matching is necessary here, not just convenient) and,
per field: an absent `std::optional` leaf becomes `nullopt`; a present leaf
is parsed via `std::from_chars` (rejecting partial matches like `"10abc"`)
or taken as-is for `std::string`; a nested struct field recurses into the
one matching same-named `Field`; a `std::vector<U>` field recurses into
*every* same-named `Field`, in order. A missing required field, or a value
of the wrong shape (a leaf where a struct was expected, or vice versa),
throws `std::runtime_error` naming the offending field -- config errors are
a fail-fast-at-startup case, unlike the OCI side's error handling, so this
doesn't try to be exception-free.

`bind_flat_fields<T>()` is the same binder restricted to `flat_schema<T>`
instead of the more permissive `config_schema<T>` -- use it where a struct
is meant to stay strictly flat (a plain DB row/record shape being the
common case), so a nested or `vector<U>` field added to it later is a
compile error right there, instead of silently being accepted.

### Field lookup: an index built once, not a linear scan per field

Each of `T`'s M fields needs to find its matching `Field` in an N-entry
`FieldList`. The obvious implementation -- linearly scan `fields` for every
one of `T`'s fields -- is O(M·N), and an earlier version of this file did
exactly that. `bind_from_fields()` now builds one `detail::FieldIndex` (a
case-insensitive `name -> vector<const Field*>` hash map, the vector since
a repeated element means several entries can share a name) over `fields`
once per call -- O(N) -- and looks up each of `T`'s fields in it -- O(1)
average each -- for O(N+M) overall instead of O(M·N).

`examples/lookup_benchmark.cpp` measures the two lookup mechanisms directly
(not the whole binder, since `T`'s field count is fixed at compile time and
can't be varied in a loop -- but the lookup is exactly what changed):

```
       N     linear(us)    indexed(us)    speedup
      10            1.8            2.1       0.9x
      50           25.2            5.6       4.5x
     100          118.1           11.4      10.3x
     500         2728.4           63.4      43.1x
    1000        11845.1          132.2      89.6x
    5000       263011.0          745.5     352.8x
   20000      3447508.8         3179.9    1084.2x
   50000     27163654.8        12151.8    2235.4x
```

Two honest things this shows, not just "faster":

- **Below roughly N=10-20, the linear scan wins.** Building a hash map has
  real fixed overhead (allocating buckets, hashing every string) that a
  handful of string comparisons doesn't need to pay. For a small,
  flat config section this is the realistic case, and the difference
  either way is a couple of microseconds -- noise next to actually reading
  the file off disk.
- **The crossover is sharp and then dominant.** By N=1000 the indexed
  version is ~90x faster; by N=50000 it's ~2200x, because linear scan is
  genuinely quadratic (11.8ms -> 27.2s as N goes from 1000 to 50000, a
  2300x increase for a 50x increase in N -- squares to 2500x, matching)
  while the indexed version stays close to linear.

For a typical config file (tens of fields), this change doesn't matter --
config loading isn't a hot path, and both numbers round to "fast." It
starts to matter for a `FieldList` with hundreds-to-thousands of entries
(a large repeated-element section, or many sibling config sections at one
nesting level), where the old linear-scan version would visibly slow down
config loading and the indexed one won't.

## What's deliberately not here

This rewrite started over with a small, scalar-only core; everything below
either never made it back in, or was removed along with the larger version
of this file it came from (still in git history):

- Collections and dynamic `IN (...)` lists of any kind -- a struct field
  that's itself a container, and the standalone Oracle-collection-object
  bind (`oci_collection_bind.h`, deleted). IN-clause support is a later,
  separate feature built on query-text rewriting, not something woven into
  the scalar bind/fetch path.
- `std::string`, LOB (`OciClob`/`OciXml`), and `OciTimestamp` as bindable
  fields -- `FixedString<N>` and `OciDate` *are* wired in now (see the
  Layout bullets above and "Testing against a real database" below);
  these three still aren't. The type definitions all exist
  (`oci_lob.h`/`oci_datetime.h`) and work standalone; none of them
  currently register an `OciTypeBinder` in `oci_client.h`, so a struct
  using one won't satisfy `scalar_bindable` at all. Wiring one back in
  means adding its `OciTypeBinder` specialization and its bind/define
  special-casing to `details/oci_client.h` -- the same shape of change
  `FixedString<N>`/`OciDate` just went through, one type at a time rather
  than all at once. `OciTimestamp` specifically needs more than that: its
  `OCIDateTime*` descriptor has no fixed-stride representation, so it can
  never join the array bind/fetch path `FixedString<N>`/`OciDate` use --
  it would need its own single-row-only bind path, the way the earlier,
  larger version of this file had one.
- A read-side counterpart to `insert_rows()`: batch-fetch straight into a
  `vector<T>` rather than through a callback. Existed and was live-verified
  in the earlier version -- `select_rows()`'s `on_batch` callback is the
  seam it would sit behind; filling a `vector<T>` is a few lines inside
  that callback today (see Demo 3 in `examples/main.cpp`), not a separate
  code path.
- A NULL landing on a field that isn't `std::optional` going undetected --
  see "NULL handling" above for the trade and how to reverse it.
- The strict, every-field-checked NULL safety net the earlier version had
  (throwing on an unexpected NULL, not just silently keeping a stale
  value) -- same section.
- Transaction/commit handling (`OCITransCommit` / `OCI_COMMIT_ON_SUCCESS`)
  -- left to the caller, as it always has been here.
- Exact decimal. Every numeric field binds as `SQLT_INT`/`SQLT_UIN`/
  `SQLT_BDOUBLE`, i.e. binary. Oracle `NUMBER` is decimal with up to 38
  digits, so a value that has to reconcile to the cent should go through
  `OCINumber`/`SQLT_VNU` (or be fetched as text) rather than a `double`.
- Automatic retry/reconnect of any kind -- see "Running a statement, and
  why there's no retry loop" above; this is the central decision of this
  rewrite, not an omission.
- Making `oci_datetime.h`'s `from_text()`/`to_text()` reachable from a bind
  struct directly -- `OciDate` fields bind/select fine (see above), but
  getting one from/to an arbitrary format string is still a separate step
  (`OciDate::from_text(conn, text)`) before/after it touches a bind
  struct, not something `execute()`/`select_rows()` does for you inline.
  `OciTimestamp` still isn't a bindable field at all (see above), so this
  applies doubly there.
- Non-`std::string` string-like leaf types in `config_bind.h`'s
  `parse_leaf_value` (only `std::string` and arithmetic types are handled;
  a custom string-view-convertible type would satisfy `is_bindable_leaf`
  but hit a `static_assert` here) -- unrelated to this rewrite, still true.
- Wiring `Config` (the project's existing TOML-based config class in
  `core/config`) up to any of this -- unrelated to this rewrite, still true.
- An ad hoc, struct-free positional bind interface (something like
  `select_rows(conn, sql, results, args...)`, each `args...` element
  binding at `:1, :2, ...` in pack order) for one-off queries where
  defining a whole named struct is overkill. Discussed, not built.

## Compiling

Verified end to end on this host: `g++ 15.2` / C++20 / Boost 1.91, both
demos (`binding_demo` for OCI, `config_demo` for XML config -- collection_demo
is gone along with the feature it demoed), via `cmake -S -B` / `--build`
(using `CMakeLists.txt`, pointed at `/mnt/c/local/boost_1_91_0` through
`BOOST_ROOT`) and via a direct `g++` invocation with
`-Wall -Wextra -Wpedantic -Wshadow` (clean except for one warning inside
Boost's own `core_name20_static.hpp`, unrelated to this code). `config_demo`
additionally needs `boost::property_tree`, which the FetchContent fallback
(standalone `pfr` only) doesn't provide -- it only builds when
`BINDING_BOOST_INCLUDE_DIR` resolves to a real local Boost install.

This rewrite (the scalar-only `oci_client.h`/`oci_connection.h`, plus
`FixedString<N>`/`OciDate` support added on top of it) has since been
live-verified against a real Oracle database -- see "Testing against a
real database" below for exactly what was checked and how to reproduce it.

## Testing against a real database

`examples/live_oracle_demo.cpp`, `examples/live_oracle_disconnect_demo.cpp`,
`examples/live_oracle_fetch_benchmark.cpp`,
`examples/live_oracle_insert_benchmark.cpp`, and
`examples/live_oracle_insert_saturation_benchmark.cpp` run against a real
Oracle instance rather than the mock -- none of them are part of the normal
CMake build (there's no Oracle client in the default build environment), so
each has its own compile command in its header comment.

### Measuring round trips: the query, not a guess

Every round-trip number in this section comes from Oracle's own server-side
accounting, not from timing or from inferring it out of configuration.
`V$SESSTAT` is a per-session statistics table; `'SQL*Net roundtrips to/from
client'` is a counter the server increments itself, internally, at the TTC
(Two-Task Common) protocol layer, every time it registers a round trip on
that session -- not something reconstructed from packets or guessed from
elapsed time:

```sql
SELECT ss.VALUE
FROM V$SESSTAT ss
JOIN V$STATNAME sn ON ss.STATISTIC# = sn.STATISTIC#
WHERE sn.NAME = 'SQL*Net roundtrips to/from client'
  AND ss.SID = SYS_CONTEXT('USERENV','SID')
```

Usage: read it once immediately before the operation you care about, once
immediately after, and the delta is the round-trip count for exactly what
ran in between (see `read_roundtrips()` in any of the `live_oracle_*`
files below for the exact pattern -- it's the same few lines in each).
`ss.SID = SYS_CONTEXT('USERENV','SID')` scopes it to *your own* session,
so it's safe to run on a shared instance without picking up other
sessions' traffic. Needs `SELECT` on `V$SESSTAT`/`V$STATNAME` -- typically
available to any account, but a locked-down application user might not
have it.

This is a different vantage point from a packet-level proxy or sniffer,
not a competing way of producing the same number: `V$SESSTAT` gives an
authoritative *count* for free, with no protocol parsing, but nothing about
per-round-trip latency or byte volume, and nothing you can use to inject
artificial delay or drops. A proxy has to work harder to get a count that
agrees with this one at all -- Oracle Net (TNS) packets are framed with
their own length header, and one logical round trip can span multiple TCP
segments depending on SDU size, so naively counting `send()`/`recv()` calls
or raw segments will overcount; you'd need to parse TNS packet boundaries,
or at minimum track distinct write-then-read cycles, to match what this
query reports. Good cross-check either way: this query's delta should equal
whatever a packet-level tool reports for the same operation.

Verified end to end against `gvenzl/oracle-free:23` (docker container
`oracle-free`, `ORACLE_PASSWORD=BindingTest123`, port 1521, service
`FREEPDB1`) using Oracle Instant Client 19.32 (basic + SDK):

- `live_oracle_demo.cpp`: creates its own table, inserts 37 rows one at a
  time (scalars, `std::optional<double>`, `FixedString<16>`,
  `std::optional<FixedString<8>>`, `OciDate`, `std::optional<OciDate>`, with
  NULLs on some rows for each optional field), then fetches them back with
  `fetch_batch_size=10` against a real server -- confirmed the fetch loop
  ran exactly 4 real `OCIStmtFetch2` round trips (10+10+10+7), row count
  matched, and every field of every row matched what was inserted,
  including every NULL landing in the right place. Also confirmed a
  malformed statement classifies as `QueryError`.
- `live_oracle_disconnect_demo.cpp`: a second connection reads the first
  connection's own SID/SERIAL# from `V$SESSION` and runs
  `ALTER SYSTEM KILL SESSION ... IMMEDIATE` against it -- a real killed
  session, not a simulated one. The first connection's next `execute()`
  correctly came back `ExecStatus::ConnectionLost` (`ORA-03113`
  underneath), confirming `is_disconnect_error()`'s classification against
  an actual dropped session, not just the mock's `FailureMode::DisconnectThenRecover`.
- `live_oracle_fetch_benchmark.cpp`: fetches 500,000 rows (generated
  server-side via a Cartesian-join row source, not through this library's
  own insert path -- see the file's header comment for why) under varying
  `prefetch_rows`/`fetch_batch_size`, reading actual round-trip counts back
  from the server's own `V$SESSTAT` ("SQL*Net roundtrips to/from client")
  rather than inferring them. Three sweeps, one run each, on this machine
  (numbers are machine/network-dependent -- the *shape* of each result is
  the point, not the exact milliseconds):

  | prefetch | fetch_batch_size | elapsed_ms | rows/sec | roundtrips |
  |---:|---:|---:|---:|---:|
  | 20000 | 1 | 226.9 | 2,203,498 | 26 |
  | 20000 | 100 | 176.5 | 2,833,241 | 26 |
  | 20000 | 1000 | 169.0 | 2,959,435 | 25 |
  | 20000 | 5000 | 156.1 | 3,203,271 | 22 |
  | 20000 | 20000 | 151.7 | 3,296,294 | 15 |

  Sweep A (`fetch_batch_size` varies, `prefetch_rows` fixed at 20000):
  `fetch_batch_size=1` -- 500,000 individual `OCIStmtFetch2` calls -- still
  only cost 26 round trips, about the same as `fetch_batch_size=20000`'s 15.
  There's a real ~50% throughput gap between the extremes (pure client-side
  per-call overhead, no round-trip difference to speak of), confirming
  `fetch_batch_size` genuinely doesn't control round trips at this scale.

  | prefetch | fetch_batch_size | elapsed_ms | rows/sec | roundtrips |
  |---:|---:|---:|---:|---:|
  | 50 | 1000 | 308.2 | 1,622,379 | 502 |
  | 500 | 1000 | 367.6 | 1,360,312 | 502 |
  | 5000 | 1000 | 196.9 | 2,539,519 | 85 |
  | 50000 | 1000 | 170.6 | 2,930,444 | 11 |

  Sweep B (`prefetch_rows` varies, `fetch_batch_size` fixed at 1000): round
  trips drop 502 -> 11 as `prefetch_rows` goes from 50 to 50000. **Correction
  to the first cut of this analysis**: this is *not* simply "`prefetch_rows`
  controls round trips, independent of `fetch_batch_size`" -- checking the
  four numbers above against `total_rows / prefetch_rows` predicts 10,000
  and 1,000 round trips for the first two rows, not the 502 both actually
  measured. What actually fits every row in both Sweep A and Sweep B is
  `total_rows / max(prefetch_rows, fetch_batch_size)`: 500 (matches 502),
  500 (matches 502), 100 (vs. measured 85), 10 (matches 11). In other
  words, asking `OCIStmtFetch2` for more rows per call than the configured
  prefetch outruns the prefetch cache and forces a round trip anyway --
  `fetch_batch_size` is a **floor** under the effective prefetch size, not
  fully independent of it. The practical rule: set `prefetch_rows` to
  whatever round-trip cost you're willing to pay, and keep `fetch_batch_size`
  at or below it -- going above it quietly reintroduces the round trips
  `prefetch_rows` was supposed to have bought you. (The original,
  incomplete claim -- "prefetch controls round trips" -- is the pre-rewrite
  version of this file's own finding at 500 rows; it's directionally right,
  just missing this interaction, which only shows up once you test a case
  where `fetch_batch_size` exceeds `prefetch_rows`.)

  | container | fetch_batch_size | elapsed_ms | rows/sec |
  |---|---:|---:|---:|
  | vector | 10 | 201.7 | 2,479,284 |
  | map | 10 | 269.1 | 1,858,264 |
  | vector | 1000 | 172.7 | 2,894,454 |
  | map | 1000 | 224.2 | 2,229,913 |

  Sweep C (vector vs. `std::map<int, BenchRow>` as the fetch destination,
  `prefetch_rows` fixed at 20000) is more nuanced than "map means batch
  size doesn't matter": `std::map` costs a fairly consistent ~30%
  throughput penalty versus `vector` at *both* batch sizes tested, not a
  bigger penalty specifically at large batch. What the numbers do support:
  the gap between a small and a large `fetch_batch_size` shrinks once
  `std::map`'s own O(log n)-per-element insertion dominates the total
  (vector: 201.7ms vs 172.7ms, ~15% apart; map: 269.1ms vs 224.2ms, ~20%
  apart) -- so a small batch costs relatively less when you were paying for
  per-element insertion overhead either way, not that it costs nothing.

- `live_oracle_insert_benchmark.cpp`: the write-side counterpart --
  `insert_rows()` (see "Running a statement" below for what that is)
  chunked over 50,000 rows, `chunk_size` swept from 1 to 50,000, measuring
  the same way (server-side round trips via `V$SESSTAT`):

  | chunk | elapsed_ms | rows/sec | roundtrips |
  |---:|---:|---:|---:|
  | 1 | 12,872.0 | 3,884 | 50,001 |
  | 10 | 1,617.7 | 30,908 | 5,001 |
  | 100 | 167.4 | 298,619 | 501 |
  | 1000 | 52.6 | 951,350 | 51 |
  | 10000 | 37.7 | 1,325,467 | 6 |
  | 50000 | 30.5 | 1,639,731 | 2 |

  This is the fetch side's mirror image, and it's clean: round trips track
  `total_rows / chunk_size` almost exactly (50001, 5001, 501, 51, 6, 2 --
  no `max()` interaction, because there is no second, independent knob on
  this side to interact with). `chunk_size=1` to `chunk_size=50000` is a
  **422x** wall-time difference (12.87s -> 30.5ms) on the exact same
  50,000 rows. This is the direct, empirical answer to "can `insert_rows`'s
  chunk size be small the way `fetch_batch_size` can": no -- there is no
  server-side write-ahead cache analogous to `OCI_ATTR_PREFETCH_ROWS` to
  absorb a small chunk size against, so shrinking it costs round trips
  (and wall time) close to linearly, not for free.

  **A real bug this benchmark caught, worth recording**: `insert_rows()`'s
  first implementation bound the whole array once against row 0 and reused
  that single bind across every chunk via `OCIStmtExecute`'s own `rowoff`
  parameter (documented for exactly this: re-running a subset of an
  already-bound array without rebinding). Against the mock this worked
  fine and all the unit-level checks passed. Against a real database it
  segfaulted on the *second* chunk -- confirmed with `gdb`: the first call
  (`rowoff=0`) succeeds, the next one (`rowoff>0`, same bind) crashes
  inside OCI's own network-marshaling code (`ttcacs`/`ttci2n`). Root cause
  not identified. `insert_rows()` now rebinds fresh for every chunk instead
  (pointed at that chunk's own starting row) -- the more conventional,
  unambiguously-documented pattern -- and the crash is gone, verified
  against a mixed row type (int/double/`FixedString<16>`/`OciDate`) with a
  deliberately-non-dividing chunk size (37 rows, `chunk_size=7`, a partial
  final chunk) round-tripped and checked value-for-value. This is exactly
  the class of bug the mock cannot catch: it never actually reads through
  the pointers/strides it's handed, so a real memory-layout mistake in an
  array bind is invisible there and only surfaces against a real OCI
  client. `OciConnection::execute()` no longer exposes a `rowoff`
  parameter at all -- better to not offer a knob with a known trap and no
  remaining caller than to document around it.

## Bottom line: fetch size, Oracle prefetch, and array-bind size

Three sizes, two completely different relationships:

- **Read side** (`select_rows()`): `prefetch_rows` and `fetch_batch_size`
  are *almost* independent. `prefetch_rows` is what actually buys round-trip
  savings; `fetch_batch_size` is close to free to shrink -- *as long as it
  stays at or below `prefetch_rows`*. Cross that line and `fetch_batch_size`
  becomes the effective floor and you silently lose the round-trip savings
  you thought `prefetch_rows` had bought you (see Sweep B's correction
  above). Rule of thumb: pick `prefetch_rows` for round-trip cost, keep
  `fetch_batch_size` ≤ it for whatever memory/CPU reason you have (a
  `std::map` destination, say).
- **Write side** (`insert_rows()`): there is no such decoupling. `chunk_size`
  *is* the round-trip granularity -- Oracle has no write-side equivalent of
  `OCI_ATTR_PREFETCH_ROWS` to buffer a small chunk against. Round trips (and
  wall time) scale close to linearly with `1 / chunk_size`, measured: 422x
  between the extremes on the same 50,000 rows. Chunk for bounded memory,
  incremental progress, or commit granularity -- not because it's free.

That write-side number was measured with `total_rows` fixed and `chunk_size`
varying -- it isolates round-trip cost, but a wall-time model actually has a
second term underneath it that number doesn't separate out:

```
elapsed_time ~= round_trips * per_round_trip_overhead
              + total_rows  * per_row_server_cost
```

`live_oracle_insert_saturation_benchmark.cpp` inverts the experiment: hold
`chunk_size` fixed (sized from a memory budget -- see the file's header
comment for why that, not an arbitrary row count, is the more meaningful
way to pick it) and vary `total_rows` instead, so `round_trips` stays flat
and whatever moves is `per_row_server_cost`. At a 100MB budget with
`BenchRow` (`sizeof` = 40 bytes), `chunk_size` comes out to ~2.6M rows --
larger than every `total_rows` tested, so every case ran as a single
`OCIStmtExecute` call (round trips stayed at 2 throughout):

| total_rows | chunks | elapsed_ms | rows/sec | roundtrips |
|---:|---:|---:|---:|---:|
| 50,000 | 1 | 35.6 | 1,405,567 | 2 |
| 200,000 | 1 | 126.1 | 1,586,373 | 2 |
| 500,000 | 1 | 721.7 | 692,793 | 2 |

Not the clean convergence-to-a-ceiling the model above predicts. Two
different effects, not one: 50k -> 200k improving is consistent with fixed
per-statement overhead (prepare, bind setup) amortizing over more rows;
200k -> 500k getting more than **2x worse** is not overhead amortizing away,
it's something becoming genuinely more expensive per row as the single
transaction grows. The honest finding: "does per-row cost converge to a
ceiling" was the wrong question for this range of `total_rows` -- there's a
cliff, not a plateau, and where it sits is a property of this specific
instance's resource sizing, not a fundamental constant of the server.

### Checking *why*: redo log switches and undo segments

Both of these track a real resource that a very large single transaction
can put real pressure on, and checking them turned "probably a redo log
switch" from a guess into something with actual evidence behind it on this
container.

**Redo logs** are where Oracle records every change before it's considered
durable -- every `INSERT`/`UPDATE`/`DELETE` writes a redo entry describing
the change, *before* that change is guaranteed safe, so the database can
replay it during crash recovery. Redo is written to a small, fixed number
of **log groups** in a round-robin: fill one, "switch" to the next; once
you've cycled through all of them, the oldest is reused -- but only once
`DBWR` (the process that writes dirty data blocks back to disk) has
confirmed every block that group's redo protects is safely on disk. If
redo generation outruns `DBWR`, Oracle has no choice but to **stall new
writes** until it catches up (the `log file switch (checkpoint incomplete)`
wait, if you've ever seen it in `V$SESSION_EVENT`) -- this is a genuine
throughput cliff, not a gradual slowdown, which is exactly the shape our
200k -> 500k number has.

```sql
SELECT GROUP#, SEQUENCE#, BYTES, MEMBERS, STATUS FROM V$LOG ORDER BY GROUP#;
```

Run against `oracle-free` right after the saturation benchmark:

```
group=1 seq=129 bytes=20971520 (20.0MB) members=1 status=INACTIVE
group=2 seq=130 bytes=20971520 (20.0MB) members=1 status=CURRENT
```

Two groups, 20MB each -- small, a default/out-of-the-box size, not
something anyone deliberately sized for this workload. A 500,000-row
`INSERT`'s redo volume (even at a conservative handful of bytes per row for
a 3-column table) can plausibly exceed 20MB on its own, meaning that single
transaction likely cycled through *both* groups, each switch potentially
paying the checkpoint-catch-up cost above. This doesn't prove the cliff is
*only* redo-related, but it's concrete, specific evidence for the
leading suspect, not just a plausible-sounding name pulled from a list.
`V$LOG_HISTORY` extends this with actual switch *timestamps*, which is what
you'd want to correlate switch timing directly against the benchmark's own
wall-clock timing on a longer-running test.

**Undo segments** are the mirror image -- for every change, Oracle also
writes an undo record capturing how to *reverse* it, which is what powers
both `ROLLBACK` and the MVCC read-consistency mechanism discussed
separately in this conversation. Even a plain `INSERT` generates undo
(a compact "delete this row" record, cheaper than an `UPDATE`'s undo but
not free), and it accumulates for the entire duration of an uncommitted
transaction -- our benchmark never commits until the very end, so a
500,000-row single transaction's undo footprint is the sum of all 500,000
rows' worth, not something that gets reclaimed as you go.

```sql
SELECT BEGIN_TIME, END_TIME, UNDOBLKS, TXNCOUNT, MAXQUERYLEN, ACTIVEBLKS, UNXPBLKSTEALCNT
FROM V$UNDOSTAT ORDER BY BEGIN_TIME DESC;
```

This is the textbook query, and it's worth knowing its real limitation
before relying on it: `V$UNDOSTAT` snapshots in **10-minute** intervals --
far coarser than a benchmark run that completes in under a second. Run
against `oracle-free` immediately after the saturation benchmark above, it
returned **zero rows** -- not a sign undo wasn't used, just that this view
either hadn't accumulated a snapshot yet on this freshly-started container
or doesn't populate the same way queried from inside a PDB (this is a
multitenant `FREEPDB1`, not the CDB root) -- either way, it's the wrong
tool for inspecting one specific short run. `V$TRANSACTION` is the
practical alternative for that: queried *while* the transaction is still
open (from a second, concurrent session, since the row disappears the
moment the first session commits or rolls back), `USED_UBLK`/`USED_UREC`
give the undo blocks/records the in-flight transaction has consumed so
far -- the same "read it from a second connection while the first is
mid-flight" pattern `live_oracle_disconnect_demo.cpp` already uses for a
different purpose (reading `V$SESSION`'s SID/SERIAL# to kill it). Doing
this for real means adding a deliberate pause into the saturation
benchmark's 500,000-row case specifically, so a concurrent session has a
window to poll `V$TRANSACTION` before it commits -- not done here, but the
mechanism already exists elsewhere in this codebase to build on.

Bottom line on the *why*: `V$LOG`'s 20MB-per-group sizing is concrete,
verified evidence that this container's redo configuration is genuinely
small relative to the workload -- a real, specific finding, not a
placeholder guess. Undo remains an open, plausible, *unverified* second
suspect on this specific container -- `V$UNDOSTAT`'s granularity ruled it
out as a tool here, and `V$TRANSACTION` is the next thing to try, ideally
against an instance with production-realistic redo/undo sizing rather than
this container's defaults.

Two things worth knowing if you try to reproduce this on a different
machine, both hit and resolved during this session:

- A second Instant Client install present on the same machine, labeled
  23.1, failed `OCIEnvCreate` outright until `ORACLE_HOME` was set to
  point at it (its directory layout resembles a partial database install --
  it ships `oracore`/`rdbms`/`network` subdirectories a standard Instant
  Client extraction doesn't), and even with that fixed, `OCILogon2` failed
  with `ORA-28041` ("authentication protocol internal error") against this
  server version. Instant Client 19 needed neither workaround.
- Linking against `libclntsh.so` needs `-Wl,-rpath-link,<dir>` in addition
  to `-L<dir>` -- without it, the link step fails with undefined references
  to internal Oracle symbols that actually live in `libclntsh.so`'s own
  shared-library dependencies (`libclntshcore.so`, `libnnz.so`,
  `libaio.so.1`), which plain `-L` does not make the linker load.

Not yet verified against MSVC in this session -- but `boost::pfr::names_as_array()`
being available there at all (see "Binding: by name for parameters..."
above) has been confirmed directly by whoever's building this against the
real MSVC toolchain in question. The `flat_schema`/`bindable`/
`config_schema` position-based field-walking engine in `reflect.h` has
separately already needed two MSVC-specific workarounds (see the comments
on `struct_field_auditor` and `check_field`), so treat MSVC as untested
for anything not explicitly called out as confirmed.
