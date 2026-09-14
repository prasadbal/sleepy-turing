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
  connection's own table of "session is gone" ORA-codes -- `OCI_SUCCESS`,
  `OCI_SUCCESS_WITH_INFO`, and `OCI_NO_DATA` all classify as `Success`.
- `include/binding/oci_statement.h` -- `OciStatement`: a statement handle
  with an explicit state enum (`Unprepared -> Prepared -> Executed ->
  Fetching -> EndOfFetch`) and `prepare()`/`bindName()`/`bindOutput()`/
  `execute()`/`fetch()` methods, each checking `state()` first and
  throwing `OciStatementStateError` -- not returning an `ExecResult` --
  on a call sequence that doesn't make sense, before any OCI call is
  attempted at all. `bindName()` is by name, type-erased (caller supplies
  the `SQLT_*` code directly); `bindOutput()` is by position, with an
  optional `elemSize` for an array-of-struct batch fetch (`sizeof(Row)`,
  matching `OCIDefineArrayOfStruct`'s own stride parameter).
- `include/binding/oci_lob.h` -- `OCILob`: the LOB *locator* lifecycle
  (`OCIDescriptorAlloc`, `OCILobCreateTemporary`/`OCILobWrite2` on the way
  in, `OCILobGetLength2`/`OCILobRead2` on the way out, all the frees)
  as its own owned object, not the user-facing value type. A value type
  wrapping a plain `std::string`/`vector<unsigned char>` (`ideas/binding`'s
  `OciClob`/`OciBlob`) would sit on top of this and never touch an
  `OCILobLocator*` directly, the same separation `ideas/binding` already
  has via free functions (`make_temp_lob`/`free_temp_lob`/
  `read_lob_bytes`) -- this is that same idea as an object instead.
- `examples/demo.cpp` -- mock-based, 8 demos covering connect, plain
  execute, the state-check exception, `bindName()`, a single-row fetch,
  a batch fetch loop, the `OCI_NO_DATA` zero-row case, and `OCILob`.
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
that would be rebuilt on top of. No by-name output column matching (that
needs `OCIParamGet`/`OCI_ATTR_NAME`, described in
`docs/oci_statement_lifecycle_notes.md`, layered on top of `bindOutput()`
-- not done here). No array-bind insert (`ideas/binding`'s
`insert_rows()`). No query logging. All of these are straightforward to
add on top of `OciStatement` once its own shape is settled; none were
built in this first pass, which focused on getting the handle lifecycle,
error retrieval, and state checking right and verified against a real
database first.

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
