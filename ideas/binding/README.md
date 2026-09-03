# binding (idea)

One `boost::pfr`-based reflection core, shared between two binding jobs that
turn out to be the same problem: mapping a flat struct (and `vector<S>` of
them) onto an external, name/position-addressed record format. Config
sections are one instance of that; Oracle OCI bind/row structs are another.

Not wired into the top-level build yet -- `boost::pfr` isn't on the
project's approved-libraries list. This is scaffolding to decide whether
that's worth doing, not a committed dependency.

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
- `include/binding/oci_connection.h` -- `OciConnection`: owns the OCI
  handles and the reconnect policy (see below).
- `include/binding/oci_client.h` -- `OciClient`: `execute()`, `insert()`,
  `select()` (see "The client's methods" below), all built on `bindable<T>`
  -- `flat_schema`'s leaf predicate, plus LOB types, reusing the same
  `struct_field_auditor` engine.
- `examples/main.cpp` -- execute() with no bind struct at all, a mid-execute
  disconnect that recovers, a plain exec error that must not retry, select()
  into `vector<T>`, binding an empty `std::optional` as SQL NULL, insert()
  with a `vector<T>` of several rows, select() with std::optional (a NULL
  column comes back as `nullopt`), and a dynamic `IN (...)` list.
- `include/binding/field_tree.h` -- `Field`/`FieldList`: a parser-independent
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
- `include/binding/oci_collection_bind.h` -- `select_with_in_collection()`:
  an Oracle collection-object bind for `IN (...)`, the alternative to
  `select_with_in_list()`'s generated placeholder list (see below).

## The client's methods (execute / insert / select)

`OciClient`'s public methods are named as SQL verbs, not as
bind-mechanism jargon:

- **`execute(conn, query_text)`** -- runs any statement that returns no
  rows and needs no bind parameters: DDL (`CREATE`/`ALTER`/`TRUNCATE`), or
  DML that's fully literal in the text.
- **`execute(conn, query_text, bind_struct)`** -- runs any statement that
  returns no rows (`INSERT`/`UPDATE`/`DELETE`/`MERGE`/a stored-procedure
  call/..., including `RETURNING ... INTO`), binding `bind_struct`'s fields
  by name (see below).
- **`insert(conn, query_text, row)`** -- the exact same mechanism as the
  `execute(..., bind_struct)` overload above; it's an alias, purely for
  reading as intent at the call site when the statement actually is an
  `INSERT`.
- **`insert(conn, query_text, std::vector<T>& rows)`** -- inserts several
  rows. For now this is a naive per-row loop: one `OCIStmtExecute` (with
  its own independent reconnect-retry) per row, not a real Oracle array
  bind. It stops at the first failed row without rolling back rows already
  inserted -- transaction/commit boundaries are the caller's responsibility
  here exactly as with every other method (see "What's deliberately not
  here"). **A real bulk-bind implementation (`OCIBindByName` against an
  array of values, one `OCIStmtExecute` with `iters = rows.size()`) is a
  deliberately deferred TODO** -- the per-row loop is correct, just not
  fast for a large `rows`; revisit when bulk-insert throughput actually
  matters for a real use case.
- **`select(conn, query_text, results)`** -- runs a `SELECT` and returns
  its rows into `std::vector<T>&`.
- **`select_with_in_list()` / `execute_with_in_list()` / (from
  `oci_collection_bind.h`) `select_with_in_collection()`** -- the dynamic
  `IN (...)` list variants (see below).

## Binding: by name for parameters, by position for result columns

`execute()`/`insert()`'s parameters bind **by name** (`OCIBindByName`):
each field binds to a `:field_name` placeholder using its own
(compiler-derived) name, e.g. field `bonus_pct` binds `:bonus_pct` wherever
that placeholder occurs in the SQL text -- in any order, and even if it
occurs more than once (see "on placeholder reuse" below).

`select()`'s result columns bind **by position** (`OCIDefineByPos`):
column order in the `SELECT` list must match `T`'s declared field order.
This isn't a design choice made here -- raw OCI simply has no
`OCIDefineByName`; binding an output column is only ever positional in
classic OCI, regardless of what `boost::pfr` can do.

**History, since this reversed an earlier decision in this file:** the
first cut of the OCI binder bound *everything* by position, on the
assumption that `boost::pfr::names_as_array()` depends on
`__FUNCSIG__`/`__PRETTY_FUNCTION__`-style compiler-specific parsing and so
wasn't reliably available on MSVC, the actual compiler target this idea
started from. That assumption turned out to be wrong for the MSVC version
in question: `boost::pfr` also has a separate consteval/
`std::source_location`-based name implementation (see
`BOOST_PFR_CORE_NAME_ENABLED` and `core_name20_static.hpp`), confirmed
working there, so parameter binding switched to `OCIBindByName`. This also
fixed a real usability wart the position-based version had: `OCIBindByPos`'s
"position" meant *occurrence order in the SQL text*, which forced a
struct's field *declaration* order to match wherever its placeholders
happened to land across a statement's clauses -- reordering the struct
silently rebinds the wrong parameter to the wrong field, and it compiles
fine. Binding by name removes that coupling entirely. `config_bind.h`'s
name-based matching (`config_schema`/`bind_from_fields`, further down) was
never affected by this -- it needs name matching regardless, since a
repeated element's several same-named `Field` entries can't be matched to
a single `vector<T>` field by position at all.

**On placeholder reuse:** a bind placeholder's "position" (whether
numbered, like `:1`, or named, like `:emp_id`) refers to the Nth *distinct*
placeholder in order of first appearance in the SQL text, not the Nth
*occurrence*. `WHERE a = :1 OR b = :1` has exactly one bind (`:1`, reused
in two places), not two -- the same rule applies to named binds. This
matters for `bind_in_list()` (below): each element of the IN-list consumes
one new placeholder, since each is a distinct value, but a struct field
whose name is written twice in a statement's text still only binds once.

## Reconnect policy

`OciConnection::run_with_reconnect()` is the one piece of retry logic, used
by every method above:

- Runs the given operation once.
- On failure, calls `is_disconnect_error()`, which reads the ORA-code off
  the error handle via `OCIErrorGet` and checks it against a small table of
  known "session is gone" codes (ORA-03113, ORA-01012, ORA-00028, ...).
- If it's a disconnect: sleep `retry_interval`, reconnect, and re-run the
  *entire* operation from scratch -- up to `max_retries` times.
- If it's anything else (bad SQL, a constraint violation, no data found):
  return failure immediately. It is not retried, because retrying an exec
  error just reproduces it.

### connect() uses OCILogon2, and checks every call

An earlier version of `connect()` did the connection setup the long way --
`OCIHandleAlloc(SERVER)` + `OCIServerAttach` + `OCIHandleAlloc(SVCCTX)` +
`OCIAttrSet(SERVER)` + `OCIHandleAlloc(SESSION)` + `OCIAttrSet(USERNAME)` +
`OCIAttrSet(PASSWORD)` + `OCISessionBegin` + `OCIAttrSet(SESSION)` -- and
only ever checked the status of `OCISessionBegin`, the second-to-last call.
A failure anywhere earlier (a bad hostname at `OCIServerAttach`, say) was
silently ignored, and every later call in the sequence ran anyway against
whatever half-set-up handle resulted, with any real error surfacing (if at
all) from the wrong call.

For the plain username/password case this class actually needs -- no
connection pooling, no external authentication -- `OCILogon2` replaces that
entire sequence with one call that hands back a ready `OCISvcCtx*`.
`connect()` now checks the status of every call it makes (`OCIEnvCreate`,
the error handle's `OCIHandleAlloc`, `OCILogon2`), tearing down via
`disconnect()` on the first failure instead of continuing past it.
`disconnect()` correspondingly shrank to `OCILogoff` plus freeing the env
and error handles -- the `OCIServer`/`OCISession` handles `OCILogon2`
manages internally never need to be held or freed here at all.

## NULL handling (std::optional<U>)

A field declared `std::optional<U>` (U arithmetic or string-convertible)
maps to a nullable column:

- **`execute()`/`insert()`**: an empty optional binds SQL NULL (indicator
  `OCI_IND_NULL`); a set one binds `*field` with indicator `OCI_IND_NOTNULL`.
- **`select()`**: a NULL column comes back as `std::nullopt`; otherwise the
  fetched value is wrapped in the optional.

Neither direction can bind/define straight into the optional's own storage:
dereferencing an empty `std::optional` to get `&*opt` is undefined behavior,
and there's no standard-sanctioned way to get the address of its unset
storage either. So each optional field gets a real, addressable staging `U`
(`detail::staging_tuple_t<T>` in `oci_client.h`) that OCI actually
binds/defines against, plus a `sb2` indicator slot per field
(`OCI_IND_NULL` / `OCI_IND_NOTNULL`) -- the bind side copies the optional
into the staging value (or leaves it default/empty), the select side copies
the staging value back into the optional (or resets it to `nullopt`) once
the indicator says which after each fetched row.

`std::string`/`optional<std::string>` **binds** fine on the
`execute()`/`insert()` side (size = content length, not `sizeof(std::string)`
-- an earlier version of this file had that bug too, since no demo
exercised a raw string bind). `select()`'s output side still rejects string
columns via `static_assert` (same reason as LOB columns): OCI needs a
fixed max buffer size to write into before it knows how long the value is,
and this client doesn't manage that yet.

### NULL on a field that isn't std::optional

Every column defined by `select()` gets an indicator captured during fetch
(see above), but until now only `std::optional` fields were ever checked
against it -- a plain `int`/`double`/`std::string` field that unexpectedly
came back NULL silently kept whatever stale/default value was already in
its staging slot. `detail::apply_field_null_semantics` (in `oci_client.h`)
now checks every field's indicator, not just optional ones: a NULL landing
on a field that isn't `std::optional<T>` throws `std::runtime_error` naming
the field, instead of continuing with a garbage value.

There's no schema/`DESCRIBE` metadata available here to know ahead of time
whether a column can be NULL, so this can only be caught *after* a fetch
actually returns one -- not at compile time, and not before running the
query. Enforcing it at compile time (checking a struct's declared
nullability against real column metadata via `OCIDescribeAny`/`OCIAttrGet`
at connection-setup time, say) is a natural next step if this idea goes
further, but isn't attempted here.

Throwing rather than returning a retriable `false` is deliberate, for the
same reason `config_bind.h`'s errors throw: no reconnect/retry can ever fix
a real data/schema mismatch like this, so retrying it would just waste a
retry budget reproducing the same throw.

## Config binding (FieldList -> struct)

`field_tree.h` defines the shape parsed config data takes before it meets a
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

## A struct field can itself be a dynamic multi-value IN-list

Before this, `execute()`/`insert()`'s struct-based binding only accepted
scalar leaf fields (plus `std::optional<leaf>` and the LOB types) -- there
was no way to bind a query that mixes ordinary named parameters with a
collection-typed one, e.g. `UPDATE trades SET status = :status WHERE
trade_id IN (...)`. The only place a collection could be bound at all was
`select_with_in_list()`/`execute_with_in_list()` below, which assume the
collection is the query's *only* parameter.

`bindable`'s field predicate now also accepts a `std::vector<U>`/
`std::set<U>`/`std::valarray<U>` field, for U a bindable leaf:

```cpp
struct TradeStatusUpdate {
    std::string status;      // an ordinary named parameter: :status
    std::set<int> trade_ids; // a dynamic multi-value IN-list: {trade_ids}
};

client.execute(conn, "UPDATE trades SET status = :status WHERE trade_id IN ({trade_ids})", filter);
```

The mechanism is the field-scoped version of the standalone `{IN}` marker
below: `detail::substitute_container_markers` (called once, unconditionally,
at the top of `run_execute_once` -- a no-op if `T` has no container field)
replaces each container field's own `{field_name}` marker with a named
placeholder list sized to that field's current element count
(`make_named_placeholders`, e.g. `:trade_ids_0,:trade_ids_1,:trade_ids_2`),
*before* `OCIStmtPrepare` -- Oracle needs the exact placeholder count fixed
in the SQL text itself, the same reason `{IN}` substitution exists.
`detail::bind_named_container` then binds each element by that generated
name (`OCIBindByName`, matching how every other field on this side binds).
A missing marker for a container field -- forgetting to write
`{field_name}` where the values should go -- throws immediately rather than
silently preparing a statement whose binds have nowhere to land.

Unlike `select_with_in_list()`'s ID collection, a struct's container field
is **not** deduped/sorted into a set first (a `std::set<U>` field is
naturally already ordered/deduped by being a set, but a `std::vector<U>`/
`std::valarray<U>` field binds exactly what the caller put there, in
order, duplicates included) -- that deduping was a deliberate,
documented optimization specific to the standalone IN-list convenience
functions, not a property every collection bind should silently apply to
a caller's own data.

`select()`'s output side explicitly rejects a container-typed field via
`static_assert` -- a single result column can't fetch into a
variable-length container (that's what multiple *rows* are for), so this
is bind-side-only by design, not an oversight.

## Dynamic IN (...) lists

`OciClient::select_with_in_list()` / `execute_with_in_list()` handle a
variable-length `WHERE col IN (...)`. `query_template` carries one `{IN}`
marker -- e.g. `"SELECT trade_id, notional FROM trades WHERE trade_id IN
({IN})"` -- which `make_in_placeholders()` expands to `:1,:2,...,:N` sized
to the ID collection, before `OCIStmtPrepare` (Oracle needs the exact
placeholder count fixed in the SQL text itself; there's no variable-arity
bind). `bind_in_list()` then binds each element positionally, reusing the
same `raw_bind_args`/`oci_type_code_v` machinery the struct binder uses.

The ID collection is a `std::set<ElemType>` (`std::vector<ElemType>` and
`std::valarray<ElemType>` overloads exist too, both purely as a convenience
that dedupes into a set before delegating) -- for two reasons:

- **Dedup.** A repeated value in an `IN` list is never meaningful, only a
  wasted bind.
- **Deterministic order.** Oracle's shared-pool cursor cache is keyed on
  SQL text. Two calls with the same *logical* set of IDs must generate
  identical placeholder text and bind order regardless of what order the
  caller happened to collect them in, or they needlessly fragment into
  separate cached cursors.

An empty collection expands to the literal `NULL` rather than an empty
placeholder list -- `... IN ()` is a SQL syntax error, but `... IN (NULL)`
is valid and (per SQL's three-valued logic, `x = NULL` is never true)
correctly matches zero rows. No special-casing needed at the call site for
an empty ID list.

`make_in_placeholders()` also enforces Oracle's other, sharper limit here:
**ORA-01795** -- a plain `IN (...)` list, whether its elements are literals
or bind placeholders, is capped at 1000 expressions by the parser itself.
That's unrelated to (and hit long before) the 64KB max SQL statement text
length -- a 1000-placeholder list is nowhere near that. Passing more than
1000 elements throws a clear error naming the actual limit, instead of
letting it surface as an opaque `ORA-01795` from `OCIStmtExecute`. Beyond
1000, or to avoid shared-pool fragmentation as list sizes vary, see the
collection bind below.

## Dynamic IN (...) lists via a collection bind (oci_collection_bind.h)

`select_with_in_collection()` is the alternative to `select_with_in_list()`
above: instead of generating a placeholder list sized to the collection, it
binds *one* Oracle collection object and queries
`WHERE id IN (SELECT column_value FROM TABLE(:1))`. The SQL text is fixed
regardless of how many elements are in the collection -- no ORA-01795 cap,
no shared-pool fragmentation as sizes vary between calls.

`OciCollectionTypeBinder<ElemType>` maps the element type to an Oracle SQL
collection type to bind through, defaulting to Oracle's built-in ODCI
schema types so the common case needs no schema setup:
`SYS.ODCINUMBERLIST` for arithmetic types, `SYS.ODCIVARCHAR2LIST` for
strings. `build_in_collection()` looks up that type (`OCITypeByName`),
creates a new empty instance of it (`OCIObjectNew`), and appends every
element (`OCICollAppend`, via `OCINumberFromInt` for a NUMBER element).
`select_with_in_collection()` then binds the whole collection as one
parameter (`OCIBindByPos` with `SQLT_NTY`, followed by `OCIBindObject`) and
frees it (`OCIObjectFree`) after the fetch loop -- otherwise the same
shape as `select()` in `oci_client.h`.

**This is meaningfully lower-confidence than the rest of this directory.**
Everything else here binds/defines through `OCIBindByName`/`OCIBindByPos`/
`OCIDefineByPos` -- a handful of well-documented, thoroughly-used scalar
OCI calls. Oracle's object/collection API (`OCIType`, `OCIObjectNew`,
`OCICollAppend`, `OCIBindObject`) is a genuinely more obscure corner of
OCI, and there's no real `oci.h`/`ociap.h` on this machine to check
`oci_object_mock.h`'s signatures against -- they're written from
documentation recall, not verified the way the rest of this codebase's OCI
signatures have been. What *is* verified: `oci_collection_bind.h` compiles
and runs correctly against its own mock (`examples/collection_demo.cpp`,
both a `std::string` and an `int` element type), proving the C++ template
plumbing hangs together. What's *not* verified is that the mock's
signatures faithfully match a real client's. Diff `oci_object_mock.h`
against the actual `oci.h`/`ociap.h` before trusting this against a live
database -- see the warning banner at the top of `oci_collection_bind.h`.

## What's deliberately not here

- Real bulk/array-bind for `insert(conn, query_text, std::vector<T>&)` --
  see "The client's methods" above. It's a correct but naive per-row loop.
- Transaction/commit handling (`OCITransCommit` / `OCI_COMMIT_ON_SUCCESS`) --
  left to the caller.
- String and LOB columns in `select()`'s result rows (`static_assert`s
  against both, for the same fixed-buffer-size reason).
- Nullable LOBs (`std::optional<OciClob>`) -- LOB fields always bind/define
  as not-null for now.
- Non-`std::string` string-like leaf types in `config_bind.h`'s
  `parse_leaf_value` (only `std::string` and arithmetic types are handled;
  a custom string-view-convertible type would satisfy `is_bindable_leaf`
  but hit a `static_assert` here).
- Wiring `Config` (the project's existing TOML-based config class in
  `core/config`) up to any of this -- this is XML-shaped scaffolding sitting
  next to it, not a replacement.
- Oracle's genuinely nested column types -- object types and `TABLE OF`
  nested tables. A normal result-set row is flat except for LOBs, which is
  exactly what `bindable` enforces; those two column kinds are a real
  exception (a single row's column can itself be struct- or collection-
  shaped), but supporting them for real needs `OCIType`/`OCIDescribe`
  metadata and `OCIObjectNew`/`OCIObjectGetInd`-style APIs -- a different
  mechanism from the `OCIBindByName`/`OCIDefineByPos` scalar+LOB path
  everything else here is built on. `bindable` correctly rejects a
  nested/`vector<U>` field today; it just doesn't yet offer a way to
  actually bind one of these two column kinds when you do need it.
- Using the same `OCIType`/`OCIObjectNew`/`OCICollAppend` machinery for
  actual Oracle object-type/nested-table *columns* (the point above) --
  `oci_collection_bind.h` only uses it for one specific purpose (a bound
  IN-list collection), not as a general nested-column-value binder.
- An ad hoc, struct-free positional bind interface (something like
  `select(conn, query_text, results, args...)`, each `args...` element
  binding at `:1, :2, ...` in pack order) for one-off queries where
  defining a whole named struct is overkill. Discussed, not built yet.

## Compiling

Verified end to end on this host: `g++ 15.2` / C++20 / Boost 1.91, all three
demos (`binding_demo` for OCI, `config_demo` for XML config,
`collection_demo` for the collection-bind IN-list), via `cmake -S -B` /
`--build` (using `CMakeLists.txt`, pointed at `/mnt/c/local/boost_1_91_0`
through `BOOST_ROOT`) and via a direct `g++` invocation with
`-Wall -Wextra -Wpedantic -Wshadow` (clean except for one warning inside
Boost's own `core_name20_static.hpp`, unrelated to this code).
`config_demo` additionally needs `boost::property_tree`, which the
FetchContent fallback (standalone `pfr` only) doesn't provide -- it only
builds when `BINDING_BOOST_INCLUDE_DIR` resolves to a real local Boost
install. See "Dynamic IN (...) lists via a collection bind" above for why
`collection_demo` passing here is weaker evidence than the other two.

Not yet verified against MSVC in this session -- but `boost::pfr::names_as_array()`
being available there at all (see "Binding: by name for parameters..."
above) has been confirmed directly by whoever's building this against the
real MSVC toolchain in question. The `flat_schema`/`bindable`/
`config_schema` position-based field-walking engine in `reflect.h` has
separately already needed two MSVC-specific workarounds (see the comments
on `struct_field_auditor` and `check_field`), so treat MSVC as untested
for anything not explicitly called out as confirmed.
