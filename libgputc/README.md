# libgputc

A prototype port of the SAWS task collection to CUDA + NVSHMEM.

**Status: sketch.** This has never been compiled and there is no GPU on the
machine it was written on. It exists to show the shape of the port — how the
steal protocol maps onto device code, where the CPU design survives intact, and
where it has to change. Treat every line as a design proposal, not as working
code.

## What ports unchanged

The core of SAWS is not the ring buffer. It is this:

> Replace negotiation with a locally computable allocation function over an
> atomically issued ticket.

A steal is one `fetch_add` that simultaneously claims a ticket and returns the
epoch parameters, followed by pure arithmetic that recovers the thief's exact
byte range, followed by a one-sided transfer. The victim never participates.

That structure survives the move to GPU completely, and it survives it *well*,
because the tradeoff it makes — spend arithmetic to avoid communication — is
more favorable on a GPU than on a cluster. ALU is nearly free; global atomics
serialize under contention.

NVSHMEM is what makes the port short. `nvshmem_uint64_atomic_fetch_add` and
`nvshmemx_getmem_nbi_warp` are callable from inside a running kernel, so the
protocol moves into device code nearly verbatim with a *block* substituted for
a PE.

## File map

| This library | libtc equivalent | Contents |
|---|---|---|
| `gputc_types.cuh` | `saws_shrb.h`, `tc.h` | steal-word layout, queue and epoch structs |
| `gpu_shrb.cuh` | `saws_shrb.c` | the steal protocol, all `__device__` inline |
| `gpu_task.cuh` | `task.c` | class registry, device-side spawn and dispatch |
| `gpu_termination.cuh` | `termination.h` | two-scope termination detection |
| `collection.cu` | `collection-saws.c` | persistent kernel, host process loop |
| `gputc_host.cu` | `init.c`, `handle.c` | lifecycle, seeding, statistics |
| `gputc.h` | `tc.h` | public host API |

## The three changes that matter

### 1. The steal schedule is flat, not geometric

This is the change that took the most iteration, and the reasoning is worth
recording because the obvious port is wrong in a way that is easy to miss.

Geometric halving bounds the thief count at about `log2(N)` — which is why
`SAWS_MAX_STEALS_PER_EPOCH` is 22. With 132 SMs and several resident warps
each, thousands of claimants compete for those 22 slots. That much is
predictable from the analysis.

The subtler problem is that halving **front-loads enormous shares**: the first
thief is offered half the epoch. A CPU thief accepts that into a `malloc`'d
buffer. A GPU thief cannot — a warp has a fixed staging buffer and executes 32
tasks at a time. So it must either truncate its claim or loop. Truncating is
silently wrong: later tickets compute their offsets from the *schedule*, not
from what any thief actually transferred, so the untaken remainder of an
over-large claim is orphaned and never executed by anyone.

An intermediate design — halve until the share drops below a warp, then issue
fixed chunks — fixes the claimant count but not the front-loading, and its
first ticket still claims 131072 tasks out of a 256K epoch.

So the schedule is flat. The epoch divides into fixed `GPUTC_STEAL_CHUNK`
(one warp) chunks, and a thief claims as many *consecutive* chunks as it can
hold by adding that many to the claim counter in its single atomic:

```
offset(k) = k * CHUNK
share(k)  = min(nchunks * CHUNK, itasks - offset(k))
```

| Epoch size | geometric tickets | flat tickets |
|---:|---:|---:|
| 64 | 2 | 2 |
| 1 024 | 6 | 32 |
| 16 384 | 10 | 512 |
| 262 144 | 14 | 8 192 |

What is given up is steal-half adaptivity, which mattered on the CPU because a
steal cost a network round trip and a worker wanted as much as possible from
it. The equivalent lever here is `nchunks`, chosen per tier from known latency
rather than derived from queue occupancy — a remote steal should claim several
chunks, a local one just enough. Both are currently 1 because the staging
buffer is one chunk; raising `GPUTC_REMOTE_CHUNKS` costs shared memory and
therefore occupancy, which is a real tuning decision that wants measurement.

The invariant that matters — shares for tickets `0..max_tickets-1` tile the
epoch exactly, with no gap and no overlap, and any later ticket gets zero — was
checked exhaustively over 5 295 epoch sizes. The geometric variants failed it
for every odd size, because `share(k) = N >> (k+1)` does not telescope under
integer truncation; it has to be `(N>>k) - (N>>(k+1))`. That bug would have
manifested as a handful of tasks silently never executing, on odd epoch sizes
only, which is close to the worst possible failure mode to debug on a GPU.

### 2. Completion tracking is counters, not an array

`completed[epoch].status[22]` assumes few, individually identifiable thieves,
and reclamation scans it for the longest completed prefix. At GPU scale that is
the wrong shape.

Each epoch instead carries `retired` (tickets finished) and `taken` (tasks
removed). The owner closes an epoch with a `fetch_or` that writes
`GPUTC_EPOCH_CLOSED` into the epoch field and returns the pre-or word, whose
claim count is exactly the number of tickets issued before the close.
Reclamation is then `retired == ntickets` — one comparison instead of a scan,
and 24 bytes of state instead of ~100.

The close is race-free because the close and every claim are atomics on the
same word and therefore linearize: a thief either lands before the `fetch_or`
and is counted, or sees `CLOSED` and bails without ever retiring. There is no
window in which a thief proceeds uncounted.

*Cost:* we lose the CPU version's ability to reclaim a partial prefix of a
still-open epoch.

### 3. Local operations are no longer free

On the CPU one worker owns the head and touches it with no synchronization at
all. On the GPU the owner is an entire block, so its warps contend. Because
exactly one block owns each queue, this can be a **block-scoped** atomic
(`atomicCAS_block`) that stays in L1 and never generates cross-SM traffic —
cheap, but not free, and that changes the accounting for very fine tasks.

The steal path stays entirely lock-free. This matters more than it does on a
CPU: a lock spanning blocks can deadlock whenever co-residency is not
guaranteed, which is also why `gputc_default_nblocks()` refuses to launch more
blocks than can be simultaneously resident.

## The tier hierarchy

The cost of finding work varies by four orders of magnitude, so the kernel
tries tiers in order and lets the cheap ones absorb most of the imbalance:

| Tier | Source | Mechanism | Cost |
|---|---|---|---|
| 0 | own deque, preferred class | block-scoped atomic | ~L1 |
| 1 | own deque, any class | block-scoped atomic | ~L1, costs coherence |
| 2 | another block, this device | global atomic + copy | ~L2 |
| 3 | another PE | NVSHMEM atomic + RDMA get | ~µs |
| 4 | nowhere | host-side termination vote | ~kernel relaunch |

Tiers 2 and 3 run *identical code*. `gputc_warp_steal()` picks between a plain
`atomicAdd` and `nvshmem_uint64_atomic_fetch_add` based on whether the victim is
peer-visible. That is the clearest evidence that the protocol is the right
abstraction: the same eight lines serve both scopes.

## Coherence-aware stealing

This is the part that is genuinely new, and the reason the queue array is
two-dimensional.

On a CPU, work stealing optimizes one objective: load. On a GPU it must jointly
optimize load **and SIMD coherence**. A warp holding a mixed bag of task classes
diverges on dispatch and loses most of its throughput. It is entirely possible
to balance the load perfectly and still lose.

So the queue is not a deque of tasks; it is a set of per-class bins. Each block
owns one deque per class, a warp adopts a preferred class and asks for it first
when stealing, and the indirect call in `gputc_dev_execute()` is uniform across
the warp by construction.

The tension is real and unresolved: the most *available* work is frequently the
wrong *kind* of work. Tier 1 above will abandon a warp's class preference
rather than go off-SM looking for a match, which is a guess, not a result. The
right policy is an open question and is the most interesting thing in this
directory.

`collection-laws.c` already contains the machinery for a steal policy driven by
something other than raw availability — locality there, class coherence here.
It is the same structural change against a different metric.

## Termination

Two scopes, two mechanisms.

**Within a device**, blocks share coherent memory, so no token is needed: a
single counter of non-idle blocks is exact and cheap.

**Across devices**, the existing tree token from `termination.c` applies with
its correctness argument unchanged — the spawned/completed pair still bounds
the in-flight count, and a task spawned on one PE may legitimately execute on
another.

This prototype drives the global vote from the **host**, between kernel
launches. The kernel exits once it is locally quiescent and has failed enough
remote probes; the host votes and relaunches if the vote fails. That costs one
relaunch (~5–10 µs) per failed round, which is irrelevant next to the
microsecond steals it arbitrates, and it keeps the hardest-to-debug component
in host code. Moving it on-device is an optimization, not a design change.

The vote itself is currently an allreduce over the counters, which is O(log P)
latency but synchronizes everyone every round. Swapping in the tree token is a
drop-in replacement and is what a real implementation should do.

## Known gaps

Things a reader should not assume work:

- **Never compiled.** Expect syntax errors and NVSHMEM signature mismatches.
- **Queue-full is a dead end.** `gputc_dev_add()` returns false and the task
  is dropped. A real version needs an overflow path — spill to a global list,
  or execute the child inline and recurse.
- **`gputc_dev_add()` stages through a fixed 256-byte buffer.** Larger task
  bodies silently overflow. Should be sized from `elem_size`.
- **Only `GputcQueueSAWS` exists.** `SDC` and `LAWS` are named in the enum so
  the comparison points are explicit, and fall back with a warning.
- **No CPU/GPU hybrid.** Tasks that should run on the host have no path.
- **`nvshmemx_quiet_warp()`** is used on the assumption it exists in the target
  NVSHMEM version; check before relying on it.
- **Statistics undercount remote steals.** `nsteals` is incremented on the
  thief's side only for local steals, since the remote path would need another
  round trip to attribute it.

## Building

```bash
make NVSHMEM_HOME=/opt/nvshmem CUDA_HOME=/usr/local/cuda
```

Then see `../gpubpc` for a worked example.
