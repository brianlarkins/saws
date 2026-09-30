# GPU SAWS

A header-only CUDA implementation of the [V2 algorithm](../GPU-SAWS-V2-Algorithm.md).
This is the first implementation of the new design. It has not been compiled or
executed on a GPU yet, and has no new tests or performance tuning.

## Application interface

Include `<libgputc/saws.cuh>` with the repository root on the include path.
Supply a task record, an executor, and one root:

```cpp
struct Task {
    unsigned depth;
};

struct MakeChild {
    __device__ Task operator()(const Task& parent, unsigned index) const {
        return Task{parent.depth + 1};
    }
};

struct Execute {
    unsigned depth_limit;

    __device__ void operator()(saws::Context<Task>& ctx,
                              const Task& task, bool valid) const {
        unsigned children = valid && task.depth < depth_limit ? 2 : 0;
        if (!ctx.spawn_n(children, task, MakeChild{})) return;
    }
};

saws::Options options;
options.queue_capacity = 4096;
auto result = saws::run(Task{0}, Execute{10}, options);
if (!result.ok()) {
    // result.message, error_worker, and error_detail describe the failure.
}
```

`run` is blocking and uses the currently selected CUDA device. It allocates the
runtime storage, initializes a counted root on `options.stream`, launches a
persistent cooperative grid, waits on that stream, and frees its storage. It
returns the completed task count on success. Application buffers referenced by
the executor remain application-owned and must be accessible on the GPU.

Tasks must be trivially copyable and trivially copy-assignable; they need no
default constructor. The executor must be trivially copyable. Each kernel is
specialized for its task and executor types; calls do not use virtual dispatch.

### Callback contract

- One worker is one full warp. Every lane enters the callback, with at most one
  valid task per lane. Invalid lanes contribute no children or application effects.
- Every lane reaches collective operations in the same sequence. In particular,
  do not return on `!valid` before a spawn call, or loop over a lane-dependent
  child count around collective calls.
- `ctx.spawn(body, predicate)` compacts true lanes into one private reservation.
- `ctx.spawn_n(count, parent, make)` concatenates child sequences in lane order.
  Construction is distributed across lanes. `parent` must be trivially copyable
  and carry any lane-specific input. `make` must have uniform state, must not
  capture lane-local references, and cannot use warp collectives.
- A false spawn result means the whole run is stopping. Temporary pinning retries
  internally, with stop checks. Return uniformly when spawning fails.
- `ctx.lane()` and `ctx.warp()` return lane and global worker IDs.
  `ctx.warp_max(value)` is collective and returns the maximum to every lane.
- Tasks finish in finite time, cannot wait on other tasks, and cannot use block
  or grid barriers. Application data races remain the application's responsibility.

### Options and failure

- `queue_capacity`: tasks per worker, a power of two from 32 through 131072;
  defaults to 4096. There are eight epoch records per worker.
- `threads_per_block`: a multiple of 32; defaults to 128.
- `workers`: zero chooses the largest resident cooperative grid for the compiled
  kernel and block size. An explicit count must fit that grid and divide into
  whole blocks. Counts smaller than one requested block shrink that block;
  `workers = 1` therefore works without changing `threads_per_block`.
- `stream`: the CUDA stream for initialization and execution; defaults to the
  default stream. The call still waits for completion before returning.

The launcher checks cooperative-launch support, actual kernel occupancy,
descriptor limits, and allocation size arithmetic. Full queues produce an
explicit capacity error; pinned outgoing copies produce retries. There is no
spill queue or recursive child execution. Any error invalidates the run, including
partial application output. Increase capacity or change the workload to address
true capacity exhaustion; no scheduler can promise arbitrary branching fits a
fixed queue.

## Reading the implementation

- `saws.cuh`: public options/results, allocation ownership, initialization and
  cooperative launch.
- `context.cuh`: collective spawn reservations, distributed construction, and
  commit/count/publication order.
- `detail/worker.cuh`: the main loop and the complete steal-copy-retire transaction.
- `detail/deque.cuh`: owner and remote views, ring positions, publication, and
  epoch retirement/reclamation. Owner metadata lives in per-worker global storage
  and is touched only by lane zero.
- `detail/claim.hpp`: packed descriptor and exact half-steal arithmetic.
- `detail/control.cuh`: conservation counters, two-pass termination, and first-error
  election.
- `detail/warp.cuh`: only the collective helpers needed by these operations.

Reservations, claims, and closed epochs are concrete records with explicit
completion steps. No destructor publishes or retires work. The worker loop never
holds an outstanding claim while executing a callback. Owner acquisition retry
never falls through to a remote steal.

This baseline favors readable operations: generic byte-wise register broadcasts,
a simple parent-lane scan in `spawn_n`, fixed backoff/check intervals, and global
owner metadata. Their generated instructions, register lifetimes, and throughput
are intended subjects for the next GPU-backed refinement.

## Build requirements

CUDA Toolkit 11.6 or newer (for `cuda::atomic_ref`), C++17, and a GPU that supports
cooperative launch. The example targets `sm_70` by default; choose the architecture
for the intended GPU. There is no OpenSHMEM dependency on the GPU path.

See [GPU UTS](../examples/gpu-uts/README.md) for the executable and build commands.
