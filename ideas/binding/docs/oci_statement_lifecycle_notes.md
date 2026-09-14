# OCI statement lifecycle, error codes, and handle abstractions -- working notes

Written up from a conversation about a real class of bugs found while
building `select()` and `select_rows()`'s by-name column matching in this
directory: OCI's status codes and a statement handle's implicit state
machine are both more nuanced than a single `== OCI_SUCCESS` check (or a
single `!= OCI_SUCCESS` check) captures, and getting either wrong produces
exactly the confusing symptoms hit today -- a spurious failure on a
perfectly good outcome, or an opaque `ORA-#####` from using a handle in a
state it wasn't in.

## The status code zoo, and why there are six of them

```
OCI_SUCCESS            0
OCI_SUCCESS_WITH_INFO   1
OCI_NEED_DATA          99
OCI_NO_DATA           100
OCI_ERROR              -1
OCI_INVALID_HANDLE     -2
OCI_STILL_EXECUTING -3123   (async only -- nothing in this codebase uses OCI asynchronously)
```

The natural instinct -- "wouldn't it be simpler if failure were always
`-1`, and you always called `OCIErrorGet` for the details?" -- is a fair
question, and the answer is that OCI *almost* does that, except for two
real distinctions the codes are protecting:

**`OCI_INVALID_HANDLE` (-2) means "don't trust `OCIErrorGet` here."**
Every other negative-or-informational code (including `OCI_ERROR`)
implies the error handle itself is in a valid state to query for detail.
`OCI_INVALID_HANDLE` specifically means one of the handles involved --
sometimes the error handle itself -- is structurally wrong, which is
exactly why it tends to come back with no retrievable error text (see
this project's own hunt for `ORA-24437`/`-2` earlier in this
conversation). Folding this into a single `-1` would erase the one piece
of information that actually matters when you hit it: whether asking for
more detail is even safe to attempt.

**`OCI_NEED_DATA` (99) and `OCI_STILL_EXECUTING` (-3123) aren't failures
at all -- they're flow control.** They mean "this piecewise or
asynchronous operation isn't finished; call me again," not "something
went wrong." There is nothing for `OCIErrorGet` to report in either case
-- collapsing them into a generic failure code would force every caller
to immediately turn around and disambiguate "genuinely failed" from "not
done yet" via some *other* signal, which is strictly more work than a
distinct return value already flagging it.

So the six codes reduce to three real buckets, and the practical
takeaway is which bucket a given piece of code needs to check for,
explicitly, rather than assuming `== OCI_SUCCESS` covers "everything
that isn't a problem":

| Bucket | Codes | What to do |
|---|---|---|
| Real failure, detail retrievable | `OCI_ERROR` | `OCIErrorGet` for the `ORA-#####` text |
| Real failure, detail not trustworthy | `OCI_INVALID_HANDLE` | Fix the calling code's handle usage; don't rely on `OCIErrorGet` here |
| Not a failure | `OCI_SUCCESS`, `OCI_SUCCESS_WITH_INFO`, `OCI_NO_DATA`, `OCI_NEED_DATA`\*, `OCI_STILL_EXECUTING`\* | Proceed -- but each carries different follow-up meaning |

\* Not used synchronously the way this codebase calls OCI; listed for completeness.

### Two real bugs this codebase had, from checking only the first bucket

Both were "only `OCI_SUCCESS` counts as success," silently misclassifying
a legitimate outcome as a failure:

- **`OciConnection::execute()`** treated `OCI_NO_DATA` as `QueryError`.
  `OCI_NO_DATA` only ever comes from a `SELECT`-shaped execute (`iters >
  0` fetching as part of execute itself -- `select()`'s own mechanism)
  finding zero matching rows -- never from DML, where a zero-row
  `UPDATE`/`DELETE` is an ordinary `OCI_SUCCESS`. Zero rows found isn't a
  query failure; it's the answer. Fixed: `status == OCI_SUCCESS ||
  status == OCI_NO_DATA` both count as `Success`, with `oci_status`
  still carrying the real value so a caller can tell "found data" apart
  from "didn't" without a dedicated `ExecStatus` value just for this.
  (This is also why `select()` no longer duplicates its own status
  classification -- it can just call `conn.execute()` now.)

- **`OciConnection::connect()`** treated `OCI_SUCCESS_WITH_INFO` as a
  failed login. `OCILogon2` returns this when the login genuinely
  succeeded but there's a warning attached -- in practice, almost always
  `ORA-28002: the password will expire within N days`. Before the fix,
  an unattended batch job hitting that warning window would report
  "couldn't connect" for a session that actually worked. Fixed the same
  way: `status == OCI_SUCCESS || status == OCI_SUCCESS_WITH_INFO` both
  proceed. The warning text itself isn't surfaced anywhere yet (would
  need an `OCIErrorGet` call right there, and somewhere for a caller to
  read the result) -- a reasonable follow-up, not attempted here.

## The statement handle's implicit state machine

Real OCI errors hit directly in this conversation (`ORA-24324`,
`ORA-24437`, the `-2`/no-text case) all trace back to the same root
cause: a statement (or service) handle has a real lifecycle, and using it
out of sequence produces an opaque OCI error rather than a clear "you
called this too early" from the code that actually knows better.

```
Unprepared --prepare()--> Prepared --execute()--> Executed --fetch()--> Fetching --fetch()--> ... --> EndOfFetch
                              |                        |
                          bind/define              (iters>0: may go
                          only valid here          straight to EndOfFetch
                                                    if zero rows matched)
```

Concretely, from what this codebase learned building it:

- **Bind and define are only valid between `prepare()` and `execute()`.**
  Binding after execute, or defining before prepare, isn't a real OCI
  operation at all -- there's no handle state that supports it.
- **Column *names* (`OCIParamGet`/`OCIAttrGet(OCI_ATTR_NAME)`) aren't
  available until after `execute()`,** even with `iters=0` (a describe-only
  execute, no rows transferred). Describing right after `prepare()`
  consistently returned zero columns against a real database -- confirmed
  empirically while building `select_rows()`'s by-name matching (see
  "select_rows() matches columns by name now, not by position" below in
  this README). Position-only defines don't have this restriction --
  they've always been valid before execute, which is why the earlier,
  purely positional design never had to think about this ordering at all.
- **A statement that's reached `EndOfFetch` can't be fetched again** the
  way you'd expect a "past the end" iterator not to be dereferenced --
  going back to `Prepared` (for a new execute) means either a fresh
  `prepare()` or, for a cached statement, releasing and re-acquiring it
  (see `OCIStmtPrepare2`/`OCIStmtRelease` below).

A state-checking wrapper (an `OciStatement`-shaped class, or the
equivalent in any other codebase) earns its keep by converting these
ordering rules from "an opaque `ORA-#####` from OCI, sometimes with no
error text at all" into "a clear, local exception naming exactly which
operation was attempted in which wrong state, before OCI is ever asked to
do anything." Whether that's an exception or a returned error code is a
real design choice, not a formality: this codebase's own `ExecResult` is
reserved for outcomes a caller has to handle *every time* as part of
normal database use -- a bad query, a lost connection, zero rows found.
Calling `fetch()` before `execute()` isn't that kind of outcome; it's
always avoidable by getting the call sequence right, which is exactly the
class of bug C++ conventionally surfaces via `std::logic_error` and its
relatives rather than folding into the same vocabulary as a real runtime
outcome.

### `OCIStmtPrepare2` adds its own lifecycle wrinkle

Separately from the state machine above: `OCIStmtPrepare2`-obtained
handles must be released with `OCIStmtRelease` (matching the same `key`
used to prepare), not `OCIHandleFree`. Mixing the two release conventions,
or reusing the same `OCIStmt*` variable across two `Prepare2` calls
without resetting it to `NULL` first, leaves the statement cache's
bookkeeping inconsistent in a way that can surface later as `ORA-24437`
("statement handle not prepared") on a handle that looks otherwise valid
-- not from anything wrong with the *current* prepare/execute pair, but
from a lifecycle mismatch on a *previous* one. This codebase only ever
uses plain `OCIStmtPrepare` (no caching), so it doesn't hit this --
worth remembering if a cached-statement path is ever added on top of
whatever `OciStatement` design comes out of this.

## Handle ownership as a generic, reusable primitive

A natural foundation underneath any statement abstraction: a single
generic RAII guard, parameterized on the handle's C++ type and its
`OCI_HTYPE_*`/`OCI_DTYPE_*` value, rather than hand-writing
`OCIHandleAlloc`/`OCIHandleFree` pairs per handle kind:

```cpp
template <typename HandleType, ub4 HandleTypeEnum>
class OciHandleGuard {
public:
    explicit OciHandleGuard(OCIEnv* env) {
        OCIHandleAlloc(env, reinterpret_cast<void**>(&handle_), HandleTypeEnum, 0, nullptr);
    }
    ~OciHandleGuard() { if (handle_) OCIHandleFree(handle_, HandleTypeEnum); }
    OciHandleGuard(const OciHandleGuard&) = delete;
    OciHandleGuard& operator=(const OciHandleGuard&) = delete;
    HandleType* get() const noexcept { return handle_; }
private:
    HandleType* handle_ = nullptr;
};

using OCIEnvHandle     = OciHandleGuard<OCIEnv,    OCI_HTYPE_ENV>;
using OCIErrorHandle   = OciHandleGuard<OCIError,  OCI_HTYPE_ERROR>;
using OCIServerHandle  = OciHandleGuard<OCIServer, OCI_HTYPE_SERVER>;
using OCISvcCtxHandle  = OciHandleGuard<OCISvcCtx, OCI_HTYPE_SVCCTX>;
using OCISessionHandle = OciHandleGuard<OCISession,OCI_HTYPE_SESSION>;
using OCIStmtHandle    = OciHandleGuard<OCIStmt,   OCI_HTYPE_STMT>;
```

Worth noting this project's own `OciConnection` doesn't need `OCIServerHandle`/
`OCISvcCtxHandle`/`OCISessionHandle` today specifically because it uses
`OCILogon2` (one call doing server-attach + session-begin + service-context
wiring atomically -- see `connect()`'s own comment) rather than the older
four-call manual sequence (`OCIServerAttach` + `OCIHandleAlloc(SVCCTX)` +
`OCISessionBegin` + `OCIAttrSet(OCI_ATTR_SESSION)`). A codebase using the
manual sequence needs all three and, per the `ORA-24324` bug walked
through in this same conversation, needs to get the `OCI_ATTR_SESSION`
attach step right or ends up with a service handle that has a server but
no session -- "looks valid, `OCI_ATTR_SERVER` reads back fine, still not
usable" being exactly that failure mode.

An `OciStatement` class composes `OCIStmtHandle` for the handle itself and
layers the state enum and `prepare()`/`execute()`/`fetch()` methods (each
checking `state_` first, throwing on violation) on top -- the guard owns
allocation/free; the statement class owns the lifecycle rules.

## LOB: two genuinely separate concerns, already separated in this codebase

A LOB locator's lifecycle (`OCIDescriptorAlloc`, `OCILobCreateTemporary`/
`OCILobWrite2` on the way in, `OCILobGetLength2`/`OCILobRead2` on the way
out, `OCILobFreeTemporary`/`OCIDescriptorFree` either way) is a distinct
concern from the user-facing value a LOB field actually holds. This
codebase already keeps them separate, just as free functions rather than
a class: `make_temp_lob`/`free_temp_lob`/`read_lob_bytes` in
`details/oci_client.h` are the "OCI-level LOB locator manager" -- they
never appear in `oci_lob.h`'s public `OciClob`/`OciBlob` types at all,
which only ever hold a plain `std::string`/`vector<unsigned char>` and
never see an `OCILobLocator*`. A dedicated `OCILob` class wrapping that
same locator lifecycle (rather than free functions) is the same
separation, just packaged as an object with its own lifetime instead of
a set of functions threaded through by the caller -- worth doing if the
lifecycle needs to be reused somewhere this codebase's own bind/define
call sites don't already cover, not a different design, just a different
packaging of the same idea already in place here.

## OciDate: storage decision already converged on

This project's `OciDate` (`oci_datetime.h`) stores the real `::OCIDate`
struct directly as its only member -- no separate year/month/day fields,
no descriptor, no indirection. A 7-byte value with the exact layout OCI
itself defines, which is what makes it usable as a plain, fixed-stride
bindable/definable field everywhere else in this file (`OciTypeBinder<OciDate>`
needs no special-casing beyond its type-code entry, unlike `FixedString<N>`
or a LOB field). Any date wrapper that instead stores year/month/day as
separate fields would need to convert to/from `::OCIDate`'s layout on
every bind/fetch; storing `::OCIDate` itself skips that entirely.
