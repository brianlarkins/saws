# CLAUDE.md — SAWS / LAWS working state

## Project
Scioto-derived task collections on OpenSHMEM (`libtc`), with two queues:
**SDC** (lock-based split deferred-copy, the baseline) and **SAWS** (single-atomic
steal claims, ICPP'21). Current research: **LAWS**, locality-aware work stealing
on top of SAWS, developed on the **`laws`** branch (created from `master` on
2026-11-18).

The design lives in `~/research/docs/laws-design` (see its `CLAUDE.md`, and
`laws-directions.pdf` for the full plan). This file tracks code and progress.
Keep both current; either folder should be a valid starting point.
Since draft v0.7 the design doc has numbered definitions, each followed by its motivation, for every
term, symbol, and policy name used here (occupancy, ρ, peeks, lp<p>, laws<p>, the pk and e suffixes, …),
and a notation table after its contents. Use those names, and add a definition there for any new policy variant.

Related: `~/research/docs/saws` (ICPP'21 paper), `~/research/saws-gpu` (GPU SAWS;
Thrust B there is the GPU counterpart of LAWS).
`~/research` is a symlink to `~/Dropbox/research`.

## Branches
- `laws` — active work, pushed to `origin/laws`.
- `locality_alt`, `origin/locality` — 2024 student prototype (SDC-based, node-root
  byte flags, first-fit target). Reference only; see table 1 of the design PDF.
- `gpu_saws`, `old-collectives` — unrelated or older.

## Code map (libtc)
- `saws_shrb.{c,h}` — SAWS queue. `steal_val` packs asteals:24 | valid:2 |
  itasks:19 | tail:19. A steal is `fetch_add(1<<40)` + `getmem_nbi` + `quiet` +
  `atomic_add` into a per-epoch completion array. There are 2 epochs. The owner's
  release and reacquire are also SHMEM atomics on its own word (HCA loopback).
- `collection-saws.c` / `collection-sdc.c` — get_buf loops, progress, stats.
- `common.c` — target selection (random / round robin), `gtc_steal_tail`,
  `gtc_try_steal_tail` (SDC's default path), stats (`gtc_print_stats`).
- `init.c` — `gtc_init`. Rank 0 prints `SCIOTO build/shmem/onnode/nodes/timing` provenance
  lines (libtc rev, SOS install, libsma path, UCX version, shared-team size, shmem_ptr,
  node layout, timer mode). `gtc_ldbal_cfg_env` applies `GTC_TARGET` and related overrides.
- `laws.c` (E2) — node work bitmap (owner-only bits), `gtc_laws_select` (bitmap, then remote bitmaps,
  then a blind probe; E2c adds a far share, `laws_near_pct`), `gtc_laws_eager_clear` (E2c). SAWS peeks are in
  `saws_shrb_pop_n_tail_impl`.
- `locality.{c,h}` (E1) — node membership from host-name hashes (works for any placement and
  build; cross-checked against `SHMEM_TEAM_SHARED`), `_c->loc` (node id, node_pes, node_of[],
  ptr_ok, node team when placement is block), `gtc_is_near(pe)`, random near/far peers, and
  `gtc_near_ptr_table(symaddr)`. `GTC_DEBUG_LOCALITY=1` prints per-rank start-up stages.
- `laws_shrb.{c,h}` + `collection-laws.c` (E3, M2) — the **two-door queue** `GtcQueueLAWS` (design doc §4.3, notes §4.3.7).
  Far door = SAWS's `steal_val`/completion arrays (SHMEM domain); near door = `near_sv` + `ncompleted[2]` (CPU domain,
  node-mates via `shmem_ptr`), each on its own cache lines. Ring `tail ≤ fsplit ≤ split`. Owner ops: release (far share
  max(1,⌈ρr⌉)), near reacquire (CPU only), collapse + SAWS far reacquire, refill, re-split, maintenance check (every
  `GTC_LAWS_POLL` progress calls while a door is live). Ring is 2× the request when ρ < 1. ρ = 1 never touches the near door.
  `GTC_LAWS_CHECK=1` verifies invariants at every owner op (tests only). Door-aware selection: `gtc_laws_select_doors` in
  `laws.c`; bitmaps word [0] = B_far (= SAWS's bit, the word other nodes read), word [1] = B_near.
- Apps: `examples/uts` (`-Q B|H|L`), `examples/bpc` (`-B|-H|-L`), `examples/madness`
  (`-B|-H|-L`); tests `tests/test-tasktree{,-twotc}` (`-B|-H|-L`). B = SDC, H = SAWS, L = two-door.
  Tests: `test-laws [-L]`, `test-laws-shrb` (E3.0/E3.1: scripted unit test + randomized exactly-once stress, `-c k` = only every
  k-th PE creates tasks). `runs/e3/validate.sh` runs the E3.1 matrix.

## Lotus (cluster)
- `ssh lotus`. Compute nodes: 2 × EPYC 7352 (48 cores, 2 NUMA, 16 CCX of 3), EDR IB, 44 nodes
  (about 43 active). Large jobs preempt OSG jobs, so full-machine runs are allowed (PI).
- Default `module load openshmem` = **`openshmem/SOS-xpmem`**
  (`/opt/tools/modules/sandia-openshmem-xpmem`, SOS 1.5.3, UCX 1.20.1 + XPMEM).
  `openshmem/SOS-main` is the old UCX-only build, used for comparison. Do not use
  `~/opt/sos-xpmem` or `~/src/SOS-xpmem`.
- Modules: `gnu14 hwloc pmix ucx openshmem/...`. Launch with
  `srun --mpi=pmix --cpu-bind=cores` (the old scripts' `--mpi=pmi2` is wrong now).
- In Slurm jobs, `/etc/profile.d/lmod.sh` is a no-op, and jobs submitted from the
  user's zsh have no bash `module`. `runs/e0/env.sh` initializes Lmod itself.
- `~/saws` on lotus is an older checkout (branch `locality`, local edits). Leave it alone.
  LAWS work uses `~/laws`.

## E0: toolchain and baselines (E0 and E0b done)
Done (committed on `laws`):
- Build provenance (`libtc/buildinfo.h` generated by `libtc/Makefile`; printed in `init.c`).
- `saws_shrb.c`: per-queue damping counters (they were `static`, shared across TCs);
  completion-status polling uses atomic loads (the reacquire spin could have been
  hoisted by the compiler) with dead code removed; counters that were never
  incremented now are (`ngets`, `nsteals`, `nxfer`, `nreacquire`, `nensure`);
  per-steal averages divide by successful steals.
- `common.c`: `gtc_try_steal_tail` is now timed. A new summary line
  `: steal ops ok N avg X us, failed M avg Y us` is printed before the queue's
  gstats, so the `&&& ... c ...` line format is unchanged.
- Toolchain fixes: no `-lpmi` (`tc.mk`); `mad3d.c` `calloc` for `func_t` (GCC 14 `-Werror`).
- Release timers (SAWS and SDC) cover only actual releases. Before the fix they ran on every
  progress call, which made per-release averages meaningless and cost about 7% on UTS.
- `runs/e0/`: `env.sh` (variant → modules), `build.sh` (variant build tree),
  `gen.sh` (suites smoke/small/scale → one Slurm job per node count, all
  variant × queue × input combinations interleaved on the same allocation).
- Locality probes: `tests/microbenchmarks/locality/locbench{,2,3}.c` (results in the design PDF §3).

Workflow:
```
rsync -a --delete --exclude .DS_Store ~/research/saws/ lotus:laws/src/      # from the Mac
ssh lotus 'cd ~/laws/src && bash runs/e0/build.sh xpmem && bash runs/e0/build.sh main'
ssh lotus 'cd ~/laws/src && bash runs/e0/gen.sh <smoke|small|scale> [nodes...]'  # → ~/laws/jobs/<suite>/
ssh lotus 'for f in ~/laws/jobs/<suite>/*.sh; do sbatch $f; done'
# results: lotus:~/laws/results/<suite>/<input>.<variant>.<B|H>.<npes>.<rep>.out
```
Inputs (PI): UTS small = T1L/T1XL, scaling = T1XXL (≤ 8 nodes) and T1WL (≥ 8 nodes);
MADNESS small = `-t 10e-6`, scaling = `-t 10e-9`; BPC scaling = `-d 500 -n 8192 -b`
(ICPP'21); BPC small = `-d 100 -n 1024 -b` (my choice).
**MADNESS is off the critical path** (PI, 2026-11-18): it has large compute-heavy tasks and
is unlikely to benefit from cheaper steals, and bitrot is possible (treat
very long runs as a likely benchmark bug). It is kept only in the small suite as a
regression check; `E0_MADNESS=1 gen.sh scale` re-enables it. Earlier one-node times: T1WL about 36 min,
BPC d500 about 7 min, MADNESS 10e-9 about 2 min. Hence no T1WL below 8 nodes. The scale suite
stops at 32 nodes to avoid holding the whole cluster.

Parse: `rsync -a lotus:laws/results/<suite>/ <dir>/ && python3 runs/e0/parse.py <dir> [--csv f]`
prints medians per (input, npes, variant, queue) and lists failed or incomplete runs.

Status log:
- 2026-11-18: both variants build. Smoke (1–2 nodes, jobs 28470/28471): all 40 runs
  exit 0. The tree tests (including two-TC) succeed for SDC and SAWS on both variants.
  MADNESS 10e-6 takes 35 s on 1 node and 17.7 s on 2. (The "SAWS release 120–290 µs" figure
  noted here turned out to be a stats artifact; see below.)
- 2026-11-18: small suite (jobs 28472–28474, 1/2/4 nodes) and scale suite
  (28475–28480, 1/2/4/8/16/32 nodes) submitted.
- 2026-11-18, small suite **complete** (jobs 28472–28474, 3 reps, no failures; CSV via
  `parse.py --csv`). Findings:
  - **XPMEM vs. main: no runtime difference at 1–4 nodes** (< 0.5% on UTS T1L/T1XL,
    BPC d100, MADNESS 1e-6). Steals are too rare to matter: passive time is 0.04–0.24% for
    T1XL and 0.4–3% for BPC.
  - **SAWS beats SDC on UTS by about 9%** (T1XL: 10.91 vs. 11.97 s on 48 PEs, 2.73 vs. 3.00 s on
    192), mostly from **local-path overhead, not steals**. **Corrected by E1: this was timer
    overhead** (SDC timed reclaim on every progress call). With per-task-path timers off, SDC is as
    fast as SAWS on UTS or up to 2% faster. Progress engine time is 1.33 s
    vs. 2.12 s per PE, because SDC reclaims on every progress call and SAWS about every 20th.
    In UTS, the per-task path (progress, release check, reclaim) is about 12–18% of runtime.
  - In-app steal ops are much slower than the microbenchmark. SAWS: ok 16–26 µs, failed 17–23 µs.
    SDC failed-after-lock: 70–95 µs. Compare the uncontended 4.4 µs.
  - **Stats artifact:** the `release` "per" figure (ms per call) divides a timer that
    runs on every progress call by the number of actual releases. The same holds for SDC. It is
    not a real cost; the earlier "SAWS release 120–290 µs" signal from smoke is this artifact.
    Fixed later (see the scale entry).

- 2026-11-18 ~14:45: the scale suite was running (1/2/4/8/16 nodes; 32 nodes pending for
  resources), with an expected finish around 16:15. When resuming, check with
  `ssh lotus 'squeue -u brian; sacct -u brian -j 28475,28476,28477,28478,28479,28480 -X -o JobName,State,Elapsed'`.
  Timeouts or failures need a look before parsing.
- 2026-11-18, scale suite **complete** (jobs 28475–28480, 156 runs, all exit 0). Findings
  (written up in design doc §7, "E0 results"):
  - The reference inputs are **not steal-bound at ≤ 32 nodes**: strong-scaling efficiency is
    99–101%, idle time is ≤ 0.03% (UTS) and ≤ 4% (BPC at 1536 PEs), and there are 53–82 steals
    per PE per UTS run. H0 is not supported: XPMEM and main agree within 0.5%.
  - SAWS beats SDC by 8.6–8.9% on UTS at every scale and ties on BPC. **This was a timer artifact; see E1.**
  - In-app steal ops grow with scale: SAWS 16→104 µs and SDC 22→149 µs (48→1536 PEs).
    Dispersion max reaches 3.7 ms (UTS T1WL SAWS) vs. 15 ms (SDC), and 12–15 ms for BPC.
- 2026-11-18: **release-timer fix** (SAWS and SDC time only actual releases; rebuilt). Check
  job 28492 (1 node, T1XXL SAWS, run order reversed): 26.34–26.38 s for both builds. A real
  release costs about 0.3 µs. Removing the two TSC reads per progress call made SAWS UTS about 7%
  faster, and it also removed the one build difference in the scale suite (28.24 vs.
  30.21 s). **The E0 tables predate this fix**, so their absolute times include the overhead,
  for both queues. Lesson: timers on the per-task path are not free.

- 2026-11-18, **E0b complete** (jobs 28493–28496: 1/4/16/32 nodes, 2 reps, both builds, 152 runs,
  all exit 0; `gen.sh e0b`). Written up in design doc §7.3. Also fixed `bpc.c`: `-p`/`-c` were
  dead options (tasks always slept 1 and 5 ms). They now set the sleep, and the defaults are unchanged.
  - **Steal-bound inputs found:** UTS T3XXL (idle 3% at 4 nodes → 57% at 32; 38% efficiency
    48→1536), UTS T3WL (32–60 s runs, 2–8% idle, 100–190 TD attempts), and **BPC fine**
    (`-d 2000 -n 1024 -p 0.1 -c 0.5 -b`, idle 70% at 32 nodes, and slower at 32 than at 16 nodes).
    SAWS's lead over SDC grows with idle time: on BPC fine it goes from 0.2% to 30%.
  - **Idle time is mostly between steal ops:** BPC fine at 1536 PEs with SAWS idles 1.70 s per PE:
    0.05 s in successful steals, 0.31 s in about 30K failed probes per PE (10–12 µs each), and
    1.33 s in search, TD, and having no work. BPC fine at scale is parallelism-starved (1024
    consumers per level < 1536 PEs) and bounded by locating the bouncing producer (about 1.2 ms
    per level with SAWS, 1.7 ms with SDC).
  - XPMEM still does not change runtime. It makes on-node steals 30–40% cheaper on 1 node. One unexplained
    case: SDC on BPC fine at 16 nodes is 11% *slower* with XPMEM (consistent across reps).
  - Limits: T3L and T1XL at scale run under 1 s. Sub-ms BPC tasks pay nanosleep slack
    (BPC c0.1 reaches about 65% of ideal on 1 node).
  - **LAWS reference set (accepted by the PI, 2026-11-18):** steal-bound runs use **UTS T3XXL** (4–32
    nodes), **UTS T3WL** (16–32), and **BPC fine** (`-d 2000 -n 1024 -p 0.1 -c 0.5 -b`, 4–32). Controls,
    which LAWS must not slow down, are **UTS T1WL** and **BPC d500** (`-d 500 -n 8192 -b`).
- Cluster (PI, 2026-11-18): larger jobs on lotus preempt OSG jobs, so runs up to the full
  machine are fine. About 43 nodes are active (one is offline). Scaling runs may go to 40+ nodes.

## E1: locality discovery, local-first policies, per-scope stats (done)
- Target policies (`gtc_ldbal_cfg_t.target_selection`; env `GTC_TARGET`):
  - `random`, `rr`
  - `local-prob`: a node-mate with probability `local_search_factor`% (`GTC_LOCAL_PCT`,
    default 75); otherwise a random off-node PE
  - `local-sweep`: every node-mate once (random start, in order), then `local_sweep_far`
    off-node attempts (`GTC_SWEEP_FAR`, default 16), then sweep again. The state is per
    `get_buf` search.

  Environment values override the application's config at `gtc_create`. Bad values abort.
- Per-scope stats: a new line `: steal scope near ok N avg X us fail M avg Y us tasks T probes P
  | far ... | target <policy>` (before the queue's gstats). `parse.py` groups by policy and
  reports near%, near-ok µs, and far-ok µs.
- **Per-task-path timers are off by default** (getbuf, progress, add*, reclaim, ensure use
  `TC_START/STOP_HTIMER`). `GTC_FULL_TIMING=1` turns them back on with no rebuild. Event timers are always on.
- SAWS queues keep `rb->near_rb` (`shmem_ptr` to each node-mate's queue) for E2's peeks.
- Tests: `tests/test-locality` checks host names vs. the node map, map agreement, the ptr table,
  peer pickers, local-prob at 75%, and the exact sweep order. It passes on both builds, block and cyclic.
  Validation job 28497: all tree tests pass under every policy and both queues; the timing switch
  works; a bad `GTC_TARGET` aborts.
- **Known SOS issue:** with `--distribution=cyclic`, start-up hangs intermittently inside
  the first `shmem_malloc` after `shmem_init` (all ranks stuck before "malloc done"; about 3 of
  50 cyclic runs on both builds; 0 of more than 24 block runs; jobs 28497 and 28501). It happens
  before any libtc logic. Use block placement (the default).
- E1 suite (`gen.sh e1`, jobs 28502–28506; 1/4/16/32/40 nodes, 2 reps, XPMEM): the reference set
  × {SAWS random, lp50, lp90, sweep; SDC random}. Result files are named
  `<input>.<variant>.<policy>.<queue>.<npes>.<rep>.out`.

Results are in `lotus:~/laws/results/e1/` (parse with `runs/e0/parse.py`).

**E1 complete** (jobs 28502–28506, 200 runs, all exit 0; written up in design doc §7.4, draft v0.4):
- **SDC ≈ SAWS on UTS with per-task timers off** (SDC is 2% faster on T1WL and within ±1% on T3XXL/T3WL).
  E0's 9% SAWS lead was timer overhead. SAWS is 16–23% faster on BPC fine from 768 PEs.
- **lp50 never hurts and wins on BPC fine:** −8.6%, −18%, and −25% at 768, 1536, and 1920 PEs (2.29 vs. 3.06 s at
  1920). This comes from fewer and cheaper failed probes (25.8K vs. 36.5K per PE; near fail 6.4 vs. 9.7 µs),
  and idle time falls by 31%. lp50 is the bar to beat for E2–E4.
- **Sweep is harmful** on binomial UTS (+20–45%) and on BPC d500 (+2–6%). Per-probe success halves and the
  number of probes triples. lp90 and sweep only help BPC fine at 32–40 nodes, so no fixed bias wins everywhere (E4).
- **Distance does not set steal cost at scale; contention does.** Under random selection, near ok = far ok
  (56.8 vs. 56.7 µs, BPC fine 1536). Even near steals cost 44–68 µs, 10–15× the uncontended 4.4 µs, because
  they queue at the target's HCA. This motivates M1 peeks and M2 CPU-side near claims.

## E2: node work bitmap, peeks, remote bitmaps (done; write-up pending E2c)
PI decisions (2026-11-18): include E2b (far-side remote bitmap reads). If results disappoint and
hot-spotting at node leaders is suspected, back off to the original M1 (on-node bitmap only:
`GTC_FAR_DIR=0`). Bits are **owner-only** (as in earlier Scioto; no initiator writes, no races).
**SDC is left alone** (it can be added for ablation later).
- `libtc/laws.c`: one symmetric 64-bit word per TC, 64-byte aligned. The node leader's copy is the
  node's bitmap; bit i is node-mate i (`_c->loc.node_rank_of`). The owner sets or clears its own bit with CPU
  atomics through `shmem_ptr`, only on change (`gtc_laws_set_bit`). Node-mates read it with loads.
  Other nodes read it with `shmem_uint64_g`, a read rather than an atomic, so the word stays in the CPU domain.
- **Owner bit: set at release, cleared lazily** (PI, 2026-11-19). `saws_shrb_release`, `release_all`, and
  `reacquire` (when work stays public) set the bit as an event. `gtc_progress_saws` clears it lazily. The per-task
  path is `if (tc->bit_set && (++rb->lawscc & laws_poll_mask) == 0 && !have_stealable) clear`, with
  `GTC_LAWS_POLL` defaulting to 16 and rounded to a power of two. The first version checked on every progress call and cost 2.5% on
  T3XXL at 1 node (first E2 attempt, job 28520). That attempt was cancelled and its results kept in
  `results/e2-v1-owner-poll`. Check job 28525 (3 reps each): random 14.12 s, lp50+peek 14.13, laws poll-1
  14.28 (+1.1%), laws poll-16 14.18 (+0.4%); bit writes 231K → 54K. This finding is written up in design doc §7.5.
- `saws_shrb_have_stealable`: a local load of its own steal_val plus cached maxsteals. **Bug found and fixed:** the owner publishes steal_val with a
  non-blocking SHMEM atomic to itself, so its own load lags the write. It now keeps `rb->sv_shadow` and trusts the
  shadow when memory's low 40 bits differ.
- `GTC_TARGET=laws` (`gtc_laws_select`): a random set bit on my node (skipping node-mates found empty this
  search via `vs_state.near_miss`); otherwise up to `GTC_FAR_DIR_TRIES` (default 2) remote bitmaps; otherwise a
  blind random far probe. `GTC_FAR_DIR=0` gives M1. Falls back to random (with a warning) without SAWS or
  shmem_ptr.
- `GTC_NEAR_PEEK=1` (any policy): load a node-mate's steal_val before the NIC claim; return 0 without a
  NIC op if there is nothing to claim.
- Stats line: `: laws bitmap reads .. empty .. | peeks .. miss .. | fardir reads .. empty .. | blind .. |
  bit writes .. | occ% <11 buckets>`. `parse.py` parses it and reports failed probes per PE.
- `tests/test-laws`: owner bits set after release (local and remote view), exactly-once execution
  under laws+peeks, and bits clear afterwards. Passes 6/6 on 1–2 nodes (jobs 28518, 28519). Tree tests pass
  under all E2 policies; SOS-main falls back with a warning.
- Validation hints (2 nodes, short runs, not results): on BPC fine, 58% of peeks miss (an owner's bit stays
  set until its next progress after initiators drain it) and about 80% of remote bitmap reads are empty.
  On T3L, laws-M1 cut idle time from 34% to 18%.
- E2 suite (`gen.sh e2`, **rerun as jobs 28527–28531** with the lazy-clear owner; 1/4/16/32/40 nodes, 2 reps,
  XPMEM): the reference set × SAWS {random, lp50, lp50pk, lawsm1, laws, lawsnp}.
- Run scripts now have a **start-up hang watchdog**: if no `SCIOTO build` line appears within `E0_HANG_SEC`
  (90 s), the step is killed and retried once, and the file records `# start-up hang on try N`. Tested with a fake srun.
  The SOS start-up hang is rare with block placement: one seen (job 28525, two-TC test), then 0/40 in job 28526.
  The two-TC test passes 3/3 under laws.

**E2 complete** (jobs 28527–28531, 240 runs, all exit 0, watchdog never fired; results in
`lotus:~/laws/results/e2/`; written up in design doc §7.5, draft v0.6). Medians vs. lp50:
- **BPC fine: laws wins big**, −31% at 1536 and 1920 PEs (1.55 vs. 2.26 s; 1.60 vs. 2.31 s for M1), −3% at 768.
  The win is targeting, not locality: lp90 (79% near, no bitmap) *lost* on BPC fine in E1.
- **Binomial UTS: laws loses big**, T3XXL +7% to +34% (192–1920 PEs) and T3WL +11% to +22%. Same signature as E1's sweep:
  99% of steals near, 7× the near probes of lp90, near steals take about half as many tasks (T3WL 5.5 vs. 11.7 at 768),
  passive time 3–4× higher. lp90 (89% near) was neutral, so the cause is **strict local-first** (going off-node only
  when the node bitmap is empty), not the near share as such. Failed probes are cheap peeks (about 0.5 µs) and not the cost.
- Controls: laws +0.5–1.0% on T1WL, +1.0–2.5% on BPC d500 (16–40 nodes). M1 ≈ E2b (no leader hot-spot sign).
  Peeks help alone: lp50pk ≤ lp50 everywhere; laws without peeks is up to 8 points slower (T3XXL).

## E2c: near share and eager bit clear (done)
- `GTC_LAWS_NEAR_PCT` (`laws_near_pct`, default 100 = E2 behavior): a laws step consults the node bitmap first with this
  probability; otherwise it goes straight to remote bitmaps, then a blind probe (`laws_near_skip` counter).
- `GTC_LAWS_EAGER` (`laws_eager`, default 0): the owner also clears its bit as soon as it runs dry, in `reacquire`
  (nothing left public) and when `gtc_get_buf_saws` starts a search. Both are events; the per-task path is unchanged.
- Stats line gains `| near skip N | eager clears N | dry with bit N stealable M` after the occ% buckets. "Dry with bit"
  counts searches that start with my own bit still set (any mode; checked on the idle path only), and "stealable"
  counts those where my public portion still had unclaimed tasks (0 in every check run, so the shadow logic holds).
  The scope line's target reads `laws/dir2/near50/eager`. `parse.py`: `near_skip`, `eager_clears`, `dry_bit`, `dry_stealable`.
- `gen.sh e2c`: 4/16/32/40 nodes, 2 reps, the reference set × {lp50pk, laws, lawse, laws90, laws90e, laws50, laws50e}
  (policy names `laws[NN][e]`).
- Validation: job 28539 (2 nodes: test-laws in 4 configs, tree + two-TC tests at near 90/50/100 with and without eager,
  bad `GTC_LAWS_NEAR_PCT` aborts) all pass. Job 28540 (2 nodes, T3XXL, 3 reps; short runs, not results): laws 7.56–8.35 s
  (passive 5.5–12%), lawse 7.46–7.80, laws50 7.13–7.14 (1.1%), laws50e 7.12–7.13 (1.0%). Under the lazy clear, 0.54–1.17M
  searches per run start with the searcher's own bit set (idle PEs advertising work). Eager clears happen in
  `reacquire` (dry-with-bit becomes 0) and cut peek misses by about 60%.

**E2c complete** (jobs 28541–28544: 4/16/32/40 nodes, 2 reps, 252 runs, all exit 0, watchdog never fired; results in
`lotus:~/laws/results/e2c/`; written up in design doc §7.6, draft v0.6). Medians vs. lp50pk in the same allocations:
- **A far share fixes UTS:** laws90/laws50 (lazy) are within ±0.5% on T3WL and 3–8% faster on T3XXL from 768 PEs (strict: +9–31%).
- **Eager clearing adds as much again:** laws50e is −0.1/−7.6/−16.1/−22.3% on T3XXL (192/768/1536/1920 PEs), +0.2/−1.3/−2.4% on T3WL,
  and −1.0/−14.4/−32.5/−36.4% on BPC fine. laws90e is similar (−19.9% T3XXL, −38.6% BPC fine at 1920). Failed probes fall 48–62% on
  UTS. Bit writes rise 4–6× on UTS (CPU atomics, no measurable cost).
- Near share 50 vs. 90 differs by ≤ 5 points: 50 is better on UTS and d500, 90 (and strict+eager) 4–9% better on BPC fine at 32–40 nodes.
- Controls: every laws variant is +0.4–1.1% on T1WL (likely the per-task lazy poll; a sparser poll is untested). laws50e −0.9 to −0.1%
  on d500; strict laws +1.0–2.7%.
- Limit: lp50pk on BPC fine at 32–40 nodes was 9–12% slower in E2c than in E2 (allocation-sensitive), so compare within a suite.

**PI decisions after the E2/E2c review (2026-11-20):**
1. **E3 baselines: both laws50e and laws90e.** The PI is fine with a performance story that offers different strategies for
   different problem types (laws50e for UTS-like trees, laws90e for BPC-like producer/consumer).
2. **E4 is deferred.** No E4 work until the PI revisits it.
3. **M2 layout: one queue with two adjacent doors** (far door older, near door newer), not two independent rings. Revisit
   two rings only if thrashing under re-splits is suspected.
4. **Write the M2 design first, for PI review, before any E3 code.** The PI agreed to one bit per door (two bitmaps) with the
   variants as runtime switches over one implementation.

**M2 design approved by the PI as written (2026-11-20; design doc v0.9, §4.3, question 7):** refill and re-split in v1 and on by
default (overhead to be judged from E3 data); far share max(1,⌈ρr⌉); E3.4 matrix stands; new queue type with its own flag.
In brief: one ring, `tail ≤ fsplit ≤ split ≤ head`; far region (SAWS `steal_val`, SHMEM domain) older than the near region
(`near_sv`, CPU domain); each door has its own 2 epochs and completion arrays on separate cache lines. Owner ops: release
(SAWS rule, split by ρ: far gets max(1, ⌈ρr⌉)), near reacquire (CPU only), collapse then SAWS far reacquire, **near refill**
(near exhausted, far still has work), **re-split** (far exhausted, near has work → near block back to private). Two bitmaps
B_near/B_far (one cache line). Door policies `GTC_LAWS_DOORS` = near-first | near-only | any (any = single-bit emulation).
Switches `GTC_LAWS_RHO` (0.5), `GTC_LAWS_REFILL`/`_RESPLIT` (1). New qtype `GtcQueueLAWS` (`laws_shrb.{c,h}`, `-Q L`/`-L`);
SAWS code untouched. Test plan E3.0–E3.4 in §4.3.7. Open review points: question 7 of the design doc.

**e2p suite (sparser lazy poll), jobs 28552 (16 nodes) and 28553 (32 nodes), submitted 2026-11-20.** No code change: the
reference set × {lp50pk, laws50e at `GTC_LAWS_POLL` 16/256/4096/65536, laws90e at 16/4096}, 3 reps. Policy names
`laws[NN][e][pMM]` (gen.sh). Purpose: remove the +0.5% T1WL cost before E3 and set k for the M2 maintenance check.
Results go to `lotus:~/laws/results/e2p/`.

**E3 implementation (2026-11-20, uncommitted).** New: `libtc/laws_shrb.{c,h}`, `libtc/collection-laws.c`, `tests/test_laws_shrb.c`,
`runs/e3/validate.sh`. Changed: `tc.h` (GtcQueueLAWS, cfg `laws_rho_pm/laws_doors/laws_refill/laws_resplit`, `bit_set_near`,
`laws_door`, `near_miss_far`), `init.c` (`GTC_LAWS_RHO/_DOORS/_REFILL/_RESPLIT`), `common.c` (dispatch, config print), `laws.c`
(B_near, door-aware selection; SAWS path unchanged), apps/tests (`-L`), `gen.sh` (suites e3a/e3p/e3s; policy options
`-nf|-no|-any`, `-rNN`, `-norf`, `-nors`; job scripts export `E0_BUILD_SUFFIX`), `env.sh` (`E0_BUILD_SUFFIX`), `parse.py`
(queue L, the `laws2` line), `build.sh` (test-laws-shrb).
- Side build while e2p used the main one: sources in `lotus:~/laws/src-e3`, build `~/laws/build-xpmem-e3`
  (`E0_BUILD_SUFFIX=-e3 bash runs/e0/build.sh xpmem ~/laws/src-e3`). Its rev reads `4a0f06665c-dirty` (uncommitted).
- Validation job 28556 (1 node, 48 PEs, GTC_LAWS_CHECK=1): **84/84 pass** — stress under ρ {0.25,0.5,0.75} × refill/re-split
  {11,00,10,01} × {random, laws50e × 3 door policies}, ρ = 1, tree/two-TC tests -L under random/lp50pk/laws50e × ρ {0.25..1},
  test-laws -L, SAWS tests. Job 28562 repeats it with the improved stress test (progress every iteration, `-c 12` contention).
- E3a (1 node, E3.0 parity + E3.3) is job 28563 (after 28562). Then e3p (16 nodes) and e3s (16/32) once nodes free up.
- Validation job 28562 (improved stress: progress every iteration, `-c 12` contention): **133/133 pass**.
- **e2p complete** (jobs 28552/28553, 16/32 nodes, 3 reps, all exit 0; design doc §7.7, table 17): the lazy check was the T1WL
  cost — laws50e/laws90e +0.4–0.5% vs lp50pk at k=16, +0.0–0.1% at k ≥ 256; steal-bound inputs move ≤1.4 pts (T3XXL), ≤0.4 (T3WL),
  ≤4 (BPC fine, noisy) in no consistent direction. **k = 256 from here on** for the bit check (policy names `laws50ep256`; names
  without pMM still mean k=16 in gen.sh and the library default is still 16). On the two-door queue the same check drives refill
  and re-split (k=256 ≈ 25–130 ms on BPC fine), so E3 compares k=16 and 256 there (e3a has laws50ep256-nf-r50).

- **E3 first results (design doc §7.8):**
  - E3.0 parity (e3a job 28563, 1 node; e3p job 28565, 16 nodes; 3 reps): L at ρ=1 is never slower than SAWS and 0.4–2.5% faster on
    UTS (T1WL too, so per-task path; cause not isolated: cache-line separation / aligned ring). Compare the near door against L ρ=1.
  - E3.2 (`tests/bench-claim.c`, job 28564): H1 holds — near claim 0.65 µs alone, ≤1.4 µs with 47 initiators (T=64), vs SAWS 4–41 µs;
    peeks matter for the near door too (no peeks: 11 µs at N=47). In-app near-door claims 0.6–1.0 µs vs SAWS on-node 5.4–8 µs.
    **SAWS publish lag:** without a SHMEM call after release, the owner's loopback atomic_set took ~3.9 ms to land (510 rounds/2 s
    at every N, T); with shmem_quiet after release 72–170 µs rounds. Experiment switch `GTC_LAWS_FAR_SYNC=1` (policy option `-fs`).
  - E3.3 one node (e3a): near door −0.8..+1.2% on T3XXL (not steal-bound at 1 node); **BPC fine +0.3..+10.8%** depending on door
    policy (any/random small, nf +6.4, no +10.8; norf-nors +3.2). Cause: BPC's bouncing producer is created first → oldest → behind the
    far door; near-first/near-only reach it last. Node-mates never use far doors on T3XXL under nf.
- Builds: `build-xpmem-e3` (validated, used by e3a/e3p); `build-xpmem-e3b` (+ bench-claim, + far-sync switch, default off; used by
  e3f and e3s). Sources: `~/laws/src-e3`, `~/laws/src-e3b`. Don't rebuild a build dir while a job uses it. The code was committed on
  `laws` after these builds, so they report `4a0f06665c-dirty`: `build-xpmem-e3b` matches the commit "Two-door queue (LAWS M2, E3)";
  `build-xpmem-e3` predates the far-sync switch (default off) and the stress/bench test changes, with the queue otherwise identical.
  The main `~/laws/src` and `~/laws/build-xpmem` still hold the E2p state; rebuild them from the commit before new suites use them.

**State when paused (2026-11-20 ~15:30):** e3f (job 28568, 1 node: far-sync A/B, 35/42 runs done) and e3s (jobs 28569 running on
16 nodes, 19 files; 28570 pending on 32 nodes) were still running. To resume:
```
ssh lotus 'squeue -u brian; sacct -u brian -j 28568,28569,28570 -X -o JobID,JobName,State,Elapsed'
rsync -a lotus:laws/results/e3f/ /tmp/e3f/ && python3 runs/e0/parse.py /tmp/e3f --csv /tmp/e3f.csv
rsync -a lotus:laws/results/e3s/ /tmp/e3s/ && python3 runs/e0/parse.py /tmp/e3s --csv /tmp/e3s.csv
```
e3f question: does `-fs` (GTC_LAWS_FAR_SYNC=1) change runtime at ρ=1 or 0.5 on T3XXL/BPC fine? e3s question (E3.4): at 16/32 nodes,
does the near door beat laws50ep256/laws90ep256 on SAWS (and L at ρ=1 by proxy, see parity), which door policy wins, and does BPC
fine still lose (producer behind the far door)? The `laws2` line in each result file has the two-door counters (parse.py columns
rho..ring). Write both into design doc §7.8 (replace its "Pending" paragraph).

Next: e3f and e3s results → §7.8; then discuss with the PI: BPC/producer placement and door policy (e.g. near-first trying the same
target's far door after a near miss), the publish lag (make the far publish blocking?), which variants go into the ρ sweep, and the
maintenance-check interval (k=16 vs 256 for refill/re-split).
Small follow-ups: the bitmap over SDC as an ablation, and a calibrated busy-wait for sub-ms BPC tasks.

## Conventions
- Add experimental results to the design doc
  (`~/research/docs/laws-design/laws-directions.tex`) as they become meaningful, with
  job IDs, inputs, and limits. Rebuild the PDF and check the renders.
- Don't commit or push unless asked. Commits and PRs carry **no Claude attribution** (no
  co-author or session trailers). This is set in `.claude/settings.json` here and globally.
  Don't touch `locality_alt` or lotus `~/saws`.
- Terminology (PI, 2026-11-19): **initiator** and **target**, not thief and victim, in code comments,
  notes, and docs, to match one-sided communication. "Steal" stays. See Def. 2 of the design doc.
- When quoting measurements, give their source (probe, job, or results file) and limits.
- `-Werror` is on (`tc.mk`). Build on lotus; the Mac has no OpenSHMEM, so the
  clangd diagnostics about a missing `shmem.h` are expected.
