# GPU SAWS V2: implementation handoff

## 1. Assignment and scope

Implement a substantially simpler, single-GPU CUDA SAWS runtime with multiple persistent blocks, one worker per warp, one global-memory deque per worker, and ordinary geometric half-stealing from one randomly selected victim at a time. Preserve the current task API and UTS application as far as practical.

The agreed architecture is:

- Many worker warps across multiple blocks.
- All task deques and remotely accessed queue metadata in device global memory.
- A flat collection of worker deques; every worker can steal from every other worker.
- Classic SAWS owner release/acquire behavior, adapted to whole 32-task chunks.
- One direct atomic discovery-and-claim operation per steal attempt, followed by copying and retirement on success.
- CPU-style half-stealing only. Other fractions and simultaneous claims against several victims belong to V2.1.

Implement the runtime, tests, benchmark updates, and documentation. This is not a request for a design-only patch. Work in the existing repository, read applicable AGENTS.md instructions, and preserve unrelated changes. Do not push or publish changes unless separately instructed.

### Baseline inspected

Repository: [https://github.com/brianlarkins/saws](https://github.com/brianlarkins/saws)

Branch: `gpu_saws`

Inspected commit: `61d6d2ae9518273ce360f4bccec7bba26de4fee2` (`Fixes and optimizations`). This was also the remote branch head when this plan was prepared. If the implementing checkout is newer, inspect the changes rather than resetting it to this commit.

Primary implementation: `libgputc/`. Application: `examples/gpu-uts/`. CPU reference: `libtc/saws_shrb.c`.

The repository's `design_ideas.pdf` describes earlier, broader goals. This handoff controls V2 scope. In particular, do not implement NVSHMEM, multi-GPU execution, task-class bins, or a deque hierarchy.

### Simplification requirements

Delete the block overflow ring, its control structure and lock, spill/recovery paths, sibling-specific scheduling, shared-memory deque allocation, and their obsolete configuration/statistics. Preserve V1 through git history, not through a second maintained scheduler in the source tree.

Do not add a generic scheduler framework, runtime-selectable geometry, multi-victim reservation machinery, remote enqueue, dynamic queue resizing, a global ready queue, or new task execution modes. Keep the existing host/device atomic scope abstraction only where it enables meaningful host protocol tests. A few named half-steal arithmetic functions are enough of an extension point for V2.1.

## 2. Existing behavior that must change


| Current V1 behavior                              | Required V2 behavior                                              |
| ------------------------------------------------ | ----------------------------------------------------------------- |
| One persistent block                             | Cooperative persistent grid with a validated launch size          |
| Shared-memory deque for each warp                | Global-memory deque for each warp                                 |
| Global ring shared under a lock                  | No overflow ring and no queue mutex                               |
| Probe sibling words, choose one, claim one chunk | Choose one global victim, atomically obtain a half-steal ticket   |
| High steal-word counter counts chunks claimed    | High counter counts steal attempts/tickets issued                 |
| Claimed prefix is `min(counter, avail)`          | Claimed prefix is computed from the half-steal schedule           |
| `reacquire()` takes all unclaimed public work    | Acquire takes approximately half and republishes the rest         |
| More warps imply shallower shared-memory deques  | Per-worker capacity is independent of block size and worker count |
| Shared-memory termination counters               | Device-wide per-worker counters and one rate-limited checker      |


Retain the separation of ownership claim, task copy, and retirement. Those stages remain necessary even after the memory hierarchy is removed.

## 3. Execution model, API, and configuration



### Workers and tasks

Use one-dimensional blocks with a multiple of 32 threads:

```cpp
lane = threadIdx.x & 31;
warp_in_block = threadIdx.x / 32;
worker_id = blockIdx.x * (blockDim.x / 32) + warp_in_block;
worker_count = gridDim.x * (blockDim.x / 32);
```

Only lane 0 mutates its worker's owner state. All 32 lanes participate in task execution, body movement, and warp collectives. Keep one task per active lane and the existing validity mask for partial batches. Task bodies remain trivially copyable with alignment at most 16 bytes. A run has one task class and begins with one root task.

Keep `Task::execute`, `Ctx::spawn`, `Ctx::spawn_n`, `lane`, and `warp_max`. Define `Ctx::warp()` to return the global worker ID and document that distinction from the block-local warp index. Preserve the existing redistribution of child construction in `spawn_n`; do not redesign UTS generation while changing the scheduler. Callbacks must finish in finite time and must not wait for another task's result or use block-wide barriers.

### Configuration

Keep configuration small. Suggested initial defaults are implementation choices, not performance claims:


| Setting                    | Initial value           | Contract                                                  |
| -------------------------- | ----------------------- | --------------------------------------------------------- |
| `chunk`                    | 32                      | Fixed execution/steal allocation unit                     |
| `warps_per_block`          | 8                       | Independent of deque capacity                             |
| `deque_chunks`             | 256                     | Fixed capacity per worker; tune using peak occupancy      |
| `epochs`                   | 8                       | Power of two, at least 2, fitting the epoch field         |
| `timing`                   | false                   | Compile-time opt-in for detailed phase timing             |
| `blocks`                   | 0 = automatic           | Runtime launch option; explicit positive values validated |
| termination check interval | 64 scheduler iterations | Used only by worker 0; configurable as a named constant   |


For a 32-byte UTS body, 256 chunks is 256 KiB per worker. Calculate total allocation as `worker_count * deque_chunks * 32 * sizeof(Body)` using checked `size_t` arithmetic, and report it. More global memory makes deeper queues possible; it does not make allocation size irrelevant.

Keep capacity as a compile-time configuration field initially, with a Makefile override such as `QUEUE_CHUNKS`. There is no need for runtime resizing or a large policy registry. Preserve `run<Task, Cfg>(root)` and existing policy calls using defaults; add a small launch-options argument for explicit block count rather than mixing it into deque operations.

### Launch and initialization

Use `cudaLaunchCooperativeKernel` for V2. Query cooperative-launch support and calculate the maximum grid size for the actual compiled kernel and block size using the occupancy API and device SM count. Auto mode uses a legal grid size; explicit oversize requests must fail with a useful message. Do not assume that one block maps permanently to each SM.

Initialize all queue states, epoch counters, statistics, run flags, and task counters in a separate initialization kernel on the same stream. Place the root in worker 0's private queue and initialize that worker's spawned count to one. Start the persistent kernel only after this initialization is ordered before it. Remove the old in-worker root seeding so the root is neither enqueued nor counted twice.

The persistent work loop needs no grid barrier and no block barrier. Its synchronization is warp-local plus device-scope atomics. Cooperative launch is used for grid-wide progress, not to impose lockstep scheduling. Unsupported cooperative launch should be reported explicitly; do not silently introduce a second scheduling model.

## 4. Queue storage and invariants

Allocate one contiguous body slab and one array of queue states. Each worker owns `deque_chunks * 32` body slots. A non-owning queue view holds pointers to that worker's state and slice of the slab. Other workers construct a view of a victim when stealing. Small helper structs are appropriate; inheritance hierarchies and dynamically allocated queue objects are not.

Retain the current ring organization and epoch FIFO where useful. Positions are unsigned free-running task counters; physical indexing masks by capacity. Required order, interpreted through bounded modular distances, is:

`tail -> closed claimed epochs -> pub_base -> current public epoch -> split -> private tasks -> head`


| Quantity                                   | Meaning                                                                     |
| ------------------------------------------ | --------------------------------------------------------------------------- |
| `head - tail`                              | Physical slots occupied or still pinned by copies                           |
| `head - split`                             | Owner-private tasks                                                         |
| `[pub_base, split)`                        | Current public epoch's original range, including any already claimed prefix |
| `half_remaining(initial_chunks, attempts)` | Currently unclaimed public chunks                                           |
| `tail`                                     | Oldest slot not yet reusable                                                |


Important invariants:

1. Exactly one owner modifies `head`, `split`, `pub_base`, `tail`, and the epoch FIFO indices.
2. Thieves modify only the steal word and their ticket's retirement counter.
3. A published range contains fully written task bodies whose spawn accounting is already visible.
4. Every successful ticket owns a unique contiguous range of full chunks, possibly wrapping physically.
5. A ticket is retired only after every lane has finished reading its source bodies.
6. Storage and epoch slots are not reused until all valid claims covered by them retire.
7. An incoming steal is not a spawn and does not change task conservation counters.
8. Only full chunks are published. A partial private tail remains owner work.
9. Unsigned position wrap is supported; do not compare free-running positions with ordinary less-than ordering. Keep capacity well below half the position-number range.

Start with owner metadata stored in global memory and small transient values on lane 0. Do not replicate a queue, epoch array, or all-worker state inside each lane's `Worker` object. Use pointers/views. Do not introduce shared-memory task staging in V2.

## 5. Exact half-steal arithmetic

All quantities in this section are in chunks, not tasks. Preserve the CPU rule: take `floor(remaining / 2)`, except that a final remaining chunk is taken as one.

Expected allocation examples:


| Initial chunks | Successive valid shares      |
| -------------- | ---------------------------- |
| 0              | none                         |
| 1              | 1                            |
| 2              | 1, 1                         |
| 3              | 1, 1, 1                      |
| 5              | 2, 1, 1, 1                   |
| 8              | 4, 2, 1, 1                   |
| 150            | 75, 37, 19, 9, 5, 2, 1, 1, 1 |


Implement pure host/device helpers:

```cpp
uint32_t half_remaining(uint32_t initial, uint32_t tickets);
ChunkRange half_claim(uint32_t initial, uint32_t ticket);
uint32_t half_claimed_prefix(uint32_t initial, uint32_t tickets);
```

There is an exact constant-time formula for this particular policy. For `N > 0`, define `h = bit_width(N - 1)`, with `bit_width(0) = 0`. Then:

```text
remaining(N, k):
    if N == 0: return 0
    h = bit_width(N - 1)
    if k > h: return 0
    return 1 + ((N - 1) >> k)

claim(N, k):
    before = remaining(N, k)
    if before == 0: return { first = N, count = 0 }
    after  = remaining(N, k + 1)
    return { first = N - before, count = before - after }

claimed_prefix(N, k):
    return N - remaining(N, k)
```

The valid ticket count is `h + 1`. Guard exhausted tickets before any increment that could overflow `k`; a ticket at or beyond the valid-count bound has an empty share. With the retained 13-bit initial-count field, legitimate shifts are at most 13. Do not use floating-point logarithms or powers. Provide a C++17-compatible host/device bit-width helper and handle zero before any count-leading-zeros intrinsic that would be undefined on zero.

The formula was checked while preparing this plan against the iterative CPU rule for every initial count 0 through 8191, including empty and exhausted tickets. The implementation must still add its own regression tests. Keep the iterative rule as an independent test oracle, not as duplicated production logic.

This replaces a potentially long arithmetic replay without introducing general-geometry machinery. V2.1 can change these helpers later.

## 6. Packed word, claims, and overflow protection

The current 64-bit layout can be retained:


| Bits   | Field                                                |
| ------ | ---------------------------------------------------- |
| 63..32 | Attempted steals / issued tickets                    |
| 31     | Closed flag                                          |
| 30..27 | Epoch slot                                           |
| 26..14 | Initial public chunks                                |
| 13..0  | Epoch base in chunk positions, modulo the base field |


Rename fields/functions that currently describe the high field as chunks claimed. Rename `avail` to `initial_chunks` if doing so reduces ambiguity. Keep capacity and base-span checks: capacities must fit the packed fields and modulo indexing. Do not silently enlarge queues beyond the representation.

A steal attempt issues one device-scope acq_rel fetch-add of `1ull << 32`. The returned word, not any previous read, determines success, epoch, base, and ticket range. No mandatory remote pre-probe and no CAS retry loop belong in the normal claim path. A closed word or zero-size share is a failed attempt and requires no retirement.

For an owner close, use the word returned by an acq_rel fetch-or of the closed bit. The correct count is `half_claimed_prefix(initial_chunks, attempts_before_close)`. Remove the current `min(attempts, initial_chunks)` calculation and the fixed-chunk `first = attempts` interpretation.

### Bounded research-runtime overflow behavior

Unconditional attempts can eventually wrap the 32-bit ticket field. V1's probe-before-claim argument no longer applies. For the simplest V2, use an explicit fail-fast guard rather than a per-worker/per-victim damping table:

- Set a guard threshold comfortably below wrap, for example `1u << 31`.
- Inspect the returned attempt field on every claim, including claims against closed or exhausted words.
- If it is at or above the threshold, mark the whole run failed and issue no more claims from that worker.
- Every worker checks the global error flag between attempts and in retry/wait loops.
- Validate a worker-count bound such that `threshold + worker_count < 2^32`. With one outstanding claim per worker, this leaves room for all claims already in flight before the guard is acted upon.

Test this with counters injected near the threshold. A guard failure is not successful termination. Original SAWS-style empty-target damping can be evaluated later; it is not necessary to build a victim-state cache or add remote probes to every V2 attempt.

## 7. Owner operations and epoch lifecycle

Use one coherent implementation of close/publish/reclaim. Keep epoch retirement in chunk units. Do not clear an epoch's retirement counter at close: valid thieves may have retired before the owner closes it.

### Release half

At a safe point after task bodies and spawn accounting have been committed, let `F = private_tasks / 32`.

- If `F == 0`, there is nothing publishable.
- If the currently published epoch still has unclaimed chunks, leave its split unchanged.
- Otherwise, ensure an epoch slot is available, close the old epoch if present, and publish `ceil(F / 2)` oldest private chunks by moving `split` forward.
- Publish the new descriptor only after initializing its retirement counter and metadata.

Use the same rounding throughout: the owner exposes approximately half, rounded upward in full chunks. The one-full-chunk case may expose that chunk, as in the CPU rule. Do not add a release-percentage tuning parameter in V2.

Call release after each completed execution batch, after receiving a steal batch, and after a completely written/counted spawn reservation. This makes generated work available during long `spawn_n` calls without exposing unwritten reservations. Never call it halfway through filling an advanced `head` range.

### Acquire half

Only when private work is empty:

1. Ensure the epoch bookkeeping can close and, if necessary, publish another epoch.
2. Atomically close the current public epoch.
3. Calculate its claimed prefix from the returned word and preserve those in-flight claims in the epoch FIFO.
4. Let `R` be the unclaimed remainder. Reacquire `ceil(R / 2)` newest chunks by moving `split` backward.
5. Publish the older `floor(R / 2)` chunks as a new epoch, or leave the word closed if none remain.

Acquisition changes ownership boundaries; it does not copy bodies. Taking all public work, as V1 does, is not an acceptable implementation of this operation.

### Reclamation and physical pressure

Reclaim closed epochs oldest first, and only when `retired_chunks == claimed_chunks`. A later completed epoch cannot release slots ahead of an earlier incomplete one. Never reuse an epoch slot still referenced by a live ticket.

A published epoch's claimed prefix may already have completed copies but remain unreclaimable until that epoch closes. Retain a small `progress`/roll operation that can close and republish the same unclaimed remainder under physical-space pressure. It changes neither the private/public ownership split nor the half-steal policy. Do not confuse it with owner acquisition.

If no safe epoch slot is available, return an explicit retry status and let the warp poll progress/back off. Do not overwrite metadata and do not bury an unbounded tight loop inside queue arithmetic. The cooperative grid and nonblocking copy path let in-flight thieves finish.

## 8. A single steal transaction

V2 uses one victim and one claim at a time. Lane 0 chooses uniformly among global worker IDs other than itself, using a worker-specific PRNG state. No same-block preference, all-worker scan, or victim list. Handle the one-worker configuration without attempting a steal. A simple exclusion mapping over `[0, worker_count - 2]` avoids retrying self-selection.

Before stealing, exhaust/reacquire all owner-accessible work. Distinguish an empty queue from a queue operation blocked waiting for an epoch slot; a retry is not proof that the worker has no own work.

### Capacity without a general reservation framework

All worker queues have the same capacity `C` chunks. No ticket can contain more than `max(1, floor(C / 2))` chunks, because no published epoch can exceed `C`. Use that conservative bound even though normal release often publishes less.

Before issuing a remote claim, reclaim/roll the local queue as necessary and require that much physical destination space. If old outgoing copies pin the space, wait and retry before claiming elsewhere. Count pinned slots as occupied.

Only this warp's lane 0 can consume its private destination space. Therefore a successful capacity precheck, followed by no intervening owner push, is a capacity reservation for this single synchronous transaction. After a valid claim returns, reserve exactly its returned size locally. No other actor can steal that free capacity in between. Document this invariant instead of building a generic speculative reservation-and-rollback API.

### Copy and retirement order

```text
lane 0: choose victim and issue atomic claim
warp: synchronize and broadcast the returned ticket
if invalid: return failed

lane 0: reserve the exact destination range (capacity already guaranteed)
warp: synchronize and broadcast destination position
warp: copy every chunk, handling source and destination wrap
warp: synchronize after all source reads and destination writes
lane 0: add ticket.count to the victim epoch's retirement counter (release)
lane 0: make any desired surplus available through normal release
return success
```

A successful claim is never truncated. Complete copying and retirement before the worker executes a task, starts another claim, or enters a space wait. Keep only the current body/chunk and small ticket metadata in registers. Copy directly from victim global storage to the local global deque with bounded warp-cooperative loops; do not stage the whole stolen batch in registers or shared memory.

Preserve body order during transfer: older victim chunks precede newer chunks in the recipient's private range. The recipient consumes from its newest end. Retire the source before publishing received work in V2; this gives a simple ownership and progress argument.

## 9. Spawn capacity and explicit run failures

Retain the existing warp-collective spawn API and its task-body generation. Successful spawn reservations follow this order: reserve private slots, write all bodies, warp synchronization, increment the owner's spawned count, then optional release. Only afterward may the slots be visible to thieves.

Remove `make_room()` and spilling. A reserve operation may perform reclamation and roll the published epoch to free completed claims. It must distinguish:

- Space that can become available when already claimed copies retire: retry with progress/backoff and error checks.
- A genuine capacity limit from still-owned work after applicable progress: record `QUEUE_CAPACITY_EXCEEDED` and fail the run explicitly. Do not depend on future thieves to resolve an otherwise full queue indefinitely.

Choose sufficiently large queues for normal UTS validation. Queue overflow in a stress test is a supported failure outcome, not a reason to add another queue or recursively execute tasks inside `spawn_n`.

For an up-to-size reservation, apply the capacity decision to the minimum acceptable number of slots, not the entire requested maximum. After appropriate progress, still-owned work consists of private tasks plus unclaimed public chunks. If that work plus the minimum new reservation exceeds capacity, waiting only for already claimed copies cannot solve it. Otherwise, wait only for the pinned copies that prevent the reservation, with retry/error handling as above.

Use a small global run-control object with separate `done` and `error_code` states. The first failure records worker ID and relevant requested/available counts. All workers eventually exit on error; host code reads the details after kernel completion and reports a nonzero/failed result. Waiting for CUDA completion makes the error payload available even if other workers observed the error code earlier.

Check failure in allocation retries, epoch waits, incoming-space waits, spawn loops, and idle loops. Do not use lane-local early returns around collectives; abort decisions must be made uniformly within the warp. An aborted run may leave tickets unretired because the whole computation is being discarded; no such run may be reported as valid throughput or successful conservation.

## 10. Memory ordering and termination



### Queue publication and reuse

Use device-scope atomics throughout remotely accessed state. Keep the current explicit warp joins; an atomic executed by lane 0 does not by itself join payload operations performed by other lanes.


| Transition                                                                   | Required ordering                                                                   |
| ---------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| All lanes finish spawned-body writes -> spawned counter -> public descriptor | Warp synchronization, spawned release store, descriptor release store               |
| Thief receives descriptor/ticket -> other lanes read source bodies           | Claim has acquire semantics, followed by warp synchronization/broadcast             |
| All thief lanes finish source reads -> victim may overwrite                  | Warp synchronization, release retirement increment, owner's acquire retirement load |
| Task callback completes on all lanes -> completed count                      | Warp synchronization, executed release store                                        |
| Checker reads counters -> publishes termination                              | Acquire counter reads in the specified order, release store to done                 |


Owner-only fields can use ordinary accesses. Steal words, retirement counters, termination counters, and run-control flags must use consistent atomics whenever another worker may concurrently access them. Do not mix block-scope and device-scope operations on a cross-block synchronization object. Do not substitute `volatile` for a publication protocol.

### Conservation counters

Retain two monotonic 64-bit counters per worker: spawned and executed. Each is written only by its owning lane 0 with device-scope atomic stores. Initialize the root's spawn before the persistent launch. Moving tasks across queues never changes these counters.

Worker 0 performs a rate-limited termination check. It remains a normal worker. All other workers poll the done/error flags without scanning all queues. Check on the designated interval even when worker 0 is making progress, and on its throttled idle path. Add overflow protection for any arithmetic whose wrap could create a false equality; UTS totals comfortably fit 64 bits, but the implementation must state the bound.

The check is deliberately two complete passes:

```text
e = sum(acquire_load(executed[w])) for every worker w
s = sum(acquire_load(spawned[w]))  for every worker w
if no error and e == s:
    release_store(done, true)
```

Do not interleave `executed[w]` and `spawned[w]` reads per worker. Keep the first implementation's order simple rather than introducing a distributed reduction.

The correctness argument is based on a closed, rooted computation: children are counted before they become executable, and a parent cannot be counted completed before counting its children. Observing a completed task through acquire operations also establishes the necessary visibility of its causal spawn chain, including cross-worker transfers. Reading all spawn counters afterward therefore includes the births of all observed completions and the children those completions created. Equality leaves no pending frontier reachable from the initialized root. A task in transit is still counted as spawned and not yet executed.

Document this argument beside the implementation and test adversarial schedules. A failed search is not evidence of termination. No external task injection is supported. No central atomic increment/decrement per task is required.

## 11. Scheduler and instrumentation

The worker object should primarily contain pointers/views, its IDs, RNG/backoff state, and optional instrumentation. The main loop should read approximately as follows; exact helper names are up to the agent:

```text
while no done/error:
    perform due termination check if worker 0
    periodically progress completed epochs

    try private pop
    if private empty:
        try acquire_half()
        retry later if epoch bookkeeping is temporarily blocked
        retry private pop

    if a batch was obtained:
        read bodies into lanes; synchronize before source-slot reuse
        execute callback on all lanes with validity mask
        synchronize; count completed tasks
        try release_half()
        reset idle backoff
    else:
        if incoming capacity is temporarily pinned:
            progress and back off
        else:
            try one direct half-steal transaction
            if success: reset backoff
            else: back off and retry
```

Do not rescan every worker for queue availability. Do not put `__syncthreads()` in a loop where warps independently become idle or exit. Keep synchronization within the warp unless it is explicitly the host-ordered initialization boundary.

Always report kernel elapsed time, launch shape, queue capacity, allocated bytes, spawned/executed totals, successful/failed claims, stolen chunks/tasks, release/acquire counts, queue peak, and error status. Distinguish logical queue occupancy from physical occupancy pinned by copies.

Detailed phase timing is compile-time optional. Prefer a short set of phases such as execute, spawn, local queue, steal claim, steal copy, progress, idle, and termination. Remove spill/global-ring/lock/sibling-only metrics. Avoid reconstructing the former scheduler solely to preserve its timing taxonomy. Produce lean throughput runs with timing disabled and diagnostic runs separately.

Report the actual kernel register count, static/dynamic shared-memory use, and occupancy-derived launch limit using CUDA function/device information and/or retained compiler output. The goal is simpler code; do not claim a register reduction without those measurements. The UTS leaf/depth atomics remain unchanged and may themselves affect multi-SM scaling; report that limitation if profiling identifies it rather than rewriting the task benchmark as part of V2.

## 12. Files and implementation order


| File/component            | Planned change                                                                                  |
| ------------------------- | ----------------------------------------------------------------------------------------------- |
| `steal_word.hpp`          | Half arithmetic, ticket interpretation, claimed-prefix calculation, counter guard contract      |
| `epoch.hpp`               | Keep a small FIFO of closed epochs with retirement counts; update comments/units                |
| `saws_ring.hpp`           | Owner half-acquire/release, geometric claim, progress/roll, safe exact receive reservation      |
| `atomics.hpp`             | DeviceScope for GPU queue/termination operations; HostScope for tests                           |
| `scheduler.cuh`           | Replace hierarchy with the flat worker loop and single-victim transfer                          |
| `termination.cuh`         | Global per-worker accounting and one controlled checker                                         |
| `kernel.cuh`              | Global worker IDs, pointer-based state, separate init kernel, no deque shared memory            |
| `run.cuh`                 | Checked allocations, initialization, cooperative launch validation, cleanup/error reporting     |
| `config.hpp`              | Remove overflow/probe-hierarchy parameters; separate worker/block count from capacity           |
| `context.cuh`, `warp.cuh` | Preserve task API and useful collectives; clarify worker ID and cross-lane ordering             |
| `stats.hpp`               | Flat-runtime counters and optional simple timing                                                |
| `examples/gpu-uts/*`      | V2 driver, fixed capacity across launch shapes, correctness checks, benchmark script            |
| `libgputc/tests/*`        | Exact half-policy and adversarial queue tests; remove obsolete spill/multi-producer-owner tests |
| `libgputc/README.md`      | Actual V2 design, contracts, flags, validation and known limits                                 |


Implement in this order:

1. Add exact arithmetic helpers and independent tests. Change packed-word interpretation and close-prefix calculations together.
2. Adapt queue ownership and epoch operations. Validate with host threads before adding a CUDA scheduler.
3. Implement initialization, allocation, cooperative launch, and the lean single-worker/single-block path using global queues.
4. Add global victim selection, synchronous half-transfer, global accounting, and multi-block termination. Validate one and two blocks first.
5. Run the required GPU tests and UTS cases; only then add a larger-grid benchmark sweep.
6. Remove obsolete code, update scripts/docs, and report source-size/resource changes and actual validation results.

Keep the CPU `libtc/` runtime, original CPU benchmarks, and unrelated examples unchanged. Use a separate V1 checkout at the inspected commit if comparison is needed. Do not expand V2 scope merely to make an existing V1-only test pass; replace tests of deleted behavior with tests of the agreed new model.

## 13. Required tests and acceptance criteria



### Host protocol tests

- Compare the constant-time arithmetic to an iterative `max(1, r / 2)` oracle for every encodable initial count 0..8191 and all valid tickets plus exhausted values. Include very large ticket counts and safe handling of `UINT32_MAX` at the pure-helper boundary.
- Verify every chunk appears exactly once across valid tickets; ranges are contiguous, nonoverlapping, and tile the epoch. Verify claimed-prefix calculations at every possible close point.
- Race several claimants with one close. Compare valid shares to the prefix from the close return value. Failed claims never retire.
- Delay retirement, including out-of-order completion and completion before close. Assert pinned bodies remain unchanged and epoch slots are not reused early.
- Exercise half-acquire concurrent with thieves, half-release after exhaustion, roll/republication, slot exhaustion, and queue wrap near the unsigned position limit.
- Test full and partial owner pops, counts around chunk boundaries, and odd public sizes.
- Verify exactly-once task IDs under concurrent ownership operations. Equal totals alone are insufficient.
- Test the single-owner capacity precheck with delayed outgoing copies; assert the post-claim exact reservation cannot fail in a valid transaction.
- Test injected attempt-counter guard failure and genuine queue-capacity failure with bounded completion.

Adapt the existing host test Makefile. Run ThreadSanitizer where the environment supports it. A tool startup failure is not a passing race test; report it separately. Host tests do not establish CUDA scope or GPU ordering correctness on their own.

### CUDA integration tests

- Empty internal test run, root-only run, and a one-worker configuration.
- A chain and a branching tree with known task IDs/counts; include uneven child counts and partial execution batches.
- A controlled test that forces a steal across two blocks and asserts that it happened. Random UTS completion alone is not proof that cross-block stealing was exercised.
- Delay a thief between claim and retirement while the owner attempts reclamation; validate no premature overwrite.
- Stress termination with the final runnable task transferring between workers and with a parent spawning shortly before finishing.
- Repeat small tests across several legal launch shapes and epochs/capacity configurations.
- Inject an oversize cooperative launch, queue overflow, and counter guard condition; verify explicit failure rather than a hang or successful throughput record.
- Run Compute Sanitizer memory and synchronization checks when available. Do not describe them as a proof of absence of all global-memory races.



### UTS validation

Compare against `examples/uts/sample_trees.sh` and the unchanged CPU tree generator. Required small workloads:


| Tree | Nodes     | Leaves    | Maximum depth |
| ---- | --------- | --------- | ------------- |
| T1   | 4,130,071 | 3,305,118 | 10            |
| T3   | 4,112,897 | 3,599,034 | 1,572         |
| T5   | 4,147,582 | 2,181,318 | 20            |
| T2   | 4,117,769 | 2,342,762 | 81            |


Require both correct reference results and `spawned == executed`, with no error flag. Run at one block, two blocks, and an occupancy-valid grid spanning the device. Repeat runs to expose races. If a supplied runtime/platform does not support executing CUDA, complete host tests and any possible compile checks, and clearly list GPU validation as unperformed; do not invent results or claim V2 validated on hardware.

### Performance measurements

Keep `-W` as warps per block and add `-B` for block count (`0` for automatic). Support at least 1, 4, 8, 16, and 32 warps per block where the compiled kernel/resources allow it; the one-warp case is useful for controlled tests. Expose capacity through the build configuration and print it on every result. Do not shrink capacity when increasing `-W`.

Update `bench.sh` to record V2 shape, capacity, timing mode, device/compiler information, correctness status, and repetitions in machine-readable output. Failures must be visible and excluded from throughput summaries. Run a warm-up and at least three measured repetitions for performance comparisons. Record CUDA-event kernel time; identify initialization/allocation time separately if also reporting end-to-end time.

Use two comparisons: one-block V2 versus V1, and V2 scaling across block counts. Report total workers as `blocks * warps_per_block`. These answer different questions; do not attribute a whole-device speedup solely to removing shared memory. Throughput improvement is a research result, not a correctness acceptance requirement.

## 14. Definition of done and implementing-agent report

V2 is complete when the runtime and UTS build, the host protocol tests pass, available GPU validation has been run and reported accurately, and the following properties can be demonstrated from code:

- Exactly one global deque per worker; no overflow ring, spill path, or queue hierarchy remains.
- One direct victim claim at a time, with exact geometric half allocation and no mandatory discovery probe.
- Classic owner half-release and half-acquire, with safe retirement and epoch reuse.
- Full successful-ticket transfer into pre-guaranteed destination space.
- Device-wide task accounting and termination with documented ordering.
- Global initialization before execution and a legal cooperative launch.
- Explicit bounded failure on capacity/counter errors; no silent task loss or indefinite capacity wait.
- Queue capacity independent of warp count, useful flat-runtime metrics, and updated documentation.

The report should include: files changed and deleted; concise description of the ownership/ordering argument; commands and environments used for validation; actual pass/fail/unsupported results; UTS correctness and measured throughput if available; register/shared-memory measurements; and any remaining limitations. Include a diff-size summary to make the simplification assessable. Do not add abstractions simply to make the file layout resemble this document.

V2.1 is explicitly deferred: alternative fractions, multiple simultaneous victims, experiments in how to schedule multiple payload copies, task-class bins, and adaptive policies. The half-arithmetic helpers and the isolated single-steal operation are the only seams needed now.

## 15. References and provenance

- Current code baseline: [https://github.com/brianlarkins/saws/tree/61d6d2ae9518273ce360f4bccec7bba26de4fee2](https://github.com/brianlarkins/saws/tree/61d6d2ae9518273ce360f4bccec7bba26de4fee2)
- CPU release/acquire and iterative half allocation: `libtc/saws_shrb.c` at that commit. Read this as a semantic reference, not as a requirement to copy its busy waits or transport layer.
- Attached *Optimizing Work Stealing Communication with Structured Atomic Operations* (2021), especially the structured steal word and completion epochs.
- Attached *Scalable Work Stealing* (2009), especially split queues and work splitting.
- Attached Scioto paper for task-pool semantics and nonblocking task execution.
- NVIDIA CUDA execution model, including cooperative-grid progress: [https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cuda-cpp-execution-model.html](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cuda-cpp-execution-model.html)
- NVIDIA CUDA memory model and atomic scopes: [https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cuda-cpp-memory-model.html](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cuda-cpp-memory-model.html)
- NVIDIA cooperative groups and launch support: [https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cooperative-groups.html](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cooperative-groups.html)

The V2 design decisions and half-steal formula in this plan are derived from the project discussion and inspected code. They are specifications to implement and validate, not a claim that a V2 implementation or GPU benchmark has already been completed.