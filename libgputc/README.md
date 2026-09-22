# libgputc: GPU SAWS (V1)

Header-only work stealing for irregular, discovery-driven workloads on one GPU,
following the SAWS protocol of `libtc/saws_shrb.c` with GPU-shaped steals:

- One persistent block of `Cfg::warps` warps. Each warp is a worker.
- Each warp owns a **warp deque** in shared memory: a private end it pushes and
  pops without atomics, and a public end described by one 64-bit steal word
  that sibling warps claim from with block-scope atomics.
- The block owns one **global ring** in device memory, the same queue type. In
  V1 it is the block's overflow stack: a warp whose deque is full spills its
  oldest chunks onto it, and a warp that runs dry takes the newest chunk back,
  both under a lock. Its steal word is for other blocks, which V1 does not have.
- Tasks are lane width: one chunk is 32 tasks, lane `i` runs task `i`. Steals
  take whole chunks straight into the thief's registers.

## API

```cpp
#include "gputc.cuh"

struct UtsTask {
  using Body = UtsNode;                          // trivially copyable, align <= 16

  template<class Ctx>
  static __device__ void execute(Ctx& ctx, const UtsNode& node, bool valid) {
    ctx.spawn_n(valid ? node.numChildren : 0, node,   // every lane reaches this call
                [](const UtsNode& parent, int i) { return make_child(parent, i); });
  }
};

gputc::Stats s = gputc::run<UtsTask>(root);                    // DefaultConfig
gputc::Stats s = gputc::run<UtsTask, MyConfig>(root, policy);
s.print();                                                     // summary, see below
```

`execute` runs on all 32 lanes of a warp. Lanes past the end of a partial
chunk get `valid == false` so that warp collectives stay uniform.

`Ctx` offers `lane()`, `warp()`, `warp_max(int)`, `spawn(body, pred)` and
`spawn_n(count, parent, make)`, all **warp-collective**: every lane must make
the same sequence of calls, passing `pred = false` or `count = 0` when it has
nothing to spawn.

- `spawn` pushes one task per lane where `pred` holds: one ballot, one deque
  reservation, one store per lane.
- `spawn_n` spawns `count` tasks per lane, `make(parent, i)` building the
  `i`-th. It reserves space for the whole warp's children at once and then
  builds them 32 at a time, one per lane, copying each child's `parent` to the
  lane that builds it. Uneven counts therefore do not idle lanes: for UTS's
  binomial trees, where a few lanes have 8 children and most have none,
  building children in a per-lane loop left most lanes idle.

### Configuration

Compile-time layout (`config.hpp`), overridden by deriving from `DefaultConfig`:

| Constant        | Default | Meaning                                         |
|-----------------|---------|-------------------------------------------------|
| `warps`         | 8       | warps in the block, at most 32                  |
| `chunk`         | 32      | tasks per steal, must equal the warp size       |
| `warp_chunks`   | 8       | warp deque capacity in chunks, power of two     |
| `warp_epochs`   | 8       | epoch slots per warp deque, power of two <= 16  |
| `global_chunks` | 4096    | global ring capacity in chunks, power of two    |
| `global_epochs` | 16      | epoch slots in the global ring                  |
| `timing`        | true    | per-phase cycle accounting (costs 2-6%)         |

Warp deques take `warps * warp_chunks * 32 * sizeof(Body)` bytes of dynamic
shared memory: 64 KB for the defaults with a 32-byte body, out of 99 KB per
block on sm_86.

`Policy` holds the runtime knobs and is passed to the kernel by value:
`release_min_chunks` (private chunks a warp holds before it publishes half),
`probe_retries`, the idle backoff bounds, and the waits for a pinned deque and
the global ring lock.

## Reading the stats

`Stats::print()` for UTS T3 on an RTX 3080:

```
gputc: 4112897 tasks in 0.0661 s = 62.19 M tasks/s  (1 block x 8 warps, deque 8 chunks, global ring 4096 chunks)
  conservation  spawned 4112897, executed 4112897: ok
  lanes         executing: 85.1% of lanes hold a task (27.2 per chunk); spawning: 69.0% of lanes build a child
  warp time     task 71.0% spawn 8.4% spill 0.1% pop 8.9% steal 0.8% global 0.2% release 4.0% idle 6.6%
                scheduler overhead 22.3% (everything but task and idle)
  cycles/chunk  task 4211 spawn 499 spill 3 pop 525 steal 46 global 13 release 237 idle 394  (warp cycles per executed chunk)
  chunk source  own deque 96.4%, siblings 3.1%, global ring 0.5% of 151092 chunks
  steals        4723 of 4812 claims succeeded (5660 probe rounds found a victim 85.0% of the time)
  deque         releases 6742 (7286 chunks), reacquires 2394, peak 8.0 of 8 chunks
  global ring   spills 173 (690 chunks), pops 690, peak 170.0 of 4096 chunks, pinned waits 0
                lock wait 0.07% of warp time
  idle          158 empty searches
```

- **lanes**: SIMT efficiency outside the task's own branches. *Executing* is
  how full the chunks were; *spawning* is how full `spawn_n`'s build rounds were.
- **warp time**: every cycle of every warp, charged to exactly one phase.
  Phases nest (a spill inside a spawn inside a task) and cycles go to the
  innermost. `task` is the application's code, including the `make` calls of
  `spawn_n`; `spawn` is only the reservation and bookkeeping. The same split in
  absolute terms is on the `cycles/chunk` line.
- **chunk source**: where executed chunks came from. A healthy run is mostly
  `own deque`; `global ring` climbing means deques are too small for the tree.
- **steals**: claims that came back empty lost a race to another thief or the
  owner. Probe rounds that found no victim end the search.
- **global ring**: `peak` shows how close the run came to trapping on a full ring.
  `lock wait` is the cost of sharing it.

`-S 1` in `gpu-uts` adds the same breakdown per warp, and every run ends with a
`gputc_kv` line of `key=value` pairs (every counter plus derived fractions)
for scripts; `examples/gpu-uts/bench.sh` turns those into a table.

## How it works

### The steal word

```
 63          32   31     30  27  26     14  13     0
+--------------+--------+-------+---------+--------+
|   claimed    | closed | epoch |  avail  |  base  |
+--------------+--------+-------+---------+--------+
```

All counts are in chunks. A thief probes the word, then issues
`fetch_add(n << 32)`; the returned word says which chunks it owns (ticket `k`
owns chunk `k`), and `interpret()` turns it into a `Ticket`. Keeping `claimed`
in the high half means an overclaim never carries into the other fields; the
counter itself could wrap after 2^32 claims, which probing before claiming
prevents. The owner closes an epoch with `fetch_or(CLOSED_BIT)`;
`taken(pre_close) = min(claimed, avail)` is exactly the number of chunks that
went to valid tickets.

### `SawsRing<Body, Scope, Chunk, CapChunks, Epochs>`

One queue template serves the warp deques (`BlockScope`), the global ring
(`DeviceScope`) and the host tests (`HostScope`). Positions are free-running
task counters:

```
tail ..closed epochs.. pub_base ..published epoch.. split ..private.. head
```

- `push_reserve(want, min)` and `pop(max)` work the private end.
- `release(k)` closes the published epoch, then publishes one new epoch
  covering its unclaimed remainder plus the `k` oldest private chunks. They
  are adjacent, so nothing is copied.
- `reacquire()` closes the published epoch and moves the unclaimed remainder
  back to the private end.
- `take_oldest(k)` removes the `k` oldest chunks the owner holds, for spilling.
- Thieves `retire` into their epoch's counter once the copy-out is done.
  `reclaim()` frees whole epochs, oldest first, once `retired == taken`.
- When a push does not fit, the owner rolls the published epoch (closes it and
  republishes the remainder) so its claimed chunks become reclaimable.

Methods are scalar protocol steps. On the device, lane 0 runs them inside
`warp::lead`, which broadcasts the result after a `__syncwarp()`, and then all
lanes move the bodies.

### Scheduling

Per warp: pop a chunk from the private end; else reacquire the public end;
else probe all sibling steal words in one load (lane `i` reads warp `i`),
ballot, pick a uniformly random stealable victim, and claim one chunk; else
take the newest chunk of the global ring; else idle.

After each chunk, a warp whose public end is exhausted and that holds at least
`release_min_chunks` private chunks releases half of them. When a spawn does
not fit, the warp reacquires its public end and spills the oldest half of its
chunks onto the global ring.

Spilling the oldest and taking back the newest keeps the traversal depth-first,
which bounds memory to about warps x tree depth x chunks per level. Either
reversal breaks this: keeping the oldest in the deque, or consuming the global
ring oldest-first, turns the traversal breadth-first, and UTS T1 then overflows
a 4096-chunk ring.

### Termination

Each warp's lane 0 counts its own `spawned` and `executed`. An idle warp sums
every `executed` counter, then every `spawned` counter. A task's spawn happens
before its execution, which happens before its executed increment, so the
executed tasks it sees are a subset of the spawned tasks it reads afterwards.
Equal sums mean no task is pending anywhere, and the warp sets `done`.

## Layout

```
steal_word.hpp    steal-word encoding, interpret, taken
common.hpp        GPUTC_HD, GPUTC_CHECK, small helpers
epoch.hpp         EpochFifo: closed epochs awaiting reclamation
atomics.hpp       BlockScope / DeviceScope (cuda::atomic_ref), HostScope
saws_ring.hpp     SawsRing: the queue
config.hpp        DefaultConfig, Policy
stats.hpp         Phase, WarpCounters, Stats and its reports
warp.cuh          lane helpers, lead/shfl, scans, victim pick, body moves
context.cuh       Ctx: the device API tasks see
termination.cuh   counters and the done flag
scheduler.cuh     Worker: search, steal, release, spawn, spill, idle, timing
kernel.cuh        persistent_kernel, shared-memory carve-up
run.cuh           host entry point
gputc.cuh         umbrella include
tests/            host-only protocol tests
```

## Building and testing

Host tests need only a C++17 compiler; they are written against `HostScope`
and run owner and thieves on `std::thread`s:

```sh
make test     # steal-word tiling and close races, SawsRing torture incl. spills and position wrap
make tsan     # the same under ThreadSanitizer
```

The device code needs CUDA 12 (`cuda::atomic_ref`) and sm_80 or newer
(`__reduce_max_sync`, `__nanosleep`). See `examples/gpu-uts`:

```sh
cd ../examples/gpu-uts
make                                        # gpu-uts for this machine's GPU, and uts-seq
source ../uts/sample_trees.sh
./test-uts-dev $T1                          # host check of the ported tree code (make test-uts-dev)
./gpu-uts $T1                               # -W 16 for 16 warps, -S 1 for per-warp stats
./uts-seq $T1                               # one CPU core, same tree code
WARPS="8 16 32" ./bench.sh                  # table over T1 T3 T5 T2
```

`gpu-uts` prints UTS's usual `Tree size = ...` line, to compare against
`sample_trees.sh`, and exits nonzero unless `spawned == executed`.

## V1 limits

- One block. Termination counters live in shared memory and the kernel is
  launched with `gridDim.x == 1`.
- One task class per run.
- The global ring is shared under one lock. With small deques (16 or 32 warps)
  most chunks round-trip through it and the lock dominates; a per-warp
  overflow region in global memory (split deques) would remove that.
- A full global ring traps with a message (`raise Cfg::global_chunks`)
  instead of running tasks inline.
- Steals are one chunk (`n = 1`); the steal word and `SawsRing` already
  support wider claims.
- Plain CUDA device memory for the global ring; no NVSHMEM.
