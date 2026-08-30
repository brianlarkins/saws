# gpubpc

GPU port of the Bouncing Producer-Consumer microbenchmark
(`examples/bpc/bpc.c`), running on `libgputc`.

**Status: sketch, never compiled.** See `../libgputc/README.md`.

## Why BPC first

BPC has exactly two task classes with a tunable cost ratio, which makes it a
direct knob on the load-versus-coherence tension that a GPU work stealer has to
resolve and a CPU one does not:

```bash
./gpubpc -p 1 -c 1      # equal cost: coherence is free, isolates load balance
./gpubpc -p 1 -c 10     # default:    a mixed warp wastes lanes on the slow class
./gpubpc -p 1 -c 100    # extreme:    coherence should dominate the result
```

Sweeping `-c` and comparing against `cfg.coherence_aware = 0` is the cheapest
available experiment on whether class-segregated queues are worth their
complexity. If the two configurations track each other across the sweep, the
whole two-dimensional queue array in `libgputc` is unjustified and should be
deleted.

UTS is the better *first* benchmark for the load-balancing question alone,
because it has a single task class and therefore no coherence confound. BPC is
the better benchmark for the question that is actually novel here.

## What changed from the CPU version

| CPU | GPU | Why |
|---|---|---|
| `gtc_clo_associate()` counters | `__device__` globals + `atomicAdd` | replicated local objects have no analogue; an atomic on a device global is cheaper than the key lookup was |
| `nanosleep()` | `clock64()` spin | burning an SM to simulate work is backwards on a GPU, but a known per-task cost is the benchmark's entire purpose |
| `gtc_add(gtc, task, me)` | `gputc_dev_add(ctx, cls, body)` | tasks are spawned from device code; the persistent kernel never returns to the host mid-run |
| task fn args via `gtc` handle | `__constant__` parameters | task bodies are reached through a function pointer and cannot take extra arguments |
| `initial_producers = 1` | `initial_producers = 64` | a single producer chain is serial and would leave a GPU essentially idle; parallelism has to come from many chains in flight |
| `maxdepth = 2000` | `maxdepth = 512` | keeps the run short enough to iterate on while the counts stay large |

## The structural problem this exposes

The producer chain is serial by construction: producer at level *k* spawns
exactly one producer at level *k+1*. On a cluster with a few hundred PEs that
is tolerable. On a GPU with thousands of resident warps, one chain leaves
essentially the whole device idle, and even 64 chains only occupies 64 lanes'
worth of the producer class.

This is not a flaw in the port — it is BPC accurately reporting that its
producer side does not have GPU-scale parallelism. Two consequences:

1. **Consumers must disperse fast.** The interesting metric is time-to-first-
   steal and the dispersion phase, not steady-state throughput. `libgputc`
   instruments neither yet; the CPU library's dispersion timer
   (`TC_START_TIMER(tc, dispersion)`) should be ported before running this in
   anger.

2. **The producer class will be perpetually starved relative to the consumer
   class.** Warps that adopt the producer class as their preference will spend
   most of their time falling through to tier 1. Whether that is a real cost or
   just an artifact of the preference being assigned round-robin at launch is
   an open question — assigning preferences in proportion to observed class
   population would be the obvious fix.

## Known gaps

- **Queue-full runs the child inline** rather than dropping it, which keeps the
  task count correct so the SUCCESS/FAILURE check stays meaningful, but
  silently distorts the load balance. Watch for it whenever efficiency looks
  anomalously low.
- **The `-b` bouncing mode is ported but untested in concept.** On the CPU it
  makes the producer chain migrate; whether that is still meaningful when the
  unit of migration is a warp rather than a process needs thought.
- **`clock64()` spins are per-lane**, so a warp's cost is the max over its
  lanes. With a uniform class that max is the common case — which is exactly
  the property the class-segregated queues exist to preserve, so the benchmark
  and the mechanism are somewhat entangled. Worth keeping in mind when reading
  results.
- **Efficiency against `ideal_walltime` is not meaningful across devices**
  since the ideal assumes perfect division of a fixed cost that is itself
  defined in terms of the SM clock.

## Building

```bash
make CUDA_HOME=/usr/local/cuda NVSHMEM_HOME=/opt/nvshmem
```

## Running

```bash
nvshmrun -n 4 ./gpubpc -d 512 -n 10 -i 64 -c 10 -v
```
