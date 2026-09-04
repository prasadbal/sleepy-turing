# Dynamic IN (...) lists

There are two distinct mechanisms here, for two distinct call shapes --
don't confuse them:

1. **A struct field that's itself a container** -- one parameter among
   several in a normal `execute()`/`insert()` bind struct, e.g. `UPDATE
   trades SET status = :status WHERE trade_id IN (...)`. Still
   placeholder-expansion, still subject to Oracle's 1000-element cap.
2. **A standalone ID collection**, the query's only dynamic input --
   `select_with_in_collection()`/`execute_with_in_collection()`. A single
   bound Oracle collection object, no element-count cap.

An earlier version of this library also had a *standalone placeholder*
mechanism (`select_with_in_list()`/`execute_with_in_list()`/
`make_in_placeholders()`/`bind_in_list()`) for the second call shape --
removed once collection-bind proved to dominate it in every way that
matters (see git history, commit `binding: remove placeholder-based
IN-list (dominated by collection bind)`, if you need the old approach as
a reference). The two limitations that made it worth removing are exactly
the two the struct-field mechanism (#1 below) still hasn't escaped, which
is why that one is still called out as an open question at the end of
this file.

## 1. A struct field can itself be a dynamic multi-value IN-list

`bindable`'s field predicate accepts a `std::vector<U>`/`std::set<U>`/
`std::valarray<U>` field, for `U` a bindable leaf, alongside ordinary
scalar fields in the same struct:

```cpp
struct TradeStatusUpdate {
    std::string status;      // an ordinary named parameter: :status
    std::set<int> trade_ids; // a dynamic multi-value IN-list: {trade_ids}
};

client.execute(conn, "UPDATE trades SET status = :status WHERE trade_id IN ({trade_ids})", filter);
```

`detail::substitute_container_markers` (called once, unconditionally, at
the top of `run_execute_once` -- a no-op if `T` has no container field)
replaces each container field's own `{field_name}` marker with a named
placeholder list sized to that field's current element count
(`make_named_placeholders`, e.g. `:trade_ids_0,:trade_ids_1,:trade_ids_2`),
*before* `OCIStmtPrepare` -- Oracle needs the exact placeholder count
fixed in the SQL text itself. `detail::bind_named_container` then binds
each element by that generated name (`OCIBindByName`, matching how every
other field on this side binds). A missing marker for a container field
throws immediately rather than silently preparing a statement whose binds
have nowhere to land.

A struct's container field is **not** deduped/sorted into a set first (a
`std::set<U>` field is naturally already ordered/deduped by being a set,
but a `std::vector<U>`/`std::valarray<U>` field binds exactly what the
caller put there, in order, duplicates included) -- deduping is specific
to mechanism #2 below, not something every collection bind should
silently apply to a caller's own data.

`select()`'s output side explicitly rejects a container-typed field via
`static_assert` -- a single result column can't fetch into a
variable-length container (that's what multiple *rows* are for).

`make_named_placeholders` enforces the same **ORA-01795** limit described
below (a plain `IN (...)` list is capped at 1000 expressions by the
parser itself, whether its elements are literals or bind placeholders) --
this mechanism has not been migrated to a collection bind, so it's still
subject to that cap. See "Open question" at the end of this file.

## 2. A standalone ID collection, bound as one Oracle collection object

`select_with_in_collection()`/`execute_with_in_collection()`
(`oci_collection_bind.h`) handle the case where the values to match
against aren't already sitting in a bind struct's own field -- the query's
IN-list is its only dynamic input:

```cpp
std::vector<int> ids = /* ... */;
std::vector<TradeRow> rows;
select_with_in_collection(conn,
    "SELECT trade_id, notional FROM trades WHERE trade_id IN (SELECT column_value FROM TABLE(:1))",
    ids, rows);
```

Instead of generating a placeholder list sized to the collection, this
binds *one* Oracle collection object and queries against
`TABLE(:1)`. The SQL text is fixed regardless of how many elements are in
the collection -- no ORA-01795 cap, no shared-pool cursor-cache
fragmentation as sizes vary between calls.

`OciCollectionTypeBinder<ElemType>` maps the element type to an Oracle SQL
collection type to bind through, defaulting to Oracle's built-in ODCI
schema types so the common case needs no schema setup:
`SYS.ODCINUMBERLIST` for arithmetic types, `SYS.ODCIVARCHAR2LIST` for
strings. `build_in_collection()` looks up that type (`OCITypeByName`),
creates a new empty instance of it (`OCIObjectNew`), and appends every
element (`OCICollAppend`, via `OCINumberFromInt` for a NUMBER element).
`select_with_in_collection()` binds the whole collection as one parameter
(`OCIBindByPos` with `SQLT_NTY`, followed by `OCIBindObject`) and frees it
(`OCIObjectFree`) after the fetch loop -- otherwise the same shape as
`select()` in `oci_client.h`. `execute_with_in_collection()` is the DML
counterpart (e.g. a `DELETE ... WHERE id IN (SELECT column_value FROM
TABLE(:1))`) -- same mechanism, no result rows. Both take
`std::set<ElemType>` directly, plus `std::vector<ElemType>`/
`std::valarray<ElemType>` convenience overloads that dedupe into a set
first (a repeated value is never meaningful in an IN-list, and a
deterministic element order keeps two calls over the same logical ID set
generating the same bind order).

### Requires OCI_OBJECT environment mode

Oracle's object/collection API (`OCITypeByName`, `OCIObjectNew`,
`OCICollAppend`, `OCIBindObject`) needs an object cache that only exists
when the environment is created with `OCI_OBJECT` (not `OCI_DEFAULT`).
`OciConnection::connect()` creates its environment with `OCI_OBJECT` for
exactly this reason -- under `OCI_DEFAULT`, every call above still
succeeds individually except the type lookup, which fails with
`ORA-21301: not initialized in object mode`. `OCI_OBJECT` is additive
over the plain scalar bind/define path (nothing that doesn't touch
collections behaves differently under it), so there's no separate
"object mode" connection type to opt into.

### Verified against a real Oracle database -- for an int/double element

Compiled clean (`-fsyntax-only`) against real `oci.h`/`ociap.h` from a
live Oracle 23.26.3.0.0 ("26ai") instance, then actually run against it
(Instant Client 19.32.0.0.0, chosen over the matching 23.26 client
specifically to test client/server version skew) with a genuine
10,000-element `std::vector<int>` IN-list against a real 20,000-row
table:

- `select_with_in_collection`: 10,000/10,000 matching rows returned,
  checksum of returned ids matched the expected sum exactly.
- `execute_with_in_collection` (`DELETE ... WHERE id IN (...)`): removed
  all 10,000 rows in one call; a follow-up `select_with_in_collection`
  over the same id set confirmed 0 remaining.
- No `ORA-01795` at any point -- the entire point of this mechanism over
  a generated placeholder list, and 10x past the 1000-element cap the
  removed standalone placeholder mechanism would have hit.

This is meaningfully stronger evidence than `examples/collection_demo.cpp`
alone provides -- that demo only exercises the C++ template plumbing
against this repo's own mock (`oci_object_mock.h`), which proves the
logic hangs together but says nothing about whether the mock's OCI
signatures match a real client's. The live test above is what actually
confirms that -- for `SYS.ODCINUMBERLIST` (an arithmetic `ElemType`).

**A `std::string` element (`SYS.ODCIVARCHAR2LIST`) is a different,
currently broken story** -- see the "KNOWN BROKEN" banner at the top of
`oci_collection_bind.h`. `OCICollAppend` segfaults inside Oracle's own
kernel object layer when appending a string element to a real collection,
confirmed with a minimal, fully status-checked standalone repro (not a
mock artifact, not a downstream effect of anything else in this
library). Tried and ruled out: a raw null-terminated `char*` as `elem`,
and a properly constructed `OCIString*` via `OCIStringAssignText` (the
documented way to represent a VARCHAR2 collection element) -- both crash
in the identical place. Root cause not identified. Don't use a
`std::string` `ElemType` against a real database until this is resolved;
the mock-only `examples/collection_demo.cpp` still exercises it (that's
all the mock ever proved for this case, and remains all it proves).

## Open question: should mechanism #1 move to a collection bind too?

Mechanism #1 (a struct's own container field) is still
placeholder-expansion under the hood (`make_named_placeholders`), even
though its *name* suggests it might share the collection-object mechanism
#2 uses. It doesn't -- it was never migrated when the standalone
placeholder mechanism was removed in favor of collection-bind, because
that removal only covered the standalone case. Mechanism #1 still has
both problems the removed code had: a 1000-element cap, and shared-pool
cursor-cache fragmentation as a struct's container field size varies
between calls.

Whether to migrate `bind_named_container`/`substitute_container_markers`
to bind a real Oracle collection object per container field (the same way
mechanism #2 does), instead of generating a sized placeholder list, is an
open design question -- not yet decided or implemented.
