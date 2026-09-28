**GPU SAWS V2 code review — 28 September 2026**

Reviewed branch: `gpu_saws`, commit [`090d13c7095a21baa707e5a874506a832d103445`](https://github.com/brianlarkins/saws/tree/090d13c7095a21baa707e5a874506a832d103445). Compared with V1 `61d6d2ae9518273ce360f4bccec7bba26de4fee2` and the committed `GPU-SAWS-V2-Implementation-Plan.md`. All source locations below refer to this reviewed V2 commit. The checkout was fast-forwarded; no implementation files were edited.

**Assessment: the architectural conversion is present, but V2 is not ready for performance conclusions or correctness acceptance.** There are build blockers, a deterministic reservation-broadcast defect, unsafe warp control flow, and missing validation. These require implementation fixes rather than parameter tuning. The half-steal arithmetic itself passed independent exhaustive checks.

**Prioritized findings**

1. **P1 — The runtime has three independent C++ compilation errors.** `libgputc/kernel.cuh` line 5 includes `scheduler.cuh` before declaring `KernelArgs`; the scheduler uses that undeclared template at line 18 and in its constructor at line 32. Separately, `scheduler.cuh` line 140 declares `pick_victim()` const, but line 141 calls the mutating, non-const `next_random()`. Finally, the capacity-error path at line 281 passes a void-returning lambda to `warp::lead()`, whose implementation in `warp.cuh` line 65 unconditionally creates a variable of the lambda's return type. That instantiates an invalid variable of type void even if the error path is never taken.

   Fix declaration order with a small shared declaration or forward declaration, make victim selection non-const, and either return a value from the error lambda or perform the leader-only action with an explicit warp join. Add a compile target that includes and instantiates the actual runtime. The protocol test does not include the scheduler and cannot catch these errors. I reproduced these ordinary C++ diagnostics using the actual headers with inert CUDA declarations; this was not an nvcc build.

2. **P1 — Successful spawn reservations are returned only to lane 0.** In `scheduler.cuh` lines 268–279, every lane has its own `Range r{0,0}`. The leader lambda writes only lane 0's `r`; `warp::lead()` broadcasts the returned `ReserveStatus`, not the captured range. On success, lanes 1–31 therefore return an empty reservation. `spawn()` line 226 and `spawn_n()` line 244 let those lanes return while lane 0 proceeds into full-warp body generation/synchronization. This breaks ordinary child spawning and can produce missing task bodies, undefined collective behavior, or a hang.

   Return and broadcast one aggregate containing both status and range, or explicitly broadcast the range before any lane branches on it. A collective spawn test must assert that all 32 lanes receive identical reservation metadata, then verify exactly-once child IDs for uneven counts and partial batches. Test both spawn APIs.

3. **P1 — Stop/error decisions and termination scans are not warp-uniform.** `Worker::run()` lines 79–81 lets every lane independently load done/error and run `check_due()`. `TerminationView::check_due()` at `termination.cuh` lines 55–58 tests worker ID, not lane ID, so all 32 lanes of worker 0 scan counters. Concurrent remote work can make their scan results differ. The reservation retry at `scheduler.cuh` line 270 and idle checks at lines 293 and 298 also independently branch on asynchronously changing flags. Some lanes can leave a function or the scheduler while others continue into collectives that require participation from those lanes, including broadcasts from lane 0.

   Perform the scan only on worker 0's lane 0 and broadcast its result. Read stop/error state once per warp at each decision point and broadcast before branching. Apply this to the main loop, reservation retries, and idle path; adding a sync after divergent early returns is insufficient. Test an error arriving during a capacity wait and termination while other workers finish their final tasks. NVIDIA's [warp intrinsic constraints](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cpp-language-extensions.html) and shuffle participation rules apply here.

4. **P1 — The new nvcc-only guard breaks the existing CPU reference and host test builds.** `libgputc/common.hpp` lines 7–12 rejects host compilation and unconditionally defines CUDA function attributes. However, `examples/gpu-uts/uts_dev.cuh` includes that shared header, and the Makefile deliberately builds `uts-seq` and `test-uts-dev` with a host C++ compiler. Both targets fail at this guard. This also prevents the CPU comparison used by the benchmark script. Removing `HostScope` and both host protocol test files is an additional departure from the handoff's validation requirements.

   Restore the host/device macro fallback for common helpers and keep CUDA-only restrictions in CUDA-specific components. Restore an injectable host atomic scope for queue protocol tests or an equally meaningful host-test mechanism. Installing nvcc alone will not fix targets that are still compiled with g++.

5. **P2 — Error reporting has concurrent non-atomic writers.** `termination.cuh` lines 35–40 implements first-error selection as a relaxed load, ordinary writes to `error_worker/error_want/error_avail`, and a release store. Two failing workers can both observe no error and race on all payload fields. This does not implement the comment's first-failure guarantee and is a data race, even though the host waits for kernel completion before reading the payload.

   Elect exactly one writer with compare-exchange on the error state. Only that winner writes the diagnostic payload; other workers may stop immediately, with the host reading details after completion. Preserve requested/available counts in the returned host diagnostics—`run.cuh` currently copies only code and worker into Stats.

6. **P2 — Stealing still has a mandatory discovery probe.** `scheduler.cuh` lines 158–159 calls `stealable(vr.probe())` before the claim at line 180. The agreed baseline explicitly requires one direct atomic discovery-and-claim attempt against the chosen victim, using the returned word as authority. The probe remains race-safe because the claim is still checked, but it changes the algorithm being benchmarked and introduces the extra remote read the plan intentionally excluded. Probe misses also disappear from the reported attempt count.

   Keep the destination capacity precheck, remove the victim preprobe, and interpret the fetched word for success, exhaustion, closed state, and the attempt guard. Count unsuccessful direct attempts. Do not add a probe cache or alternate stealing policy while completing V2.

7. **P1 — The replacement CUDA test can report success after CUDA failure, and it does not test the runtime.** `libgputc/tests/test_protocol.cu` lines 158–168 ignores every allocation, launch, synchronization, and copy result. `h_fail` starts at zero, so a failed device execution or failed result copy can leave it zero and reach `test_protocol: ok`. The sole device test performs queue operations in one thread. Its `acquire_half()` call succeeds because private work remains; it does not exercise the actual public-to-private half-acquire branch. Host tests cover arithmetic and a steal-word close race, not concurrent deque ownership/retirement or the scheduler.

   Check every CUDA operation and distinguish unavailable execution from a passing test. Add the planned runtime compile check, forced cross-block transfer, exactly-once task IDs, delayed-copy reclamation, epoch reuse/exhaustion, partial batches, late-child termination, capacity failure, and counter-guard tests. The absence of hardware during implementation can justify recording tests as unrun; it does not justify omitting the tests or accepting silent CUDA failures.

8. **P2 — Benchmark output is incomplete and can misrepresent validation.** In `examples/gpu-uts/bench.sh` line 31, a nonzero GPU process exit terminates the script under `set -e` before the captured failure is logged. The lane-fill field read at line 40 is never emitted by `Stats::print_kv()` in `stats.hpp`, so successful rows print 0.0% lane utilization. The header format at line 19 also has fewer placeholders than arguments and produces a stray row. I reproduced both the missing failure log and 0.0% result using controlled executable output; a full 32-task batch should have been 100%.

   Capture exit status without losing stdout/stderr, log failures explicitly, and exclude failed results from throughput summaries. Emit lane fill or derive it from tasks and chunks. Include measured repetition index, timing mode, compiler/device identity, and correctness status. Conservation alone does not compare UTS nodes/leaves/depth against the reference; neither the executable nor this script currently enforces those reference results. Add the reference checks before accepting benchmark rows.

**Additional implementation and cleanliness work**

| Location | Issue and recommended correction |
| --- | --- |
| `saws_ring.hpp` line 130; `scheduler.cuh` lines 117, 152 | `acquire_half()` conflates an empty queue with temporary epoch-slot exhaustion. The second acquire in `try_steal()` ignores its result. Preserve a retry outcome and resume local acquisition before initiating a remote steal; this matches the planned scheduler and avoids unnecessary remote work while owned work is pending. |
| `termination.cuh` lines 19–24, 45–49 | Neither counter increments nor the aggregate sums detect 64-bit overflow. Add checked arithmetic or an enforced documented run bound; otherwise wrap can theoretically manufacture false equality. This is unlikely for the supplied UTS cases but is an explicit missing contract. |
| `scheduler.cuh` lines 27, 80 | `sched_iter` is signed and incremented indefinitely. Use an unsigned counter for intentional wraparound. |
| `scheduler.cuh` lines 134–136, 215, 230, 258 | `released_chunks` increments by one per counted event, regardless of actual chunks, while releases from spawn/receive paths are not counted. Return the release size and account for all release sites consistently. |
| `scheduler.cuh` lines 275–276 | The sole queue peak records physical usage during spawn reservations only; incoming transfers are missed and logical occupancy is not separately recorded. |
| `saws_ring.hpp` lines 84–86, 164–166, 188–191 | Progress/reservation unconditionally closes and republishes a live public epoch when possible, even when nothing was claimed. This is an avoidable source of global atomic/store traffic. After correctness is established, roll only when needed to advance reclamation or resolve pressure; measure rather than assuming a throughput impact. |
| `run.cuh` line 161 | `dynamic_smem` is assigned `attr.sharedSizeBytes`, the static allocation. This launch requests zero dynamic shared memory; report zero. |
| `run.cuh` line 71 | Negative block counts silently select automatic launch. Reject negative values rather than accepting malformed configuration. |
| `libgputc/README.md`; `libgputc/Makefile` | README advertises `QUEUE_CHUNKS` in both directories, but only the GPU-UTS Makefile implements it. Document the actual supported override. |

The architecture is simpler: the overflow queue, queue lock, shared deque allocation, and sibling hierarchy are gone. The source-size claim should still be measured accurately: top-level `libgputc` runtime headers increased from 1,365 to 1,468 lines, while `scheduler.cuh` became 66 lines shorter. Deleted documentation and tests should not be counted as runtime simplification. No register or occupancy improvement has been measured here.

**Compliance with the agreed V2**

| Requirement | Review result |
| --- | --- |
| Multiple blocks; one globally indexed warp worker per deque | Present in initialization and persistent kernel code. |
| Global body slab and queue states; no hierarchy or overflow lock | Present. |
| Capacity independent of warp/block count | Present; UTS uses one configurable capacity across its launch variants. |
| Cooperative launch checked against occupancy; separate initialization | Present in source; hardware behavior untested. |
| Pure geometric half arithmetic | Present; independent exhaustive host check passed. |
| Owner half-release and half-acquire | Implemented; focused sequential checks passed, concurrency coverage incomplete. |
| Device-scope publication/retirement | Appropriate acquire/release operations and copy joins are visible; no complete GPU correctness claim. |
| Direct victim claim without discovery probe | Not compliant. |
| Guaranteed incoming space before claim; full transfer and retirement | Capacity check and complete chunk-copy path present; integration untested. |
| Uniform collective control, safe failure, correct accounting | Blocked by findings above; overflow protection also missing. |
| Meaningful host and CUDA validation, accurate benchmark results | Incomplete and partly broken. |
| V2.1 geometries/multiple victims deferred | Compliant. |

**Validation actually performed**

The environment has g++ but no nvcc or visible NVIDIA device. No CUDA compilation, GPU UTS execution, Compute Sanitizer run, register measurement, or performance result is claimed.

| Check | Result |
| --- | --- |
| Fetch and fast-forward `gpu_saws`; inspect final worktree | At `090d13c`; tracked source unchanged. |
| `make test` in `libgputc` | Unavailable: `/usr/local/cuda/bin/nvcc` does not exist. |
| `make test-uts-dev` in `examples/gpu-uts` | Failed: newly introduced nvcc-only guard and CUDA attributes in the host build. |
| `make uts-seq` in `examples/gpu-uts` | Failed for the same regression. |
| C++ syntax diagnostics using real runtime headers and inert CUDA declarations | Confirmed missing `KernelArgs`, const violation, and void result in `warp::lead`. No CUDA semantics validated by this check. |
| Real half helpers through a temporary host adapter, compiled with undefined-behavior sanitizer | Passed all initial counts 0..8191, 40 ticket positions, independent iterative shares, contiguous tiling, close prefixes, and very large ticket indices including `UINT32_MAX`. |
| Real ring helpers through a temporary CPU atomic adapter | Passed a focused sequential sequence with out-of-order retirement before close, delayed oldest retirement, owner reacquisition, and task-position wrap near `UINT32_MAX`. This is not a concurrent queue stress test or CUDA memory-model validation. |
| Isolated benchmark fixture | Reproduced missing failed-run log, absent lane-fill metric interpreted as zero, and malformed header. |

The temporary adapters and fixtures were kept outside the repository. They support specific findings and arithmetic checks; they do not replace the planned test suite.

**Recommended correction order for the coding agent**

1. Fix runtime compilation and restore both host UTS targets. Compile the actual UTS scheduler instantiations, not only protocol headers.
2. Broadcast reservation results and make all collective stop/error decisions uniform. Add focused regression tests for these two defects before general stress tests.
3. Make first-error election atomic, preserve diagnostics, distinguish acquire retry from empty, and add the accounting bounds.
4. Restore direct-claim stealing and host protocol testing. Preserve the working half arithmetic and the existing epoch ownership model.
5. Add checked CUDA tests that exercise the actual scheduler, then run one worker, one block, two blocks with a forced cross-block steal, and an occupancy-valid grid. Include injected failure and delayed-copy cases.
6. Validate T1/T3/T5/T2 nodes, leaves, and maximum depth against the plan, with conservation and zero errors, over repeated runs. Run available memory/synchronization sanitizers and report unsupported checks honestly.
7. Correct statistics and benchmark logging, then collect warm-ups and repeated timings. Record registers, shared memory, capacity, launch shape, and timing mode. Keep one-block V1/V2 comparison separate from full-device scaling.

No alternative geometry or additional scheduling layer is needed to address these findings.
