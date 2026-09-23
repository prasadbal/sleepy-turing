# Windows growable memory-mapped file: two designs

Two implementations of "write to a memory-mapped file that grows as you go,
truncate to the real size on close," for the CSV/report-writer sink
discussed alongside `ideas/aggregate` and `core/posreport`. Windows-only
(`windows.h`), not wired into the project's (Linux-first) CMake build --
build and run each with its own `.bat`-equivalent command below. Not yet
built against a real MSVC target inside this repo's own CMake; verified
standalone (GCC/Clang don't apply here -- this is MSVC/Windows API code).

## `segmented_mmap.h` -- sliding window (the one to use for a write-only,
## ever-growing file)

At most **one** segment is ever mapped at a time. The initial view is size
N; once a write would exceed it, the current segment is unmapped, the file
is extended, and a new segment is mapped starting at the new offset --
**not** remapped from offset 0 with the whole capacity. Since this process
never reads a segment again once it's been written, unmapping it as soon as
the write cursor moves past frees the memory immediately instead of holding
every byte ever written resident for the life of the writer.

Verified, not assumed:
- A segment is genuinely unmapped the instant it's slid past -- proven by a
  `debug_on_unmap` hook that checks write-access at that exact moment. (A
  naive "does this pointer still trap" check *after* the fact is unreliable:
  Windows immediately reuses a freed virtual address for the very next
  mapping, observed directly while building this -- so segment N+1 can
  legitimately land at the same address segment N had.)
- Data written into a segment, then unmapped with **no explicit flush**, is
  still correctly on disk after `close()`, read back with a completely fresh
  file handle.
- **Memory actually stays bounded**: writing 200 segments (12,800 KiB total,
  64 KiB granularity) grew the process working set by 168 KiB, not 12,800 --
  measured via `GetProcessMemoryInfo`, not inferred.
- Extending the file's length never happens while anything is mapped at all
  (unmap always precedes the resize), so this makes no assumption about
  whether Windows allows resizing while some *other*, non-overlapping region
  is still mapped.

```
cl /std:c++latest /O2 /EHa /W4 segmented_mmap_test.cpp && segmented_mmap_test.exe
```
(`/EHa` -- not `/EHsc` -- is required: the test uses `__try`/`__except` (SEH)
to prove a segment traps on write once unmapped.)

## `growable_mmap_win.h` -- single view, remap-from-offset-0 (superseded for
## this use case, kept for reference)

The earlier design: one view, covering `[0, capacity)`. Growing means unmap
the whole thing, extend the file, and remap the **entire** new capacity from
offset 0 again. Simpler (`ensure_capacity(pos, len)` hands back one stable
`base()` pointer valid across the whole mapped region, and the caller does
its own `memcpy` -- no split-write handling needed), but every grow's cost
scales with the *total* capacity so far, not just the increment, and every
byte ever written stays resident until `close()`. Fine for a bounded or
randomly-accessed file; wrong for an unboundedly growing write-only stream,
which is why `segmented_mmap.h` exists.

Also includes the finding from comparing this against
`boost::interprocess::file_mapping`/`mapped_region`: wrapping Boost's
primitives here would have been *more* fragile, not safer --
`file_mapping`'s Windows backend opens its internal file handle with
`dwShareMode = 0` (exclusive, hardcoded), so growing through it means
closing and reopening the *file itself* on every grow, not just the mapping
object and view the way this hand-rolled version does.

```
cl /std:c++latest /O2 /EHsc /W4 test_growable_mmap.cpp && test_growable_mmap.exe
```

## Open items

- Neither design has a Linux counterpart in this repo yet (the earlier
  session discussion sketched one on `mmap`/`mremap`/`ftruncate`, but it was
  never built or tested -- only the Windows side has real code and tests).
- Not integrated with `core/posreport` or a CSV row-writer yet; that's the
  next step once a sink interface (template `Sink` vs `std::ostream`,
  discussed earlier -- templates won, for the zero-virtual-dispatch reason)
  is decided.
- `segmented_mmap.h`'s `write()` decides segment size as
  `max(view_size, remaining)` rounded up to a `view_size` multiple when a
  single call needs more than one segment's worth at once -- untested at
  very large single-write sizes (multi-GiB in one `write()` call).
