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
- `include/binding/oci_client.h` -- `OciClient::execute<T>` (single-row DML)
  and `OciClient::query<T>` (SELECT into `vector<T>`), both built on
  `oci_row_schema<T>` -- `flat_schema`'s leaf predicate, plus LOB types,
  reusing the same `struct_field_auditor` engine.
- `examples/main.cpp` -- five scenarios: a mid-execute disconnect that
  recovers, a plain exec error that must not retry, a `SELECT` into
  `vector<T>`, binding an empty `std::optional` as SQL NULL, and a `SELECT`
  where a NULL column comes back as `std::nullopt`.
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
  and a field absent from the XML; then shows the missing-required-field
  error path.

## NULL handling (std::optional<U>)

A field declared `std::optional<U>` (U arithmetic or string-convertible)
maps to a nullable column:

- **`execute()`**: an empty optional binds SQL NULL (indicator `OCI_IND_NULL`);
  a set one binds `*field` with indicator `OCI_IND_NOTNULL`.
- **`query()`**: a NULL column comes back as `std::nullopt`; otherwise the
  fetched value is wrapped in the optional.

Neither direction can bind/define straight into the optional's own storage:
dereferencing an empty `std::optional` to get `&*opt` is undefined behavior,
and there's no standard-sanctioned way to get the address of its unset
storage either. So each optional field gets a real, addressable staging `U`
(`detail::staging_tuple_t<T>` in `oci_client.h`) that OCI actually
binds/defines against, plus a `sb2` indicator slot per field
(`OCI_IND_NULL` / `OCI_IND_NOTNULL`) -- `execute()` copies the optional into
the staging value (or leaves it default or empty), `query()` copies the
staging value back into the optional (or resets it to `nullopt`) once the
indicator says which after each fetched row.

`std::string`/`optional<std::string>` **binds** fine on the `execute()` side
(size = content length, not `sizeof(std::string)` -- an earlier version of
this file had that bug too, since no demo exercised a raw string bind).
`query()`'s output side still rejects string columns via `static_assert`
(same reason as LOB columns): OCI needs a fixed max buffer size to write
into before it knows how long the value is, and this client doesn't manage
that yet.

## Why OCI binds by position but config binds by name

The first draft of the OCI code bound by name (`OCIBindByName`, deriving
`:PLACEHOLDER` from the struct's field name via `boost::pfr::names_of`).
Field-*name* reflection in `boost::pfr` depends on parsing compiler-specific
`__FUNCSIG__`/`__PRETTY_FUNCTION__` output and isn't reliably available on
MSVC, which is the actual compiler target this idea started from. So
`oci_client.h` binds/defines *by position* instead --
`tuple_size`/`tuple_element_t`/`for_each_field`/`get`, the only primitives it
depends on, always iterate in declaration order on every compiler. The cost:
bind placeholders in your SQL text must occur left-to-right in the same
order as the struct's fields (see the comment on `FinancialUpdate` in
`main.cpp`), not by matching names.

`config_bind.h` makes the opposite call, deliberately: it uses
`boost::pfr::names_as_array<T>()` and matches a struct field to a `Field` of
the same name, case-insensitively (`iequals`). Two reasons position doesn't
work here the way it does for OCI:

- A repeated XML element (see `field_tree.h`) isn't a distinct "array" thing
  in the `FieldList` -- it's just several `Field` entries that happen to
  share a name, wherever they land among their differently-named siblings.
  A `vector<Replica>` field has to gather *all* of them by name; there's no
  single struct-field-index <-> `FieldList`-index correspondence to walk
  positionally once repetition is in play.
- Config files are edited by humans who reasonably expect reordering keys
  not to break parsing, unlike SQL bind parameter order, which has no such
  expectation.

This is a confirmed, deliberate choice for config binding specifically, not
a reversal of the position-based decision for OCI -- see the note at the top
of `reflect.h`.

## Reconnect policy

`OciConnection::run_with_reconnect()` is the one piece of retry logic, used
by both `execute()` and `query()`:

- Runs the given operation once.
- On failure, calls `is_disconnect_error()`, which reads the ORA-code off
  the error handle via `OCIErrorGet` and checks it against a small table of
  known "session is gone" codes (ORA-03113, ORA-01012, ORA-00028, ...).
- If it's a disconnect: sleep `retry_interval`, reconnect, and re-run the
  *entire* operation from scratch -- up to `max_retries` times.
- If it's anything else (bad SQL, a constraint violation, no data found):
  return failure immediately. It is not retried, because retrying an exec
  error just reproduces it.

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
that's a nested `config_schema` struct, or a `std::vector<U>` of one (the
repeated-element case) -- both recurse through the same
`struct_field_auditor` engine used everywhere else in this directory.

`config_bind.h`'s `bind_from_fields<T>(fields, out)` walks `T`'s fields by
name (see above for why) and, per field: an absent `std::optional` leaf
becomes `nullopt`; a present leaf is parsed via `std::from_chars` (rejecting
partial matches like `"10abc"`) or taken as-is for `std::string`; a nested
struct field recurses into the one matching same-named `Field`; a
`std::vector<U>` field recurses into *every* same-named `Field`, in order.
A missing required field, or a value of the wrong shape (a leaf where a
struct was expected, or vice versa), throws `std::runtime_error` naming the
offending field -- config errors are a fail-fast-at-startup case, unlike the
OCI side's error handling, so this doesn't try to be exception-free.

## Dynamic IN (...) lists

`OciClient::query_with_in_list()` / `execute_with_in_list()` handle a
variable-length `WHERE col IN (...)`. `query_template` carries one `{IN}`
marker -- e.g. `"SELECT trade_id, notional FROM trades WHERE trade_id IN
({IN})"` -- which `make_in_placeholders()` expands to `:1,:2,...,:N` sized
to the ID collection, before `OCIStmtPrepare` (Oracle needs the exact
placeholder count fixed in the SQL text itself; there's no variable-arity
bind). `bind_in_list()` then binds each element positionally, reusing the
same `raw_bind_args`/`oci_type_code_v` machinery the struct binder uses.

The ID collection is a `std::set<ElemType>`, not a `std::vector` (a
`std::vector` overload exists too, but only as a convenience that dedupes
into a set before delegating) -- for two reasons:

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

`query_with_in_collection()` is the alternative to `query_with_in_list()`
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
`query_with_in_collection()` then binds the whole collection as one
parameter (`OCIBindByPos` with `SQLT_NTY`, followed by `OCIBindObject`) and
frees it (`OCIObjectFree`) after the fetch loop -- otherwise the same
shape as `query()` in `oci_client.h`.

**This is meaningfully lower-confidence than the rest of this directory.**
Everything else here binds/defines through `OCIBindByPos`/`OCIDefineByPos`
-- a handful of well-documented, thoroughly-used scalar OCI calls. Oracle's
object/collection API (`OCIType`, `OCIObjectNew`, `OCICollAppend`,
`OCIBindObject`) is a genuinely more obscure corner of OCI, and there's no
real `oci.h`/`ociap.h` on this machine to check `oci_object_mock.h`'s
signatures against -- they're written from documentation recall, not
verified the way the rest of this codebase's OCI signatures have been.
What *is* verified: `oci_collection_bind.h` compiles and runs correctly
against its own mock (`examples/collection_demo.cpp`, both a `std::string`
and an `int` element type), proving the C++ template plumbing hangs
together. What's *not* verified is that the mock's signatures faithfully
match a real client's. Diff `oci_object_mock.h` against the actual
`oci.h`/`ociap.h` before trusting this against a live database -- see the
warning banner at the top of `oci_collection_bind.h`.

## What's deliberately not here

- Transaction/commit handling (`OCITransCommit` / `OCI_COMMIT_ON_SUCCESS`) --
  left to the caller.
- String and LOB columns in `query()`'s result rows (`static_assert`s
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
  exactly what `oci_row_schema` enforces; those two column kinds are a real
  exception (a single row's column can itself be struct- or collection-
  shaped), but supporting them for real needs `OCIType`/`OCIDescribe`
  metadata and `OCIObjectNew`/`OCIObjectGetInd`-style APIs -- a different
  mechanism from the `OCIBindByPos`/`OCIDefineByPos` scalar+LOB path
  everything else here is built on. `oci_row_schema` correctly rejects a
  nested/`vector<U>` field today; it just doesn't yet offer a way to
  actually bind one of these two column kinds when you do need it.
- Using the same `OCIType`/`OCIObjectNew`/`OCICollAppend` machinery for
  actual Oracle object-type/nested-table *columns* (the point above) --
  `oci_collection_bind.h` only uses it for one specific purpose (a bound
  IN-list collection), not as a general nested-column-value binder.

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

Not yet verified against MSVC. That matters more for `config_bind.h` than
anywhere else in this directory: it's the one place that uses
`boost::pfr::names_as_array()`, the field-name-reflection feature the rest
of this directory deliberately avoids as MSVC-unreliable (see "Why OCI binds
by position but config binds by name" above) -- confirmed as a deliberate,
working choice in conversation, but still only compiler-tested on g++ here.
The `flat_schema`/`oci_row_schema`/`config_schema` position-based engine in
`reflect.h` has separately already needed two MSVC-specific workarounds (see
the comments on `struct_field_auditor` and `check_field`), so treat MSVC as
untested for this directory generally until it's actually been built there.
