# GPU SAWS V2: minimum reimplementation contract

This is a standalone specification of the intended V2 algorithm, extracted from
the current GPU implementation, its V2 plan, and the CPU SAWS half-splitting
rules. It describes the behavior to preserve; individual implementation paths
are not the specification. It includes no test harness, benchmark, or profiling
requirements.

**The central invariant: claiming a task transfers the right to execute it;
retiring its copy transfers the right to reuse its source storage. Those are
different events.**

## 1. Computation and workers

- One GPU, a persistent grid, one worker per 32-thread warp, one fixed-capacity
  deque per worker in global memory. Every worker can steal from every other.
- Lane 0 alone changes its deque's owner metadata. Thieves change only its
  atomic steal descriptor and epoch retirement counters.
- One task class, trivially copyable task bodies, one initially counted root.
  Further tasks come only from running tasks. There is no external injection.
- Each execution batch contains up to 32 tasks, one per valid lane. All lanes
  enter `Task::execute(ctx, body, valid)`, including invalid lanes, which must
  contribute no tasks or application effects.
- Tasks finish in finite time. They cannot wait for another task or use
  block/grid barriers. The runtime's independently progressing workers use
  warp synchronization and device-scope atomics.
- All workers that may hold a claim must be able to progress. V2 establishes
  this with an occupancy-valid cooperative launch. Initialization of queues,
  counters, flags, and the root precedes that launch on the same stream. The
  work loop requires no grid barrier.

There is one flat pool of deques: no overflow queue, remote enqueue, sibling
hierarchy, queue lock, dynamic resizing, or simultaneous multi-victim claims.

## 2. Deque model

Let `B = 32` tasks per chunk and `Q` be capacity in tasks. Positions are unsigned
logical task positions; physical indexing is modulo `Q`.

```text
tail -> closed claimed epochs -> pub_base -> current public range -> split -> private -> head
```

- `head`: next private insertion position. Append here; pop from this end.
- `split`: boundary between public and private work. `[split, head)` is private.
- `pub_base`: start of the current public epoch's original range. While open,
  `[pub_base, split)` includes its already claimed prefix.
- `tail`: oldest physical slot that cannot yet be reused. It includes space
  pinned by outgoing copies, even when those tasks are no longer owner work.

With an open epoch initially containing `N` chunks and `k` issued tickets:

```text
private tasks       = head - split
unclaimed public    = remaining(N, k) chunks
owner's queued work = private tasks + B * remaining(N, k)
physical occupancy  = head - tail
```

These last two counts are deliberately different. Retired copies in an open
epoch may still occupy storage until the owner closes and reclaims that epoch.

Only complete chunks are public. Public boundaries stay chunk-aligned; a
partial private remainder stays private. A private pop takes
`n = min(B, head - split)`, decreases `head` by `n`, and assigns the resulting
range to valid lanes. All lanes finish loading bodies before a spawn can reuse
the popped slots. Execution then uses those loaded bodies.

Interpret position differences with bounded unsigned modular arithmetic, not
ordinary `<` comparisons between free-running positions. Keep capacity below
half the position-number range. Every physical read/write must handle ring
wrap, including both ends of a stolen transfer.

## 3. Exact half-steal allocation

All arithmetic here is in **chunks**. Ticket indices start at zero. Each valid
ticket takes `floor(remaining / 2)`, except that a final single chunk is taken
whole. Tickets consume an oldest-first contiguous prefix of the original epoch.

Examples: `5 -> shares 2, 1, 1, 1`; `8 -> shares 4, 2, 1, 1`.

The exact constant-time form is:

```text
remaining(N, k):
    if N == 0: return 0
    h = bit_width(N - 1)           // bit_width(0) = 0
    if k > h: return 0
    return 1 + ((N - 1) >> k)

claim(N, k):
    before = remaining(N, k)
    if before == 0: return {first: N, count: 0}
    after = remaining(N, k + 1)
    return {first: N - before, count: before - after}

claimed_prefix(N, k) = N - remaining(N, k)
```

For `N > 0`, exactly `bit_width(N - 1) + 1` tickets have nonempty shares.
Exhausted tickets are empty; guard them before shifts or increment arithmetic
that could overflow. Counts and offsets determine unique, disjoint ranges
covering the epoch. Do not interpret ticket count as chunk count.

## 4. Atomic claim, close, and epoch lifetime

The atomic descriptor contains `{attempts, closed, epoch_slot, initial_chunks,
base_chunk}`. All fields are observed together in one atomic word.

**Claim:** lane 0 issues one device-scope acquire-release fetch-add to the
attempt field. The returned pre-increment word supplies the ticket index and
all descriptor fields. A closed word or empty share is a failed attempt and
needs no retirement. A previous load cannot authorize a claim. V2 has no
mandatory victim discovery load or claim CAS retry loop.

**Close:** the owner atomically sets `closed` with acquire-release fetch-or.
From its returned pre-close word, calculate
`T = claimed_prefix(initial_chunks, attempts)`. The atomic order decides races:
claims before close own their shares; claims observing closed own nothing.
The claimed prefix is `[pub_base, pub_base + T * B)`.

If `T > 0`, retain a closed epoch record containing:

```text
claimed_chunks = T
end_position   = pub_base + T * B
retired_chunks = existing counter for this epoch
```

Advance `pub_base` past that prefix and queue the record for reclamation. The
remaining `[pub_base, split)` is unclaimed owner work. A zero-claim close needs
no pending record. **Do not reset retirement at close:** copies may have
retired before close.

**Publish:** obtain an epoch slot with no live references; initialize its
retirement counter to zero; publish the descriptor with a release store and
attempt count zero. The descriptor describes fully written, counted tasks.
Reopening an unclaimed remainder starts a new half-steal schedule over that
remainder. Closed records keep their original retirement counters.

**Retire:** after every lane has finished all source reads and destination
writes, synchronize the warp, then lane 0 adds the ticket's chunk count to the
source epoch's counter with a release atomic. Retire each valid ticket exactly
once. Retirement counts chunks copied, not tickets or tasks executed.

**Reclaim:** process closed epochs oldest first. An acquire load must observe
`retired_chunks == claimed_chunks` before advancing `tail` to that record's
end and freeing its epoch slot. Never skip an incomplete oldest record to free
a later one. With no closed records, `tail` may advance to `pub_base`.

Keep room for the current epoch and any replacement epoch as well as pending
closed records. If bookkeeping is temporarily full, return retry and progress
reclamation. An epoch slot or body range cannot be reused while a live ticket
can still refer to it.

## 5. Owner half-release and half-acquire

**Release:** let `F = floor(private_tasks / B)`. If `F == 0`, or the current
public epoch has unclaimed chunks, do nothing. Otherwise, obtain safe epoch
space, close the exhausted epoch, move `split` forward by
`ceil(F / 2) * B`, and publish those oldest private chunks. This can expose the
only complete chunk. The leftover partial chunk remains private.

Attempt release after a completed execution batch, after a received transfer,
and after each fully written and counted spawn segment. Never publish while
an advanced `head` still covers unwritten reserved bodies.

**Acquire:** only when private work is empty, close the current public epoch
and preserve its claimed prefix. Let `R` be its unclaimed remainder in chunks.
Move `split` backward by `ceil(R / 2) * B`, making the newest half private.
Republish the older `floor(R / 2)` chunks, or leave the word closed if zero.
Acquisition moves ownership boundaries; it copies no bodies.

**Roll under storage pressure:** close an open epoch and republish its
unclaimed remainder without moving `split`. This permits reclamation of its
completed claimed prefix. Rolling is distinct from acquisition. Obtain safe
epoch space first and reclaim after closing as needed to make storage reusable.

Queue results must distinguish available private work, no owner-accessible
work, and retry due to epoch/storage pinning. Retry does not establish emptiness.

## 6. One steal transaction

First exhaust private work and try owner acquisition. If that is temporarily
blocked, progress/retry it before stealing. Pick one random global victim
excluding self, with no same-block preference. With one worker, skip stealing.

For uniform deque capacity `C = Q / B` chunks, no ticket exceeds
`M = max(1, floor(C / 2))` chunks. **Before the remote claim**, guarantee
`M * B` physically free local slots, reclaiming/rolling and waiting for already
claimed outgoing copies if needed. Do not consume that space with an intervening
owner push. Only this owner can allocate it, so this suffices as a reservation.

```text
lane 0: direct atomic claim on the chosen victim
warp: synchronize and broadcast the entire ticket
if invalid: finish this attempt
lane 0: reserve exactly ticket.count * B destination slots
warp: synchronize and broadcast the destination range
warp: copy the entire ticket, preserving oldest-to-newest body order
warp: synchronize after all reads and writes
lane 0: retire the victim's ticket
lane 0: attempt normal half-release of received work
```

Complete copying and retirement before executing tasks, issuing another claim,
or waiting for space. A valid claim is never truncated or abandoned in a
successful run. The recipient later consumes from its newest end. Transfers
neither spawn nor complete tasks and change neither conservation counter.

## 7. Collective spawn and capacity

Preserve these task-facing semantics if keeping the existing API:

- `ctx.spawn(body, pred)`: compact true predicates in lane order into one
  private reservation, one child per true lane.
- `ctx.spawn_n(count, parent, make)`: concatenate each lane's child sequence in
  lane order. Prefix sums identify each child's parent lane and index. Redistribute
  construction across lanes; `make(parent, i)` may run on a different lane from
  its parent. Transfer the parent value; `make` cannot use warp collectives.
- Every lane calls spawn and other warp collectives in the same sequence;
  lanes with no children supply false/zero. `ctx.lane()` is lane index,
  `ctx.warp()` is global worker ID, and `ctx.warp_max()` is collective.

Each spawn segment follows:

```text
reserve private slots -> broadcast status AND range -> write all bodies
-> synchronize warp -> release-update spawned count -> attempt half-release
```

Large `spawn_n` calls can commit multiple segments. A reservation requesting up
to `want` slots has a minimum acceptable size `min`; decide capacity against
`min`, then grant up to the physical free space.

After applicable reclaim/roll progress:

- If owner queued work plus `min` exceeds `Q`, fail explicitly for capacity.
- If it fits but physical occupancy plus `min` exceeds `Q`, retry for pinned
  outgoing copies, with progress, backoff, and stop/error checks.
- Otherwise reserve and commit the bodies before exposing them.

Do not wait indefinitely for hypothetical future thieves to cure true capacity
exhaustion. Do not spill, drop tasks, or execute children recursively to make
room. Pending execution bodies are already loaded outside the deque; their
slots need not stay occupied while their callback runs.

## 8. Ordering and uniform control

All cross-worker synchronization uses device-scope atomics. Owner-only metadata
uses ordinary accesses. The necessary visibility chains are:

```text
spawn body writes on all lanes
  -> warp join -> spawned release store -> descriptor release store
  -> thief acquire claim -> warp join/broadcast -> source reads

source reads and destination writes on all thief lanes
  -> warp join -> release retirement increment
  -> owner's acquire retirement load -> source/epoch reuse

task callbacks and their child commits on all lanes
  -> warp join -> executed release store
  -> checker's acquire reads
```

A lane-0 atomic alone does not join the other lanes' memory operations. At
every collective decision, broadcast the complete result before branching:
reservations, tickets, acquire status, done, and error. All lanes take the same
stop/retry path even if flags change concurrently. Warp-local validity permits
different task bodies, not divergent participation in collectives.

## 9. Conservation and termination

Maintain monotonic 64-bit `spawned[w]` and `executed[w]` counters. Only worker
`w`'s lane 0 writes them, using release stores. Count the root once before
execution. Count children after their bodies are committed and before they
can execute. Count a batch completed only after all callbacks finish and all
their children have been counted.

Worker 0 lane 0 periodically checks termination, including while it has work.
Other workers only poll flags. The checker performs **two complete passes in
this order**:

```text
e = sum(acquire_load(executed[w])) over all workers
s = sum(acquire_load(spawned[w]))  over all workers
if no error and e == s:
    release_store(done, true)
```

Do not interleave completion/spawn reads per worker or reverse the passes.
Detect overflow in individual counter increments and both sums, or enforce
an equivalent explicit bound. Counters cannot wrap.

Why equality is sufficient: the root is already counted; observing a completion
also observes the birth of that task and every child it created through the
publication/claim/completion ordering chain. The later spawn pass includes those
births. Starting from the root, equality leaves no uncompleted descendant
frontier. Queued, executing, and in-transit tasks remain spawned but uncompleted.
An unsuccessful steal alone says nothing about global termination.

## 10. Bounded failure and representation choices

Poll done/error uniformly in scheduler, spawn, space, epoch, and idle retries.
Any capacity or counter error fails the whole run; error takes precedence over
done. Elect one error writer atomically if retaining shared diagnostic fields.
Use strong CAS or retry spurious failures of weak CAS. The host can read the
winner's payload after kernel completion. An aborted run may discard outstanding
tickets; it cannot report successful completion.

Direct attempts against closed/exhausted words still increment the attempt
field. Prevent its wrap. The present scheme guards when the returned count plus
one reaches `G = 2^31`, stops further attempts from that worker, and fails the
run. With at most one outstanding attempt per worker and a flag check before
each attempt, require `G + worker_count < 2^32` to cover attempts already in
flight. Any replacement must prevent duplicate ticket indices within an epoch.

The current encoding is a useful implementation choice, not a mathematical
requirement: bits `63..32` attempts, bit `31` closed, bits `30..27` epoch slot,
bits `26..14` initial chunks, bits `13..0` base chunk modulo `2^14`.
If retaining it, initial chunks must fit 13 bits, epoch slots fit 4 bits, and
truncated bases must reconstruct the same physical slot. The existing masked
ring uses power-of-two chunk capacity no greater than 8191 (therefore at most
4096), and a power-of-two epoch count between 2 and 16. Check storage-size
arithmetic too. Power-of-two masking is optional if the representation changes.

Block size, deque depth, epoch count, PRNG implementation, sleep durations,
termination-check interval, body-copy vector width, file layout, and instrumentation
are implementation/tuning choices. Preserve the ownership, allocation, lifetime,
ordering, and conservation rules above when changing them.

## Extraction provenance

Inspected on 30 September 2026: working tree based on commit
`b79707f66eea0acbae805dbae5ecfc9f965c6a43`, including current local edits.
Primary sources: `libgputc/steal_word.hpp`, `saws_ring.hpp`, `epoch.hpp`,
`scheduler.cuh`, `warp.cuh`, `context.cuh`, `termination.cuh`, `kernel.cuh`,
`run.cuh`, `run_control.hpp`; `GPU-SAWS-V2-Implementation-Plan.md`; and
`libtc/saws_shrb.c`. This document is sufficient without those files.

The intended contract resolves existing implementation shortcuts: local acquire
retry must not fall through to remote stealing; committed segments of a long
spawn call should release work; individual conservation increments need overflow
protection; and error election must not silently lose a spurious weak-CAS failure.
