# libgputc: GPU SAWS (V1)

Header-only work stealing for irregular, discovery-driven workloads on one GPU,
following the SAWS protocol of `libtc/saws_shrb.c` with GPU-shaped steals:

- One persistent block of `Cfg::warps` warps. Each warp is a worker.
- Each warp owns a **warp deque** in shared memory: a private end it pushes and
  pops without atomics, and a public end described by one 64-bit steal word
  that sibling warps claim from with block-scope atomics.
- The block owns one **global ring** in device memory, the same queue with
  device-scope atomics. It is fed when a warp deque overflows and drained by any
  warp whose sibling probes come up empty.
- Tasks are lane width: one chunk is 32 tasks, lane `i` runs task `i`. Steals
  take whole chunks straight into the thief's registers.

## API

```cpp
#include "gputc.cuh"

struct UtsTask {
  using Body = UtsNode;                          // trivially copyable, align <= 16

  template<class Ctx>
  static __device__ void execute(Ctx& ctx, const UtsNode& node, bool valid) {
    int mine = valid ? node.numChildren : 0;
    int most = ctx.warp_max(mine);
    for (int i = 0; i < most; ++i) {
      bool make = i < mine;
      UtsNode child{};
      if (make) child = make_child(node, i);
      ctx.spawn(child, make);                    // every lane reaches this call
    }
  }
};

gputc::Stats s = gputc::run<UtsTask>(root);                    // DefaultConfig
gputc::Stats s = gputc::run<UtsTask, MyConfig>(root, policy);
```

`execute` runs on all 32 lanes of a warp. Lanes past the end of a partial
chunk get `valid == false` so that warp collectives stay uniform.

`Ctx` offers `lane()`, `warp()`, `warp_max(int)` and `spawn(body, pred)`.
`spawn` and `warp_max` are **warp-collective**: every lane must make the same
sequence of calls, passing `pred = false` when it has nothing to spawn. One
`spawn` call costs one ballot, one cursor update, and one coalesced store for
up to 32 new tasks.

`Stats` reports `executed`, `spawned`, steals from siblings and from the global
ring, `steal_misses` (a probe looked stealable but the claim came back empty),
`releases`, `reacquires`, `promotes`, and wall time in `seconds`.

### Configuration

Compile-time layout (`config.hpp`), overridden by deriving from `DefaultConfig`:

| Constant        | Default | Meaning                                     |
|-----------------|---------|---------------------------------------------|
| `warps`         | 8       | warps in the block, at most 32              |
| `chunk`         | 32      | tasks per steal, must equal the warp size   |
| `epochs`        | 16      | epoch slots per ring (4 steal-word bits)    |
| `warp_chunks`   | 8       | warp deque capacity in chunks, power of two |
| `global_chunks` | 4096    | global ring capacity in chunks, power of two|

Warp deques take `warps * warp_chunks * 32 * sizeof(Body)` bytes of dynamic
shared memory (64 KB for the defaults with a 32-byte body).

`Policy` holds the runtime knobs and is passed to the kernel by value:
`release_min_chunks` (private chunks a warp holds before it publishes half),
`probe_retries`, and the idle backoff bounds.

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
in the high half means overclaims only carry out of bit 63. The owner closes an
epoch with `fetch_or(CLOSED_BIT)`; `taken(pre_close) = min(claimed, avail)` is
exactly the number of chunks that went to valid tickets.

### `SawsRing<Body, Scope, Cfg>`

One queue template serves the warp deques (`BlockScope`), the global ring
(`DeviceScope`) and the host tests (`HostScope`). Positions are free-running
task counters:

```
tail ..closed epochs.. pub_base ..open epoch.. split ..private.. head
```

- `release(k)` closes the open epoch, then publishes one new epoch covering
  its unclaimed remainder plus the `k` oldest private chunks. They are
  adjacent, so nothing is copied.
- `reacquire()` closes the open epoch and moves the unclaimed remainder back
  to the private end.
- Thieves `retire` into their epoch's counter once the copy-out is done.
  `reclaim()` frees whole epochs, oldest first, once `retired == taken`.
- When a push does not fit, the owner rolls the open epoch (closes it and
  republishes the remainder) so its claimed chunks become reclaimable.

Methods are scalar protocol steps. On the device, lane 0 runs them and all
lanes move the bodies between steps, with `__syncwarp()` ordering the two.

### Scheduling

Per warp: pop a chunk from the private end; else reacquire the public end;
else probe all sibling steal words in one load (lane `i` reads warp `i`),
ballot, pick a random stealable victim, and claim one chunk; else claim from
the global ring; else idle.

After each chunk, a warp whose public end is exhausted and that holds at least
`release_min_chunks` private chunks releases half of them. When a spawn does
not fit, the warp takes the global ring's lock, moves the top half of its
private chunks into the ring, and publishes them.

### Termination

Each warp's lane 0 counts its own `spawned` and `executed`. An idle warp sums
every `executed` counter, then every `spawned` counter. A task's spawn happens
before its execution, which happens before its executed increment, so the
executed tasks it sees are a subset of the spawned tasks it reads afterwards.
Equal sums mean no task is pending anywhere, and the warp sets `done`.

## Layout

```
StealWord.hpp     steal-word encoding, interpret, taken
common.hpp        GPUTC_HD, small helpers
ring.hpp          RingView: power-of-two index math, span splitting
epoch.hpp         EpochFifo: closed epochs awaiting reclamation
atomics.hpp       BlockScope / DeviceScope (cuda::atomic_ref), HostScope
saws_ring.hpp     SawsRing: the queue
config.hpp        DefaultConfig, Policy
stats.hpp         Counters, Stats
warp.cuh          lane helpers, broadcast, victim pick
context.cuh       Ctx: the device API tasks see
termination.cuh   counters and the done flag
scheduler.cuh     Worker: search, steal, release, spawn, promote, idle
kernel.cuh        persistent_kernel, shared-memory carve-up
run.cuh           host entry point
gputc.cuh         umbrella include
tests/            host-only protocol tests
```

## Building and testing

Host tests need only a C++17 compiler; they are written against `HostScope`
and run owner and thieves on `std::thread`s:

```sh
make test     # steal-word tiling and close races, ring spans, SawsRing torture
make tsan     # the same under ThreadSanitizer
```

The device code needs CUDA 12 (`cuda::atomic_ref`) and sm_80 or newer
(`__reduce_max_sync`, `__nanosleep`). See `examples/gpu-uts`:

```sh
cd ../examples/gpu-uts
make test-uts-dev && ./test-uts-dev $T1     # host check of the ported tree code
make && source ../uts/sample_trees.sh && ./gpu-uts $T1
```

`gpu-uts` prints UTS's usual `Tree size = ...` line, to compare against
`sample_trees.sh`, and checks `spawned == executed`.

## V1 limits

- One block. Termination counters live in shared memory and the kernel is
  launched with `gridDim.x == 1`.
- One task class per run.
- A global ring that stays full traps with a message
  (`raise Cfg::global_chunks`) instead of running tasks inline.
- Steals are one chunk (`n = 1`); the steal word and `SawsRing` already
  support wider claims.
- Plain CUDA device memory for the global ring; no NVSHMEM.
