# Oracle MVCC (read consistency) -- working notes

Written up from a conversation about why concurrent reads shouldn't (and,
more precisely, mostly don't) interfere with the write-side benchmarks in
this directory -- see `../README.md`'s "Testing against a real database"
section for the benchmarks that prompted this. Not specific to this binding
library; this is just how Oracle works, recorded here because it came up
while reasoning about what the benchmark numbers actually meant.

## The claim being explained

Oracle's classic pitch: **readers never block writers, and writers never
block readers.** Unlike lock-based (two-phase-locking) systems where a
reader might take a shared lock that a writer has to wait behind, or a
writer's exclusive lock makes a reader wait, Oracle readers and writers
never wait on each other at all. This note is about *how* that's possible
without giving up correctness -- i.e., what "a consistent read" actually
means when the data underneath it keeps changing.

## The mechanism, step by step

**1. Every read gets a snapshot SCN.** SCN (System Change Number) is
Oracle's global, monotonically increasing logical clock -- every commit
gets the next SCN. At the *start* of a read (for an ordinary `SELECT` under
READ COMMITTED, this is the start of that one statement; under
SERIALIZABLE, or an explicit read-only transaction, it's the start of the
whole transaction instead), Oracle records the current SCN. That value is
fixed for the rest of that read's duration, however long the read actually
takes to run.

**2. Every data block tracks who's touched it.** Each block carries a
small header -- the ITL, Interested Transaction List -- recording which
transactions have modified rows in that block, whether each is committed,
and (for committed ones) at what SCN. It also points to the relevant undo
record for each change: the recipe for how to reverse that specific
modification.

**3. A reader compares its snapshot SCN against the block's history.**
When the reader visits a block: is the *current* version's owning
transaction committed, and if so, was its commit SCN at or before my
snapshot SCN? If yes, the current version is fine to use as-is. If no --
either the transaction is still open (uncommitted), or it committed *after*
my snapshot -- the reader can't use this version.

**4. Consistent-read (CR) reconstruction.** In that failing case, Oracle
takes a private copy of the block and walks backward through its version
chain, applying undo records in reverse, until it reaches a version whose
governing transaction *does* satisfy the check above (committed, at or
before the snapshot SCN). That reconstructed copy -- not the live block --
is what actually answers the read for those specific rows. This happens
per block, on demand, only for blocks that actually changed since the
snapshot was taken; blocks nobody's touched since then are used directly,
no reconstruction needed.

**5. An uncommitted change is unconditionally invisible to everyone else.**
This falls out of the same rule rather than needing a separate case: an
open transaction's changes have no commit SCN yet, so they can never
satisfy "committed at or before my snapshot SCN" for any other session's
read, regardless of timing. The reader always falls through to undo for
that row, gets the last *committed* version, and never sees the in-flight
change at all.

## What that resolves

**"What if another transaction commits in the middle of my read?"** You
never see it. Your snapshot SCN was fixed before that commit happened, so
any block that transaction touched now looks "changed since my snapshot" to
you the moment you visit it, and you reconstruct the pre-commit version via
undo exactly as in step 4. The rest of your query keeps going against the
same fixed SCN throughout -- which is exactly why a single `SELECT` can
return a result that's internally consistent (as if the whole table were
frozen at one instant) even though it took real wall-clock time to run and
the table kept changing underneath it the whole time.

**"Doesn't a row lock make this ambiguous?"** No -- because the row lock
and the visibility check above are two separate mechanisms that never
consult each other. The row lock's *only* job is writer-vs-writer
serialization: if transaction A has updated row X and hasn't committed,
and transaction B tries to update that *same* row, B waits for A's lock.
That's it. An ordinary reader never acquires, checks, or waits on that
lock at all -- it isn't part of the reader's algorithm in any way. Concrete
case: A has updated row X to `'new'` and holds the lock, uncommitted. B
starts a read with snapshot SCN=100, before A ever commits. B reads the
block, sees via the ITL that an active, uncommitted transaction owns the
current version -- that alone is enough to fail step 3's check
unconditionally -- and falls through to undo, reading `'old'`. B never
touches the lock table. The lock governs what writers can do; the ITL/SCN
comparison governs what readers see; a "locked" row isn't a third,
ambiguous state visible to readers, it's simply "uncommitted," which the
visibility rule already handles the same way it handles anything else that
fails the SCN check.

**Why a commit is never gated on readers.** This is the part that actually
makes "readers never block writers, writers never block readers" true, not
just a nice-sounding claim. A commit does not check for, wait on, or even
know about any concurrent readers -- it writes a commit record to the redo
log and flips a flag in the transaction table, unconditionally, regardless
of how many sessions are mid-read against rows that transaction touched.
All of the protective work is on the *reader's* side (the CR-reconstruction
in step 4), never a gate the writer has to pass through. The writer pays
zero cost for this; a reader pays a small, local cost, and only on the
specific blocks it collides with.

## The practical limit: `ORA-01555`

Consistent-read reconstruction depends on the needed undo record still
existing. Undo space is finite and gets reused over time (governed by
`UNDO_RETENTION` and how much undo activity is competing for that space).
If a read is slow enough, or undo turnover is fast enough, that the record
needed to roll a block back to your snapshot SCN has already been
overwritten by the time you get to it, the read fails outright with
`ORA-01555: snapshot too old` -- not silent wrong data, a hard error. This
is the actual, practical ceiling on "how far back can a read reconstruct
history," not a theoretical concern.

## Statement-level vs. transaction-level consistency

Two different snapshot-taking rules, both described above but worth being
explicit about the distinction:

- **READ COMMITTED** (Oracle's default): a new snapshot SCN is taken at the
  start of *each statement*. Two `SELECT`s in the same transaction can see
  two different snapshots if something else committed in between them.
- **SERIALIZABLE**, or an explicit read-only transaction: the snapshot SCN
  is taken once, at the start of the *transaction*, and every statement in
  that transaction sees the same fixed snapshot throughout, no matter how
  much time passes or how many commits happen elsewhere in the meantime.

## Why this came up here

The immediate question was whether concurrent `SELECT`s against a table
would affect the *write*-side benchmark numbers in this directory
(`live_oracle_insert_benchmark.cpp`,
`live_oracle_insert_saturation_benchmark.cpp`). The answer, given the
mechanism above: **no locking interaction, but not zero effect either.**
Reads and writes never block each other here -- that part of the intuition
that started this conversation was correct. But reads still compete for
the same finite physical resources a concurrent insert needs (CPU, buffer
cache and its latches, I/O bandwidth) -- so "shouldn't matter" is right
about locking and incomplete as a claim about elapsed time under real
concurrent load. That's a resource-contention effect, not an MVCC one, and
it's a separate, still-open thing to actually measure (run a competing read
workload alongside the insert benchmark and compare against the idle
numbers already in the README) -- not done in this pass.
