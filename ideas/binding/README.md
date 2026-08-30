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

## Why positional binds, not named

The first draft of the OCI code bound by name (`OCIBindByName`, deriving
`:PLACEHOLDER` from the struct's field name via `boost::pfr::names_of`).
Field-*name* reflection in `boost::pfr` depends on parsing compiler-specific
`__FUNCSIG__`/`__PRETTY_FUNCTION__` output and isn't reliably available on
MSVC, which is the actual compiler target this idea started from (see the
`flat_schema` concept in `reflect.h` and the note in `oci_client.h`).
`tuple_size`/`tuple_element_t`/`for_each_field` -- the only primitives this
directory depends on -- always iterate in declaration order on every
compiler, so binding/defining *by position* sidesteps the problem entirely.
The cost: bind placeholders in your SQL text must occur left-to-right in the
same order as the struct's fields (see the comment on `FinancialUpdate` in
`main.cpp`), not by matching names.

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

## What's deliberately not here

- Transaction/commit handling (`OCITransCommit` / `OCI_COMMIT_ON_SUCCESS`) --
  left to the caller.
- String and LOB columns in `query()`'s result rows (`static_assert`s
  against both, for the same fixed-buffer-size reason).
- Nullable LOBs (`std::optional<OciClob>`) -- LOB fields always bind/define
  as not-null for now.
- Config-file binding itself (XML/TOML -> `flat_schema` struct) -- the
  concept exists in `reflect.h`, but no parser is wired to it yet. That's
  the natural next step if this idea goes anywhere.

## Compiling

Verified end to end on this host: `g++ 15.2` / C++20, both via
`cmake -S -B` / `--build` (using `CMakeLists.txt`, pointed at
`/mnt/c/local/boost_1_91_0` through `BOOST_ROOT`) and via a direct `g++`
invocation with `-Wall -Wextra -Wpedantic -Wshadow` (clean except for one
warning inside Boost's own `core_name20_static.hpp`, unrelated to this
code). Not yet verified against MSVC -- the `flat_schema`/`struct_field_auditor`
engine in `reflect.h` has already needed two MSVC-specific workarounds (see
the comments on `struct_field_auditor` and `check_field`), so treat MSVC as
untested until it's actually been built there.
