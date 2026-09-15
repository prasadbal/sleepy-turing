# binding (new architecture)

A from-scratch OCI wrapper built around three ideas that came out of
working on `ideas/binding` and hitting real OCI errors that a
reflection-based, free-function design doesn't have a natural place to
prevent: centralized error retrieval (`call_oci`), generic handle
ownership (`OciHandleGuard`), and an explicit, enforced statement
lifecycle (`OciStatement`). Where `ideas/binding` infers everything from
a caller's own C++ struct via `boost::pfr`, this is the lower layer --
type-erased, position/name given explicitly by the caller, no reflection
at all. The two aren't competing designs so much as different altitudes:
this is closer to what `ideas/binding`'s `bind_one_param`/
`define_one_column` are built out of underneath, made into its own
reusable thing.

## Why this exists

Building `ideas/binding`'s by-name column matching and `select()`
surfaced three real, confusing errors in quick succession: `ORA-24324`
("service handle not initialized" -- a service handle with a server but
no session attached), a bare `-2` (`OCI_INVALID_HANDLE`) with no
retrievable error text, and `ORA-24437` ("statement handle not
prepared"). All three come from the same root cause: OCI handles have a
real lifecycle, and using one out of sequence produces an opaque OCI
error rather than a clear "you called this too early" from code that
already knows better. Separately, two real bugs were found in
`ideas/binding` itself: `OCI_NO_DATA` (a `SELECT` finding zero rows) and
`OCI_SUCCESS_WITH_INFO` (a login succeeding with a password-expiry
warning) were both being misclassified as failures, because the
classification logic was written ad hoc, once per call site, rather than
in one place that already knew about every OCI status code. See
`docs/oci_statement_lifecycle_notes.md` (copied from `ideas/binding`,
where this was first written up) for the full story.

## Layout

- `include/binding/oci_compat.h`, `include/binding/oci_mock.h` -- copied
  unchanged from `ideas/binding`: picks the real `<oci.h>` when available,
  otherwise the mock. Both directories share the exact same mock, so a
  bug fixed in one is fixed in both, and there's one shared source of
  truth for "how does the mock simulate OCI" rather than two drifting
  copies.
- `include/binding/oci_call.h` -- `call_oci(func, args...)`: calls any
  OCI function, and whenever the returned status isn't `OCI_SUCCESS`,
  finds the `OCIError*` already present somewhere in `args...` (every OCI
  function takes one) and retrieves whatever `OCIErrorGet` has to offer.
  One place that knows how to get error detail out of *any* OCI call,
  instead of a hand-written `OCIErrorGet` sequence at every call site.
  Deliberately does not decide whether a given status counts as a
  failure for the call that produced it -- `OCI_NO_DATA` means something
  different depending on which function returned it, and that
  interpretation belongs to whoever's calling `call_oci` for that
  specific operation, not to `call_oci` itself.
- `include/binding/oci_handle_guard.h` -- `OciHandleGuard<HandleType,
  HandleTypeEnum>`, one generic RAII wrapper for every OCI handle kind,
  plus the six aliases (`OCIEnvHandle`, `OCIErrorHandle`,
  `OCIServerHandle`, `OCISvcCtxHandle`, `OCISessionHandle`,
  `OCIStmtHandle`). Only `OCIErrorHandle` and `OCIStmtHandle` are
  actually used by this codebase today (`OciConnection` uses `OCILogon2`,
  which manages server/session/service-context atomically and never
  exposes them as separate handles) -- the rest are declared for
  completeness against the full set of handle kinds, and because getting
  the manual `OCIServerAttach`/`OCISessionBegin`/`OCI_ATTR_SESSION`
  sequence right (a codebase *not* using `OCILogon2`'s shortcut) is
  exactly what produced the real `ORA-24324` mentioned above.
- `include/binding/oci_connection.h` -- `OciConnection` (connect via
  `OCIEnvCreate`+`OCILogon2`, `OCIErrorHandle` for the error handle) plus
  `OciConnection::classify(OciCallResult)`, which turns a raw `call_oci`
  result into `Success`/`ConnectionLost`/`QueryError` using this
  connection's own table of "session is gone" ORA-codes. The
  Success/error line is drawn at `status < 0`, not an enumerated list of
  known-good codes -- `OCI_SUCCESS=0`, `OCI_SUCCESS_WITH_INFO=1`,
  `OCI_NEED_DATA=99`, and `OCI_NO_DATA=100` are all `>= 0`; the real
  error codes (`OCI_ERROR=-1`, `OCI_INVALID_HANDLE=-2`,
  `OCI_STILL_EXECUTING=-3123`) are all negative. That's Oracle's own
  convention: execute()/fetch() return their status immediately, without
  a second round trip through `OCIErrorGet`, specifically so the caller
  doesn't have to make that extra call just to learn "this wasn't a
  failure, it was end-of-rows/a login warning/etc" -- the sign of the
  number already says that. `status < 0` reads that convention directly
  instead of re-deriving it as a per-code allowlist that a status this
  codebase hasn't specifically seen yet would fall through incorrectly.
- `include/binding/oci_statement.h` -- `OciStatement`: a statement handle
  with an explicit state enum (`Unprepared -> Prepared -> Executed ->
  EndOfFetch`) and `prepare()`/`bindName()`/`bindNameArray()`/
  `bindOutput()`/`execute()`/`fetch()`/`describeColumnPosition()`/
  `describeColumns()` methods,
  each checking `state()` first and throwing `OciStatementStateError` --
  not returning an `ExecResult` -- on a call sequence that doesn't make
  sense, before any OCI call is attempted at all.
  - `Executed`/`EndOfFetch` are read directly from real OCI's own
    `OCIAttrGet(OCI_ATTR_STMT_STATE)` after every `execute()`/`fetch()`
    call, not re-derived from the call's return status -- OCI already
    knows the handle's actual state; asking it is more honest than
    guessing the same answer a second time. `Unprepared`/`Prepared`
    still have to be tracked here, since OCI reports the same
    `INITIALIZED` value for both. There's no separate "mid-batch-fetch"
    state distinct from `Executed` either, matching what
    `OCI_ATTR_STMT_STATE` itself reports (only three values exist).
  - `bindName()` is a single-row IN parameter by name, type-erased
    (caller supplies the `SQLT_*` code directly).
  - `bindNameArray()` is a chunked array-bind IN parameter -- the
    `insert_rows()` equivalent -- rebinding fresh per chunk at that
    chunk's own starting address rather than `OCIStmtExecute`'s own
    `rowoff` parameter (the same choice `ideas/binding` made, for the
    same reason: `rowoff` crashed against a real database while building
    that one).
  - `bindOutput()` is by position, with an optional `elemSize` for an
    array-of-struct batch fetch (`sizeof(Row)`, matching
    `OCIDefineArrayOfStruct`'s own stride parameter) and an optional
    `rlskip`, the separate stride for the `outsize`/`rlenp` array --
    left at `0` (meaning "same as `elemSize`") for the common case where
    the reported length lives inside the same per-row struct as the
    value itself (`FixedString<N>::length_ref()`), but overridable when
    it doesn't: `select_generic()` (`oci_client.h`, below) reports every
    row's length into its own tightly-packed `vector<ub2>`, a different
    stride from the column's own per-row byte width, and that mismatch
    was a real bug caught before it shipped (a `VARCHAR2` column's real
    fetched lengths landed at the wrong stride and read back wrong).
  - `describeColumns()` is `describeColumnPosition()`'s sibling: every
    column's real name, position, native OCI type code, and size in one
    `OCIParamGet` pass, for a caller that doesn't want to declare a row
    struct at all -- `select_generic()` is what actually uses it.
  - `bindName()`/`bindNameArray()`/`bindOutput()`/`execute()` are all
    valid in either the `Prepared` or `Executed` state -- not just
    `Prepared` -- specifically so two real patterns both work on one
    prepared statement without ever re-preparing: chunked array-bind
    insert (bind, execute, rebind the next chunk, execute again) and
    by-name output matching (`describeColumnPosition()`, next, needs
    `execute()` to have already run before a name can resolve to a
    position, so `bindOutput()` has to be callable *after* execute() too).
  - `fetch()`/`describeColumnPosition()`/`bindOutput()` are also valid in
    `EndOfFetch`, not just `Executed` -- a real, worth-recording finding,
    confirmed at all three call sites separately: with
    `set_prefetch_rows()` set above the real row count, real Oracle
    reports `OCI_ATTR_STMT_STATE == END_OF_FETCH` immediately after
    `execute()` -- sometimes before `fetch()` has ever run, sometimes
    (a genuinely small result set) before `bindOutput()` has even had a
    chance to define an output column at all -- because the server has
    nothing more to send even though the client hasn't drained its own
    prefetch cache into the caller's bind buffers yet. See
    `docs/oci_statement_lifecycle_notes.md` for both real crashes this
    produced and the fix (a batch loop must stop on the individual
    `fetch()` call's own `OCI_NO_DATA`, not on `state() == EndOfFetch`).
- `include/binding/oci_log.h` -- `set_statement_logger(logger)`: opt-in,
  off by default. `OciStatement::execute()` logs the SQL text plus every
  `bindName()`'d value (rendered via `render_typed_value()`, which
  switches on the *runtime* `SQLT_*` code -- there's no C++ type to
  dispatch on on this type-erased layer -- covering integer widths,
  float/double, a character buffer, and `OciDate` via a real
  `OCIDateToText` call) as one line per `execute()` call.
- `include/binding/oci_lob.h` -- `OCILob`: the LOB *locator* lifecycle
  (`OCIDescriptorAlloc`, `OCILobCreateTemporary`/`OCILobWrite2` on the way
  in, `OCILobGetLength2`/`OCILobRead2` on the way out, all the frees)
  as its own owned object, plus `OciClob`/`OciBlob`, the user-facing
  value types on top of it (a plain `std::string`/`vector<unsigned char>`
  wrapper, ported unchanged from `ideas/binding`'s own `oci_lob.h`) that
  never touch an `OCILobLocator*` directly. `oci_client.h`'s reflection
  layer is what actually constructs an `OCILob` (transiently, once per
  bind or once per fetched row) and copies bytes in and out of these --
  see that file's own comment for how that staging works.
- `include/binding/oci_fixed_string.h`, `oci_datetime.h` -- `FixedString<N>`
  and `OciDate`, ported unchanged from `ideas/binding` (both are already
  self-contained, needing only `oci_compat.h`/`oci_connection.h`).
- `include/binding/oci_client.h` (+ `details/oci_client.h`) -- the
  reflection layer `ideas/binding` has (`execute()`/`select_rows()`/
  `select()`/`insert_rows()`, each walking a plain struct via
  `boost::pfr`), rebuilt to drive `OciStatement` instead of raw OCI
  calls. Two things fall out of that for free rather than needing their
  own machinery here: query logging (every `bindName()` call already
  logs itself when `set_statement_logger()` is installed) and the
  `EndOfFetch`-tolerant lifecycle (inherited straight from
  `OciStatement`). A LOB field's staging slot is an `OCILob` -- or a
  `std::vector<OCILob>`, one per batch row, on the fetch side, with
  `elemSize = sizeof(OCILob)` as the array-of-struct stride, since every
  element of that vector lays its own locator out at the same relative
  offset -- so this file's own bind/define/apply code only ever calls
  `create_temporary()`/`write()`/`read()` on it and never touches
  `OCIDescriptorAlloc`/`OCILobWrite2`/`OCILobRead2`/`OCILobFreeTemporary`
  itself. The apply loop that copies a fetched batch's optional/LOB
  staging values into the caller's row struct is skipped entirely (not
  just a per-field no-op) via `needs_apply_loop_v<T>`, a compile-time
  check of whether `T`'s staging tuple is all `std::monostate` -- true
  for the common case of a plain-scalar row type, which never needs a
  post-fetch copy since every field already landed directly in the
  batch via `bindOutput()`.

  `select_generic()`, same file, is the no-struct-at-all counterpart:
  describes a query's columns via `describeColumns()` and defines every
  one of them using its own described OCI type and size, completely
  unconverted -- a `NUMBER` column stays `SQLT_NUM`, raw Oracle-internal
  bytes; a `VARCHAR2`/`CHAR` column stays `SQLT_CHR`/`SQLT_AFC`, raw
  bytes plus a real per-row length. No `boost::pfr`, no row type declared
  anywhere -- for a caller that doesn't know a row shape ahead of time,
  or wants to measure this layer's raw fetch throughput with no
  decode/convert step at all between OCI and the callback (`select_rows`
  decodes into real C++ types as it goes; this hands back exactly what
  Oracle described). A `SQLT_CLOB`/`SQLT_BLOB` column is rejected with a
  clear `QueryError` before any fetch is attempted -- a locator isn't a
  flat byte buffer the way every other described type here is.
- `examples/demo.cpp` -- mock-based, 10 demos covering connect, plain
  execute, the state-check exception, `bindName()`, a single-row fetch,
  a batch fetch loop, the `OCI_NO_DATA` zero-row case, `OCILob`,
  `set_statement_logger()`, and `bindNameArray()`.
- `examples/demo_client.cpp` -- mock-based, exercises `oci_client.h`'s
  four struct-based functions directly: `execute()` (plain and with bind
  params, including an optional field), `select_rows()` (plain-scalar
  row with the apply loop elided, and an optional-field row with it
  engaged), `select()`, `insert_rows()`, and an `OciClob` field on both
  the bind and fetch sides. (`select_generic()` needs a real describable
  backend -- see `live_oracle_generic_demo.cpp` below, not this file.)
- `examples/live_oracle_demo.cpp`, `live_oracle_client_demo.cpp`,
  `live_oracle_generic_demo.cpp` -- the same shapes (`OciStatement`
  directly, `oci_client.h`'s struct-based functions, and
  `select_generic()` respectively), verified against a real database.
  Not part of any CMake build; compile directly (see each file's own
  header comment).
- `examples/live_oracle_empty_table_demo.cpp` -- a genuinely empty table
  (zero rows, not just zero *matching* rows) reaches `EndOfFetch` during
  `execute(0)` itself, guaranteed rather than conditional on
  `prefetch_rows` -- the same mechanism as the prefetch-overrun finding
  above, just impossible to avoid instead of only likely. Verifies every
  entry point that could be affected (`select_generic()`, `select_rows`,
  `select`, and `OciStatement` directly) handles it cleanly with no
  exception and no garbage output.
- `docs/oci_statement_lifecycle_notes.md` -- copied from `ideas/binding`:
  the six OCI status codes and why there are that many rather than one
  generic failure code, the statement lifecycle state machine, the
  `OCIStmtPrepare2`/`OCIStmtRelease` lifecycle wrinkle, and the design
  notes this architecture is actually built from.

## What this doesn't have (yet)

Nothing left unbuilt at the scope `ideas/binding` covers: array-bind
insert (`bindNameArray()`/`insert_rows()`), by-name output matching
(`describeColumnPosition()`), query logging (`set_statement_logger()`),
and the reflection layer itself (`oci_client.h`) were all added after
the initial pass -- see "Testing against a real database" below for
each one's real verification. What's different from `ideas/binding` is
only the layer this one's reflection functions are built on
(`OciStatement`'s state-checked, `call_oci`-backed primitives, vs. raw
OCI calls there directly) and the LOB staging mechanism (an owned
`OCILob` object vs. a raw locator plus hand-rolled alloc/free
functions) -- not the set of OCI operations either architecture can
actually do.

## Testing against a real database

`examples/live_oracle_demo.cpp`, run against `oracle-free` (the same
container `ideas/binding` uses), all checks passed on the first real
run:

```
[OK] connect() succeeded
[OK] CREATE TABLE succeeded
[OK] fetch() before execute() threw OciStatementStateError, no OCI call attempted
[OK] insert row 1/2/3 succeeded
[OK] single-row select() succeeded
[OK] id=2, notional=3.0 fetched correctly
[OK] collected all 3 rows across multiple fetch() calls
[OK] took more than one fetch() call for 3 rows at batch size 2
[OK] rows came back in id order: 1, 2, 3
[OK] zero-row query classified Success
[OK] oci_status is OCI_NO_DATA (100)
[OK] output left untouched when no row matched
[OK] state transitioned straight to EndOfFetch
[OK] bad table name -> QueryError
[OK] call_oci retrieved real error text via OCIErrorGet
    error_code=24374 error_text=ORA-24374: define not done before fetch or execute and fetch
[OK] LOB insert succeeded
[OK] LOB select succeeded
[OK] LOB content round-tripped byte-for-byte through a real insert+select
```

Two real things worth noting from this run, not just "it passed":

- The zero-row case's `oci_status` came back exactly `100` (`OCI_NO_DATA`)
  and `status` `Success` -- confirming `OciConnection::classify()` gets
  this right from a real database, not just the mock, which is the exact
  bug `ideas/binding`'s own `OciConnection::execute()` had until it was
  found and fixed.
- `call_oci` retrieved a real `ORA-24374` for the "bad table name" check
  -- an artifact of that particular query never calling `bindOutput()`
  first (Oracle's own error, not a bug here), but exactly the point:
  `call_oci`'s generic error-retrieval mechanism surfaced a genuine,
  specific, correct Oracle error message with zero call-site-specific
  code, on the first attempt.

One real fix needed along the way: two calls (`OCIStmtExecute`,
`OCILobWrite2`) originally passed `static_cast<void*>(nullptr)`/
`static_cast<const dvoid*>(nullptr)` for arguments that real `<oci.h>`
types more strictly than the mock does (`const OCISnapshot*`, an
`OCICallbackLobWrite2` function pointer) -- `call_oci`'s template deduces
`Args...` from exactly what's passed, so a `void*`-cast `nullptr` doesn't
implicitly convert to a stricter pointer type at the real call site the
way a bare, uncast `nullptr` (whose type is `std::nullptr_t`, implicitly
convertible to *any* pointer type) does. Fixed by removing those casts;
worth remembering for any future `call_oci` call site passing a null
pointer argument -- pass it bare, not pre-cast to `void*`/`dvoid*`.

### `set_statement_logger()`: real `OciDateToText` rendering

```
[OK] exactly one log line for the INSERT
    SQL: INSERT INTO new_arch_log_test VALUES(:id, :cob_date) | id=1, cob_date='13-SEP-26'
[OK] id rendered correctly
[OK] OciDate rendered via a real OCIDateToText call, not a placeholder
```

`render_typed_value()`'s `SQLT_ODT` branch calls the real `OCIDateToText`
-- confirmed producing an actual formatted date (`'13-SEP-26'`), not a
raw `::OCIDate` struct dump, against a real database.

### `describeColumnPosition()`: by-name matching, reversed order + extra column

The same adversarial shape `ideas/binding`'s own by-name matching was
proven against: a table `(id, middle_extra, name)`, queried in that
column order, with output variables bound in the *opposite* order (name
first, id second) via names resolved through `describeColumnPosition()`
rather than assumed from declaration order:

```
[OK] select execute() succeeded
[OK] describeColumnPosition("name") resolved to real position 3
[OK] describeColumnPosition("id") resolved to real position 1
[OK] fetch() after post-execute bindOutput() succeeded
[OK] id=7, name=SEVEN correct despite reversed bind order + extra middle column
[OK] a column name with no match returns 0, not a wrong guess
```

A real, worth-recording mistake surfaced building this test, not in
`OciStatement` itself: the first attempt passed `nullptr` for the
`SQLT_CHR` (`VARCHAR2`) column's `outsize` (`rlenp`) parameter, and
Oracle responded by blank-padding the entire 16-byte buffer ("SEVEN"
followed by 11 trailing spaces) instead of writing just the 5 real bytes
and leaving the rest alone. `ideas/binding` always passes a real
`length_ref()` for exactly this reason (see `FixedString<N>`'s own
comments); this test hadn't been carrying that forward until it broke
this same way here. Fixed by passing a real `ub2 name_len` and using it
to bound the read (`std::string(name_buf, name_len)`), not
`std::strlen`/null-termination -- a `SQLT_CHR` fetch is explicit-length,
not null-terminated, exactly as documented everywhere else in this
codebase and `ideas/binding` both.

### `bindNameArray()`: chunked array-bind insert

7 rows, chunk size 3 (`3+3+1` -- a genuine partial final chunk), one
`OCIStmtPrepare` call, three `bindNameArray()`+`execute()` cycles on the
same `OciStatement`, never re-preparing:

```
[OK] all 3 chunks (3+3+1) executed successfully, same prepared statement throughout
[OK] all 7 rows actually landed in the table, not just 3 or a partial count
[OK] row 7 (from the final, partial chunk) has the correct value: id=7, notional=10.5
```

No crash, no wrong values, on the first attempt -- worth calling out
specifically because `ideas/binding`'s own array-bind insert did crash
the first time it was tried against a real database (using
`OCIStmtExecute`'s own `rowoff` parameter to reuse a single bind across
chunks, rather than rebinding fresh per chunk). `bindNameArray()` here
was written knowing that finding already, rebinding at each chunk's own
starting address from the start rather than rediscovering the same
crash the hard way a second time. The spot-check on row 7 specifically
exercises the case a stride or rebind-offset mistake would get wrong --
the last row of the last, partial chunk, furthest from the "happens to
work because the first chunk always starts at offset 0" case.

### `oci_client.h`: the reflection layer, end to end

`examples/live_oracle_client_demo.cpp` exercises all four functions
together against a real table -- named binds with a nullable field,
by-name output matching with a deliberately non-declared column order,
`select()`, a chunked `FixedString<N>` array-bind insert, and an
`OciClob` field on both the bind and fetch sides:

```
[OK] insert row 1 (notional=2.5) succeeded
[OK] insert row 2 (notional=NULL) succeeded
[OK] select_rows() succeeded
[OK] collected exactly 2 rows
[OK] row 1: id=1, notional=2.5, name=Alpha (by-name matching, non-declared column order)
[OK] row 2: notional correctly round-tripped as NULL (nullopt), not a stale value
[OK] SELECT COUNT(*) via select() == 2
[OK] insert_rows() (3+3+1 chunks) succeeded
[OK] row count is 9 (2 earlier + 7 bulk)
[OK] last row of the partial final chunk (id=16) has the correct FixedString + scalar value
[OK] LOB insert via execute() succeeded
[OK] LOB select_rows() succeeded
[OK] LOB content round-tripped byte-for-byte through a real OCILob-backed locator
```

Building this surfaced one more real, genuine finding, not just a
mechanical port: `bindOutput()` needed the same `EndOfFetch` relaxation
`fetch()`/`describeColumnPosition()` already had, for a reason specific
to this layer's own call order. `select_rows()`'s column-name resolution
requires `execute()` to run *before* `bindOutput()` can be called (see
the state-machine section above), and with a small real result set plus
`prefetch_rows` set above the actual row count, `execute(0)` alone was
enough to already flip `OCI_ATTR_STMT_STATE` to `END_OF_FETCH` --
*before* a single output column had been defined. `bindOutput()` threw
`OciStatementStateError` the first time this ran against a live
database; relaxing it to also accept `EndOfFetch` (same as the other
two) fixed it. See `docs/oci_statement_lifecycle_notes.md` for the full
writeup -- this is now confirmed at three separate call sites, not a
one-off.

### A plain `FixedString<N>` field is never `std::optional`, because Oracle already treats empty and NULL as the same thing

Oracle's SQL engine can't store an empty `VARCHAR2`/`CHAR` value distinct
from `NULL` -- inserting `''` always lands as `NULL`, and a query that
matches a NULL column can't be told apart client-side from one that
matched a genuinely empty string. `execute<T>()`/`select_rows<T>()`/
`select<T...>()` all lean into that instead of fighting it: a plain
(non-`optional`) `FixedString<N>` field binds a zero-length value as an
explicit `NULL`, and fetches a `NULL` column back as `view().empty()`.
`std::optional<FixedString<N>>` is rejected outright by `scalar_bindable`
(`is_scalar_bindable_field_v` in `oci_client.h` deliberately checks
`is_fixed_string_v<U>` only, not `is_fixed_string_v<optional_value_t<U>>`
-- the same exclusion `is_oci_lob_v` already has, for the same reason):
allowing it would mean two different ways to say the same thing (`{}` vs.
an empty string) for one field, which is worse than just not allowing
it. This mirrors how a LOB field is handled -- never wrapped in
`optional<>`, its own presence/absence encoded some other way (a real
indicator, always present, checked instead of inferred) -- rather than
being a special case invented just for `FixedString<N>`.

This closed a real, previously-latent gap, not just a nicety: a plain
`FixedString<N>` output column used to define with **no indicator at
all** (matching `ideas/binding`'s own comment on the same point -- "an
unexpected NULL there silently leaves that row's slot holding whatever
the batch buffer already had"). `select_rows()`'s array-of-struct batch
fetch reuses the same one-row (or N-row) buffer across every
`fetch()` call, so a NULL row landing in a slot a *previous* fetch call
had already filled with real content would silently keep that stale
value -- a real correctness bug, not a theoretical one.
`examples/live_oracle_client_demo.cpp` verifies this exact scenario
against a real database: row 202 (real content: `"Real"`) is fetched
first into a one-row batch buffer (`fetch_batch_size = 1`, so the same
buffer is genuinely reused call to call), then row 201 (a real SQL
`NULL`) is fetched into that same buffer on the next call --

```
[OK] row 202 (fetched first) has its real content
[OK] row 201 (SQL NULL, fetched second into the SAME reused buffer) is empty, not stale 'Real'
[OK] row 200 (empty-string bind) fetched back as empty, not garbage
```

-- confirming the fix actually clears the field rather than merely
happening to pass because a freshly-constructed buffer starts zeroed
anyway. The same fix applies to `select()`'s positional `FixedString<N>`
outputs (a real indicator now, cleared back to empty on NULL, and left
untouched -- not even indicator-checked -- when the query matches zero
rows at all, matching this function's existing "arguments untouched on
`OCI_NO_DATA`" contract).

Not extended to `insert_rows()`'s array-bind path: a per-row NULL there
would need a per-row indicator computed from each row's own content
length, not the single blanket `OCI_IND_NOTNULL` fill that path uses
today (see its own file comment for why array-bind indicators can't be
`nullptr` at all). Left as a known, narrower follow-up rather than
bundled in here.
