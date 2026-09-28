# libgputc: GPU SAWS V2

Header-only CUDA work stealing for irregular task pools (Scioto-style tasks, one root).
V2 is a **flat** runtime: one global-memory deque per worker warp, geometric half-stealing,
and cooperative multi-block launch.

**Build with nvcc only** — protocol headers use `#error` if included from a host C++ compiler.

## Model

- **Workers:** `worker_id = blockIdx.x * (blockDim.x/32) + warp_in_block`
- **Queues:** `deque_chunks * 32` body slots per worker in global memory
- **Stealing:** one random victim, one ticket, copy, retire, release
- **Termination:** worker 0 sums all `executed`, then all `spawned` (see `termination.cuh`)

## API

```cpp
#include "gputc.cuh"

gputc::LaunchOptions launch;
launch.blocks = 0;

gputc::Stats s = gputc::run<MyTask, MyConfig>(root, gputc::Policy{}, launch);
s.print();
```

### Configuration (`config.hpp`)

| Field             | Default |
|-------------------|---------|
| `warps_per_block` | 8       |
| `chunk`           | 32      |
| `deque_chunks`    | 256     |
| `epochs`          | 8       |
| `timing`          | false   |

`make QUEUE_CHUNKS=512` in `libgputc/` or `examples/gpu-uts/`.

## Tests

```bash
cd libgputc && make test    # half-steal checks + device ring smoke (needs GPU)
cd ../examples/gpu-uts && make
source ../uts/sample_trees.sh && ./gpu-uts $T1
```

## Layout

```
steal_word.hpp    packed word + half-steal helpers
saws_ring.hpp     deque protocol (device atomics)
scheduler.cuh     worker loop
kernel.cuh        init + persistent kernels
run.cuh           host driver (alloc, cooperative launch)
```
