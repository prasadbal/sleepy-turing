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
- `include/binding/oci_fixed_string.h` -- `FixedString<N>` (`OciChar<N>` /
  `OciVarchar2<N>`), a fixed-capacity character buffer. This is the type a
  CHAR/VARCHAR2 column binds *and* defines through: `N` is the maximum output
  buffer size OCI needs before it knows how long a value is, and the
  characters sit inline in the row struct at a fixed stride, which is what
  both `OCIDefineArrayOfStruct` and `OCIBindArrayOfStruct` need. `std::string`
  can satisfy neither, which is why it stays an input-only bind type.
- `include/binding/oci_datetime.h` -- `OciDate` (a DATE column, `SQLT_ODT`)
  and `OciTimestamp` (a TIMESTAMP column, `SQLT_TIMESTAMP`). `OciDate` wraps
  the real 7-byte `::OCIDate` struct directly, with no descriptor or
  allocation, so (like an arithmetic field or `FixedString<N>`) it needs no
  special-casing anywhere and works in every path: scalar bind/select,
  bulk `insert(vector<T>&)`, and `select()`'s batch array fetch -- all
  live-verified, including 500 rows through the bulk/batch paths.
  `OciTimestamp` is different: its `OCIDateTime*` is a per-value
  descriptor (allocated via `OCIDescriptorAlloc`, populated via
  `OCIDateTimeConstruct`), the same shape as `OciClob`/`OciXml`'s locator
  -- so it's bind-side only (`execute()`/`insert()`, single-row), not
  select()-able and not bulk-bindable, exactly like a LOB field. Use
  `OciDate` for a plain calendar date (e.g. a COB/business date, which
  has no meaningful time-of-day); reach for `OciTimestamp` only when
  sub-day precision genuinely matters. Neither models fractional seconds
  or timezones yet.
- `include/binding/oci_connection.h` -- `OciConnection`: owns the OCI
  handles and the reconnect policy (see below).
- `include/binding/oci_client.h` -- `OciClient`: `execute()`, `insert()`,
  `select()` (see "The client's methods" below), all built on `bindable<T>`
  -- `flat_schema`'s leaf predicate, plus LOB types, reusing the same
  `struct_field_auditor` engine.
- `examples/main.cpp` -- execute() with no bind struct at all, a mid-execute
  disconnect that recovers, a plain exec error that must not retry, select()
  into `vector<T>`, binding an empty `std::optional` as SQL NULL, insert()
  with a `vector<T>` of several rows, a struct field that's itself a dynamic
  multi-value IN-list alongside an ordinary named field, select() with
  std::optional (a NULL column comes back as `nullopt`), and a NULL landing
  on a field that isn't `std::optional`.
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
- `include/binding/oci_collection_bind.h` -- `select_with_in_collection()`/
  `execute_with_in_collection()`: a dynamic `IN (...)` list bound as a
  single Oracle collection object (see
  [docs/in_list_binding.md](docs/in_list_binding.md)).

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
  rows as a real Oracle array bind: one `OCIBindByName` per field (pointing
  at `rows[0]`'s field) plus `OCIBindArrayOfStruct` (telling OCI the byte
  stride to the same field in the next row), then a single `OCIStmtExecute`
  with `iters = rows.size()` -- one round trip for the whole batch, reading
  values directly out of `rows`' own contiguous storage. Verified live
  against a real database: 2000 rows inserted in one call, all values
  confirmed correct on read-back.
  Scope: a plain (non-optional) arithmetic leaf field, or a `FixedString<N>`
  field, binds this way -- both have their bytes inline in `T` at a fixed
  offset, which is exactly what a fixed-stride array bind needs (a
  `FixedString`'s per-row length travels through `OCIBindArrayOfStruct`'s
  `alskip`, the same way its value travels through `pvskip`). `std::string`,
  `std::optional<U>`, LOB, and vector/set/valarray fields all `static_assert`
  here instead of silently binding garbage (a string's characters live in its
  own heap/SSO storage, not inline in `T` at a fixed stride -- declare the
  field `FixedString<N>` instead; an empty `std::optional` has no address to
  bind through; LOB needs a per-row locator array; a container field has no
  per-row meaning at all). For a row type with any of those, insert each row
  in a loop via `insert(conn, query_text, T&)` instead.
  Transaction/commit boundaries are the caller's responsibility here
  exactly as with every other method (see "What's deliberately not here").
- **`select(conn, query_text, results)`** -- runs a `SELECT` with no bind
  parameters and returns its rows into `std::vector<T>&`.
- **`select(conn, query_text, input, results)`** -- same, but also binds
  `input`'s fields as named parameters first (e.g. a `WHERE` clause) --
  the read-side counterpart to `execute(conn, query_text, bind_struct)`.
  `input` only ever supplies parameters; `results`' column-order/type
  rules are unchanged from the no-input overload above.

  Both overloads fetch as a real array-of-struct batch: `OCIDefineByPos` +
  `OCIDefineArrayOfStruct` define every column directly into a
  `kSelectBatchRows`-sized (100) `std::vector<T>`, and each
  `OCIStmtFetch2` call asks for up to that many rows at once (via
  `OCI_ATTR_ROWS_FETCHED` to learn how many actually came back, since the
  final batch is usually partial), appending each batch to `results` in
  one bulk `insert()` rather than one `push_back()` per row. This is
  deliberately *not* about network round trips -- Oracle's own
  client-side prefetch cache (`OCI_ATTR_PREFETCH_ROWS`, set here
  regardless) already collapses round trips for a plain one-row-at-a-time
  fetch loop; measured directly against a real database, fetching 500
  rows one at a time took 252 round trips at the client's small default
  prefetch setting, and 2 round trips with `OCI_ATTR_PREFETCH_ROWS` set to
  500 -- no change to the fetch loop itself required. What batching the
  fetch call *does* buy, independent of that: fewer `OCIStmtFetch2`/
  `OCIAttrGet` calls and fewer, larger `results` growth operations for a
  large result set, instead of one small step per row. Verified live
  against a real database: a 20,000-row result (an exact multiple of the
  batch size) and a 2,050-row one (forcing a partial final batch of 50),
  both an exact row-count and checksum match; `std::optional<double>`
  NULL handling verified correct across multiple batches too, not just
  within one.
- **(from `oci_collection_bind.h`) `select_with_in_collection()` /
  `execute_with_in_collection()`** -- a dynamic `IN (...)` list bound as a
  single Oracle collection object (see
  [docs/in_list_binding.md](docs/in_list_binding.md)), for when the values
  to match against aren't already sitting in a bind struct's own field.

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
matters for `bind_named_container()`
([docs/in_list_binding.md](docs/in_list_binding.md)): each element of a
container field's IN-list consumes one new, distinctly-named placeholder,
since each is a distinct value, but an ordinary field whose name is
written twice in a statement's text still only binds once.

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
- The operation only runs at all while there *is* a session. An earlier
  version went straight back into the operation after a failed reconnect,
  with `env_`/`svc_`/`err_` all null -- `OCIHandleAlloc(nullptr, ...)`
  followed by `OCIStmtPrepare` on the null statement it handed back. A
  failed reconnect is now just another consumed retry.

Every OCI call the client makes is status-checked, not only the ones in
`connect()`. `OCIHandleAlloc`, `OCIStmtPrepare`, every `OCIBindByName`/
`OCIDefineByPos`/`ArrayOfStruct`, `OCIAttrSet`/`OCIAttrGet`, `OCILobWrite2`,
`OCITypeByName`/`OCIObjectNew`/`OCICollAppend` all previously ran unchecked,
so a failure part-way through configuring a statement was skipped and every
later call ran against the half-configured result -- the same shape of bug
the `connect()` rewrite below removed from the session-setup path. A failing
field bind now aborts the attempt and reports that call's status.

Statement handles and collection instances are owned by RAII guards
(`detail::StmtHandle`, `detail::LocatorGuard`, `detail::ObjectInstanceGuard`)
rather than freed on each return path. Several paths here throw -- an
unexpected NULL on a non-optional column, an over-cap IN-list, a missing
container marker -- and an explicit free at the end of a method is skipped
entirely when the stack unwinds through it, leaking one handle per throw.

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
exercised a raw string bind). It is still rejected by `static_assert` as a
`select()` output column, and as a bulk-insert column: OCI needs a fixed max
buffer size to write into before it knows how long a value is, and a
`std::string`'s characters are not inline in the row at a fixed stride.

**A string column uses `FixedString<N>` instead** (`binding/oci_fixed_string.h`),
in either direction and in both the scalar and the array paths:

```cpp
struct ReportRow {
    std::int64_t             position_id;
    binding::FixedString<16> desk;        // a VARCHAR2(16) column
    binding::FixedString<8>  risk_class;
    double                   delta;
    std::optional<double>    vega;
};
```

`N` is the buffer OCI defines into, and the field's own `length_ref()` is
passed as OCI's actual-length pointer (`rlenp` on define, `alenp` on bind),
strided by `sizeof(T)` so every row reports its own length. A value longer
than `N` truncates, exactly as it would against a VARCHAR2(N) column.
`std::optional<FixedString<N>>` works for a nullable string column, staged
per batch the same way an `optional<double>` is.

Binding through `FixedString<N>` is also what makes an **OUT** parameter safe
(`RETURNING ... INTO`): the bound size is the buffer capacity, which is what
OCI is allowed to fill. A `std::string` bound at content length would be
overflowed by a longer returned value.

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

## Dynamic IN (...) lists

Two distinct mechanisms -- a struct field that's itself a container
(still placeholder-expansion, still capped at 1000 elements), and a
standalone ID collection bound as a single Oracle collection object (no
cap, verified against a real Oracle database) -- are documented in
[docs/in_list_binding.md](docs/in_list_binding.md), including the
`OCI_OBJECT` environment-mode requirement the collection bind needs and
the live 10,000-element test that found it.

## Collection bind: number precision, and the VARCHAR2 crash

`append_collection_element` used to funnel every arithmetic element through
`static_cast<int>` before `OCINumberFromInt`, so a `std::set<double>` bound as
an IN-list silently matched on truncated integers -- wrong rows, no error
anywhere, even though `OciCollectionTypeBinder<double>` is specialized and so
that path is reachable by design. A floating-point element now goes through
`OCINumberFromReal`, and an integral one keeps its own width and signedness
instead of narrowing to `int`.

The `std::string` element crash (see the KNOWN BROKEN note in
`oci_collection_bind.h`) is **not** fixed. The cheap way around it, rather
than root-causing `SYS.ODCIVARCHAR2LIST`: create your own collection type and
point the binder at it -- the specialization hook already exists.

```sql
CREATE TYPE frtb_id_list AS TABLE OF VARCHAR2(64);
```

```cpp
template <> struct binding::OciCollectionTypeBinder<std::string> {
    static constexpr std::string_view schema = "FRTB";
    static constexpr std::string_view type_name = "FRTB_ID_LIST";
};
```

The `SYS.ODCI*` types are Oracle's own helper types for extensible indexing,
not general-purpose bind vehicles; a bounded user-defined nested table is
what you would want in production regardless. Untested against a real
database -- it replaces one unverified path with another, but with a type
whose definition you control.

## What's deliberately not here

- Bulk array-bind for `insert(conn, query_text, std::vector<T>&)` when `T`
  has a `std::string`/`std::optional<U>`/LOB/container field -- see "The
  client's methods" above for exactly which field kinds the real array
  bind supports today (`FixedString<N>` is supported; `std::string` is not);
  a row type with any of the others still needs a per-row loop via
  `insert(conn, query_text, T&)`.
- Chunking `insert(conn, query_text, std::vector<T>&)`: the whole vector is
  bound and executed as one `OCIStmtExecute` with `iters = rows.size()`. For
  a very large batch that is one enormous bind with no partial progress --
  worth splitting once transaction boundaries exist to split it *on*.
- Making `kSelectBatchRows`/`OCI_ATTR_PREFETCH_ROWS` (both 100) configurable.
  100 is small for an extract of millions of rows.
- Transaction/commit handling (`OCITransCommit` / `OCI_COMMIT_ON_SUCCESS`) --
  left to the caller.
- `std::string` and LOB columns in `select()`'s result rows (`static_assert`s
  against both, for the same fixed-buffer-size reason). A string column is
  supported as `FixedString<N>` -- see "NULL handling" above.
- Exact decimal. Every numeric field binds as `SQLT_INT`/`SQLT_UIN`/
  `SQLT_BDOUBLE`, i.e. binary. Oracle `NUMBER` is decimal with up to 38
  digits, so a value that has to reconcile to the cent should go through
  `OCINumber`/`SQLT_VNU` (or be fetched as text) rather than a `double`.
  `float`/`SQLT_BFLOAT` is kept only for compatibility; ~7 significant
  digits is not enough for anything money-shaped.
- Date/timestamp columns. There is no `SQLT_DAT`/`SQLT_TIMESTAMP` mapping
  and no date type here at all.
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
install. `collection_demo` here only exercises the mock -- see
[docs/in_list_binding.md](docs/in_list_binding.md) for the separate live
test against a real Oracle database that actually verified this code
path.

Not yet verified against MSVC in this session -- but `boost::pfr::names_as_array()`
being available there at all (see "Binding: by name for parameters..."
above) has been confirmed directly by whoever's building this against the
real MSVC toolchain in question. The `flat_schema`/`bindable`/
`config_schema` position-based field-walking engine in `reflect.h` has
separately already needed two MSVC-specific workarounds (see the comments
on `struct_field_auditor` and `check_field`), so treat MSVC as untested
for anything not explicitly called out as confirmed.
