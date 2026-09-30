# GPU UTS

Unbalanced Tree Search using the new `libgputc` task API. `tree.cuh` contains the
task record, child generator, and single executor callback. `rng.cuh` implements
the SHA-1 root/child construction used by the existing CPU UTS example. `uts.cu`
handles command-line parameters, application statistics, and the blocking run.

The source is a first implementation and has not been compiled or run on CUDA.

## Build and run

From the repository root, on a CUDA development machine:

```sh
make -C examples/gpu-uts ARCH=sm_80
./examples/gpu-uts/gpu-uts --help
./examples/gpu-uts/gpu-uts -t 3 -b 4 -d 6
```

Override `ARCH` for the target device. The Makefile defaults to `sm_70` and uses
C++17; it does not use the OpenSHMEM build configuration.

The existing sample tree definitions use the same parameter spelling:

```sh
source examples/uts/sample_trees.sh
./examples/gpu-uts/gpu-uts $T1 --capacity 4096
./examples/gpu-uts/gpu-uts $T3 --capacity 16384
```

These are commands for future GPU validation, not verified results. Capacity
requirements depend on branching and the run's scheduling. If SAWS reports
capacity exhaustion, increase `--capacity` within its supported bounds.

Supported UTS parameters are `-t`, `-a`, `-b`, `-d`, `-r`, `-q`, `-m`, `-f`, and
`-g`. SAWS options are `--capacity`, `--workers`, and `--threads`. A worker count
of zero selects an occupancy-valid cooperative grid; `--workers 1` selects a
single warp.

The output reports nodes, leaves, maximum depth, worker count, and elapsed time.
Time includes the SAWS call's allocations, initialization, launch, and result
readback. Application statistics currently use one leaf-count addition and one
depth maximum per batch. This is a clear baseline, not an optimized benchmark.

Tree distributions, truncation at 100 children (except a binomial root), and
SHA-1 seed encoding follow the CPU example. Floating-point geometric generation
still needs comparison on the target GPU. `-g` repeats the same child hash without
changing the generated tree.
