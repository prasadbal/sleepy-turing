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
  Fetching -> EndOfFetch`) and `prepare()`/`bindName()`/`bindNameArray()`/
  `bindOutput()`/`execute()`/`fetch()`/`describeColumnPosition()` methods,
  each checking `state()` first and throwing `OciStatementStateError` --
  not returning an `ExecResult` -- on a call sequence that doesn't make
  sense, before any OCI call is attempted at all.
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
    `OCIDefineArrayOfStruct`'s own stride parameter).
  - `bindName()`/`bindNameArray()`/`bindOutput()`/`execute()` are all
    valid in either the `Prepared` or `Executed` state -- not just
    `Prepared` -- specifically so two real patterns both work on one
    prepared statement without ever re-preparing: chunked array-bind
    insert (bind, execute, rebind the next chunk, execute again) and
    by-name output matching (`describeColumnPosition()`, next, needs
    `execute()` to have already run before a name can resolve to a
    position, so `bindOutput()` has to be callable *after* execute() too).
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
  as its own owned object, not the user-facing value type. A value type
  wrapping a plain `std::string`/`vector<unsigned char>` (`ideas/binding`'s
  `OciClob`/`OciBlob`) would sit on top of this and never touch an
  `OCILobLocator*` directly, the same separation `ideas/binding` already
  has via free functions (`make_temp_lob`/`free_temp_lob`/
  `read_lob_bytes`) -- this is that same idea as an object instead.
- `examples/demo.cpp` -- mock-based, 10 demos covering connect, plain
  execute, the state-check exception, `bindName()`, a single-row fetch,
  a batch fetch loop, the `OCI_NO_DATA` zero-row case, `OCILob`,
  `set_statement_logger()`, and `bindNameArray()`.
- `examples/live_oracle_demo.cpp` -- the same shapes, verified against a
  real database. Not part of any CMake build; compile directly (see the
  file's own header comment).
- `docs/oci_statement_lifecycle_notes.md` -- copied from `ideas/binding`:
  the six OCI status codes and why there are that many rather than one
  generic failure code, the statement lifecycle state machine, the
  `OCIStmtPrepare2`/`OCIStmtRelease` lifecycle wrinkle, and the design
  notes this architecture is actually built from.

## What this doesn't have (yet)

No reflection layer at all -- every field is bound/defined one call at a
time, by the caller, with an explicit OCI type code. `ideas/binding`'s
whole value proposition (`boost::pfr` walking a plain C++ struct's fields
automatically) isn't reproduced here; this is the layer something like
that would be rebuilt on top of, not a gap in this one -- a reflection
layer built on top of `OciStatement` would call `bindName()`/
`bindNameArray()`/`bindOutput()` per field the same way `ideas/binding`'s
`bind_one_param`/`define_one_column` call raw OCI functions today.

Array-bind insert (`bindNameArray()`), by-name output matching
(`describeColumnPosition()`), and query logging
(`set_statement_logger()`) were all added after the initial pass -- see
"Testing against a real database" below for each one's real
verification. At this point every capability `ideas/binding` has at the
scalar-bind/fetch level has a type-erased counterpart here; what's
different is the layer above it (reflection vs. explicit type codes),
not the set of things OCI operations this architecture can do.

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
