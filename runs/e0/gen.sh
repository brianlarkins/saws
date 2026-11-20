#!/bin/bash
# runs/e0/gen.sh -- generate Slurm scripts for the E0 baselines.
#
#   gen.sh <suite> [nodes ...]
#
# Suites:
#   smoke : 1 rep, tiny inputs, plus test-tasktree{,-twotc}; checks the builds
#   small : 3 reps, UTS T1L/T1XL, BPC d=100 n=1024, MADNESS -t 10e-6
#   scale : 3 reps, UTS T1XXL (<= 8 nodes) and T1WL (>= 8 nodes),
#           BPC d=500 n=8192 bouncing (ICPP'21 config).  MADNESS (-t 10e-9) is
#           off the critical path for now (PI, 2026-11-18): large compute-heavy
#           tasks, little expected sensitivity to steal cost, possible bitrot.
#   e0b   : 2 reps, search for steal-bound regimes (E0 showed the ICPP'21 inputs
#           are not steal-bound at <= 32 nodes): UTS binomial trees (T3L, T3XXL,
#           T3WL), UTS geometric trees strong-scaled to little work per PE
#           (T1XL/T1XXL at >= 16 nodes), and fine-grained BPC (0.1 ms producers,
#           0.5 or 0.1 ms consumers; needs the bpc.c -p/-c fix).
#   e1    : 2 reps, the accepted LAWS reference set (UTS T3XXL, T3WL, BPC fine;
#           controls UTS T1WL, BPC d500) under target policies: SAWS with random,
#           local-prob 50% and 90%, and local-sweep; SDC with random.  XPMEM build.
#   e2    : 2 reps, the reference set with SAWS: random, lp50 (E1's best), lp50+peeks,
#           laws M1 (node bitmap + peeks, blind off-node), laws (bitmap + peeks +
#           remote bitmaps, E2b), and laws without peeks.  XPMEM build.
#   e2c   : 2 reps, 4-40 nodes, the reference set with SAWS: lp50pk (the bar), and laws
#           (bitmap + peeks + remote bitmaps) x {strict local-first, 90% and 50% of
#           steps via the node bitmap} x {lazy, eager bit clear}.  E2 found strict
#           laws recirculates work inside a node on binomial UTS (like E1's sweep).
#   e2p   : 3 reps, 16 and 32 nodes, the reference set: lp50pk, laws50e and laws90e at the default
#           lazy poll (every 16 progress calls) and sparser polls (256, 4096, 65536).  E2c found
#           every laws variant +0.4-1.1% on T1WL, likely the per-task lazy poll; with eager
#           clearing, a much sparser poll may suffice.
#   e3a   : 3 reps, 1 node (E3.0 parity and E3.3): UTS T3XXL and BPC fine; SAWS (H) vs the
#           two-door queue (L) at rho = 1 under random and laws50e, then L at rho 0.5 under random
#           and laws50e with each door policy, rho 0.25/0.75, a sparser maintenance check (p256),
#           and refill and re-split off.
#   e3p   : 3 reps, 16 nodes (E3.0 parity at scale): UTS T3XXL and T1WL; H vs L at rho = 1 under
#           random and laws50e.
#   e3f   : 3 reps, 1 node: does the far word's publish lag matter in applications?  T3XXL and BPC
#           fine; SAWS random, and L at rho 1 and 0.5 with and without GTC_LAWS_FAR_SYNC.
#   e3s   : 2 reps, 16 and 32 nodes (E3.4 screening): the steal-bound inputs; laws50e and laws90e
#           on H, and on L each door policy x {laws50e, laws90e} at rho 0.5 (48 runs per node count);
#           all with the e2p check interval k = 256.
#
# Target policies (set through the environment; see gtc_ldbal_cfg_env):
#   random  GTC_TARGET=random          lpNN     GTC_TARGET=local-prob GTC_LOCAL_PCT=NN
#   sweep   GTC_TARGET=local-sweep     sweepNN  ... with GTC_SWEEP_FAR=NN
#   lpNNpk  local-prob NN with GTC_NEAR_PEEK=1
#   laws    GTC_TARGET=laws GTC_NEAR_PEEK=1 GTC_FAR_DIR=1   (bitmap + peeks + remote bitmaps)
#   lawsm1  GTC_TARGET=laws GTC_NEAR_PEEK=1 GTC_FAR_DIR=0   (bitmap + peeks, blind off-node: M1)
#   lawsnp  GTC_TARGET=laws GTC_NEAR_PEEK=0 GTC_FAR_DIR=1   (bitmap + remote bitmaps, no peeks)
#   laws[NN][e][pMM]  laws with GTC_LAWS_NEAR_PCT=NN (default 100), with "e" GTC_LAWS_EAGER=1,
#                and with "pMM" GTC_LAWS_POLL=MM (default 16) (e.g. laws50, laws90e, lawse, laws50ep4096)
# Two-door queue (queue L) options, appended to any policy with "-":
#   -nf | -no | -any   GTC_LAWS_DOORS=near-first | near-only | any
#   -rNN               GTC_LAWS_RHO=NN/100 (r100 = SAWS's split)
#   -norf | -nors      GTC_LAWS_REFILL=0 | GTC_LAWS_RESPLIT=0
#   -fs                GTC_LAWS_FAR_SYNC=1 (experiment: shmem_quiet after each far publish)
#   e.g. laws50e-nf-r50, random-r100, laws90e-any-r50-norf-nors
#
# Every run gets a per-step time limit (srun --time, minutes; E0_STEP_MIN,
# default 15) so one runaway input cannot consume the rest of the job.
#
# One job per node count.  Each job runs every (input, SOS variant, queue)
# combination on the same allocation, interleaved within each repetition, so
# variants and queues see the same nodes and the same background load.
# Output: $E0_ROOT/results/<suite>/<input>.<variant>.<queue>.<npes>.<rep>.out
set -euo pipefail

suite=${1:?usage: gen.sh <smoke|small|scale|e0b|e1|e2|e2c|e2p|e3a|e3p|e3f|e3s> [nodes ...]}; shift
E0_ROOT=${E0_ROOT:-$HOME/laws}
here=$(cd "$(dirname "$0")" && pwd)
ppn=48
variants="xpmem main"
queues="B H"   # B = SDC, H = SAWS

# UTS trees (examples/uts/sample_trees.sh)
T1L="-t 1 -a 3 -d 13 -b 4 -r 29"
T1XL="-t 1 -a 3 -d 15 -b 4 -r 29"
T1XXL="-t 1 -a 3 -d 15 -b 4 -r 19"
T1WL="-t 1 -a 3 -d 18 -b 4 -r 19"
T3L="-t 0 -b 2000 -q 0.200014 -m 5 -r 7"        # binomial, 111M nodes, depth 17844
T3XXL="-t 0 -b 2000 -q 0.499995 -m 2 -r 316"    # binomial, 2.8B nodes
T3WL="-t 0 -b 2000 -q 0.4999995 -m 2 -r 559"    # binomial, 157B nodes, depth 758577

case $suite in
  smoke) reps=1; default_nodes="1 2" ;;
  small) reps=3; default_nodes="1 2 4" ;;
  scale) reps=3; default_nodes="1 2 4 8 16 32" ;;
  e0b)   reps=2; default_nodes="1 4 16 32" ;;
  e1)    reps=2; default_nodes="1 4 16 32 40" ;;
  e2)    reps=2; default_nodes="1 4 16 32 40" ;;
  e2c)   reps=2; default_nodes="4 16 32 40" ;;
  e2p)   reps=3; default_nodes="16 32" ;;
  e3a)   reps=3; default_nodes="1" ;;
  e3p)   reps=3; default_nodes="16" ;;
  e3f)   reps=3; default_nodes="1" ;;
  e3s)   reps=2; default_nodes="16 32" ;;
  *) echo "unknown suite $suite" >&2; exit 1 ;;
esac
nodes=${*:-$default_nodes}

# inputs for a suite at a node count: lines of "label|binary|args" (binary relative to build dir)
inputs() {
  local s=$1 n=$2
  case $s in
    smoke)
      echo "uts-T1L|examples/uts/uts-scioto|$T1L"
      echo "bpc-d20|examples/bpc/bpc|-d 20 -n 1024 -b"
      echo "mad-1e-6|examples/madness/mad3d|-t 10e-6"
      echo "tasktree|tests/test-tasktree|"
      echo "tasktree2tc|tests/test-tasktree-twotc|"
      ;;
    small)
      echo "uts-T1L|examples/uts/uts-scioto|$T1L"
      echo "uts-T1XL|examples/uts/uts-scioto|$T1XL"
      echo "bpc-d100|examples/bpc/bpc|-d 100 -n 1024 -b"
      echo "mad-1e-6|examples/madness/mad3d|-t 10e-6"
      ;;
    scale)
      if (( n <= 8 )); then echo "uts-T1XXL|examples/uts/uts-scioto|$T1XXL"; fi
      if (( n >= 8 )); then echo "uts-T1WL|examples/uts/uts-scioto|$T1WL"; fi
      echo "bpc-d500|examples/bpc/bpc|-d 500 -n 8192 -b"
      if [[ ${E0_MADNESS:-0} == 1 ]]; then echo "mad-1e-9|examples/madness/mad3d|-t 10e-9"; fi
      ;;
    e0b)
      if (( n <= 4 ));  then echo "uts-T3L|examples/uts/uts-scioto|$T3L"; fi
      echo "uts-T3XXL|examples/uts/uts-scioto|$T3XXL"
      if (( n >= 16 )); then echo "uts-T3WL|examples/uts/uts-scioto|$T3WL"; fi
      if (( n >= 16 )); then echo "uts-T1XL|examples/uts/uts-scioto|$T1XL"; fi
      if (( n >= 16 )); then echo "uts-T1XXL|examples/uts/uts-scioto|$T1XXL"; fi
      echo "bpc-fine|examples/bpc/bpc|-d 2000 -n 1024 -p 0.1 -c 0.5 -b"
      if (( n <= 16 )); then echo "bpc-c01|examples/bpc/bpc|-d 500 -n 8192 -p 0.1 -c 0.1 -b"; fi
      ;;
    e3a|e3f)
      echo "uts-T3XXL|examples/uts/uts-scioto|$T3XXL"
      echo "bpc-fine|examples/bpc/bpc|-d 2000 -n 1024 -p 0.1 -c 0.5 -b"
      ;;
    e3p)
      echo "uts-T3XXL|examples/uts/uts-scioto|$T3XXL"
      echo "uts-T1WL|examples/uts/uts-scioto|$T1WL"
      ;;
    e3s)
      echo "uts-T3XXL|examples/uts/uts-scioto|$T3XXL"
      echo "uts-T3WL|examples/uts/uts-scioto|$T3WL"
      echo "bpc-fine|examples/bpc/bpc|-d 2000 -n 1024 -p 0.1 -c 0.5 -b"
      ;;
    e1|e2|e2c|e2p)
      echo "uts-T3XXL|examples/uts/uts-scioto|$T3XXL"
      if (( n >= 16 )); then echo "uts-T3WL|examples/uts/uts-scioto|$T3WL"; fi
      echo "bpc-fine|examples/bpc/bpc|-d 2000 -n 1024 -p 0.1 -c 0.5 -b"
      if (( n >= 16 )); then echo "uts-T1WL|examples/uts/uts-scioto|$T1WL"; fi
      if (( n >= 4 ));  then echo "bpc-d500|examples/bpc/bpc|-d 500 -n 8192 -b"; fi
      ;;
  esac
}

# (variant queue policy) combinations run for every input
combos() {
  case $1 in
    e1) echo "xpmem H random"; echo "xpmem H lp50"; echo "xpmem H lp90"
        echo "xpmem H sweep";  echo "xpmem B random" ;;
    e2) echo "xpmem H random"; echo "xpmem H lp50"; echo "xpmem H lp50pk"
        echo "xpmem H lawsm1"; echo "xpmem H laws";  echo "xpmem H lawsnp" ;;
    e2c) echo "xpmem H lp50pk"
         for pol in laws lawse laws90 laws90e laws50 laws50e; do echo "xpmem H $pol"; done ;;
    e2p) echo "xpmem H lp50pk"
         for pol in laws50e laws50ep256 laws50ep4096 laws50ep65536 laws90e laws90ep4096; do echo "xpmem H $pol"; done ;;
    e3a) echo "xpmem H random"; echo "xpmem L random-r100"; echo "xpmem H laws50e"; echo "xpmem L laws50e-nf-r100"
         for pol in random-r50 laws50e-nf-r50 laws50e-no-r50 laws50e-any-r50 laws50e-nf-r25 laws50e-nf-r75 \
                    laws50ep256-nf-r50 laws50e-nf-r50-norf-nors; do echo "xpmem L $pol"; done ;;
    e3f) echo "xpmem H random"
         for pol in random-r100 random-r100-fs laws50ep256-nf-r100 laws50ep256-nf-r100-fs laws50ep256-nf-r50 laws50ep256-nf-r50-fs; do
           echo "xpmem L $pol"; done ;;
    e3p) echo "xpmem H random"; echo "xpmem L random-r100"; echo "xpmem H laws50e"; echo "xpmem L laws50e-nf-r100" ;;
    e3s) echo "xpmem H laws50ep256"; echo "xpmem H laws90ep256"   # k = 256 from e2p
         for p in laws50ep256 laws90ep256; do for d in nf no any; do echo "xpmem L $p-$d-r50"; done; done ;;
    *)  for v in $variants; do for q in $queues; do echo "$v $q random"; done; done ;;
  esac
}

# wall-clock limit per job (generous; estimates from earlier lotus runs)
walltime() {
  local s=$1 n=$2
  case $s in
    smoke) echo "00:30:00" ;;
    small) echo "01:30:00" ;;
    scale) case $n in 1) echo "04:00:00";; 2) echo "02:30:00";; 4) echo "02:00:00";;
                      8) echo "02:30:00";; 16) echo "01:30:00";; *) echo "01:00:00";; esac ;;
    e0b)   case $n in 1) echo "01:30:00";; 4) echo "01:00:00";; *) echo "01:00:00";; esac ;;
    e1)    case $n in 1) echo "00:45:00";; 4) echo "01:15:00";; 16) echo "01:30:00";; *) echo "01:00:00";; esac ;;
    e2)    case $n in 1) echo "00:45:00";; 4) echo "01:30:00";; 16) echo "01:45:00";; *) echo "01:15:00";; esac ;;
    e2c)   case $n in 4) echo "01:30:00";; 16) echo "01:45:00";; *) echo "01:45:00";; esac ;;
    e2p)   case $n in 16) echo "03:00:00";; *) echo "02:15:00";; esac ;;
    e3a)   echo "01:45:00" ;;
    e3p)   echo "01:00:00" ;;
    e3f)   echo "01:00:00" ;;
    e3s)   echo "01:00:00" ;;
  esac
}

jobdir=$E0_ROOT/jobs/$suite
outdir=$E0_ROOT/results/$suite
mkdir -p "$jobdir" "$outdir"

for n in $nodes; do
  np=$(( n * ppn ))
  pnp=$(printf "%04d" $np)
  f=$jobdir/e0-$suite-$(printf "%02d" $n).sh
  {
    echo "#!/bin/bash"
    echo "#SBATCH -J e0-$suite-$n"
    echo "#SBATCH -N $n"
    echo "#SBATCH --ntasks-per-node=$ppn"
    echo "#SBATCH --exclusive"
    echo "#SBATCH -t $(walltime $suite $n)"
    echo "#SBATCH -o $outdir/slurm-%j.out"
    echo
    echo "export SCIOTO_DISABLE_PERNODE_STATS=1"
    echo "export E0_BUILD_SUFFIX=${E0_BUILD_SUFFIX:-}   # env.sh: build-<variant><suffix>"
    echo "here=$here"
    echo "out=$outdir"
    echo
    echo "run() {  # run <label> <variant> <policy> <queue> <rep> <binary> <args...>"
    echo "  local label=\$1 v=\$2 pol=\$3 q=\$4 rep=\$5 bin=\$6; shift 6"
    echo "  local f=\$out/\$label.\$v.\$pol.\$q.$pnp.\$rep.out envs o"
    echo "  local base=\${pol%%-*} opts=\${pol#\${pol%%-*}}   # two-door options follow the policy after '-'"
    echo "  case \$base in"
    echo "    random)  envs=\"GTC_TARGET=random\" ;;"
    echo "    lp*pk)   envs=\"GTC_TARGET=local-prob GTC_NEAR_PEEK=1 GTC_LOCAL_PCT=\$(echo \${base#lp} | sed s/pk//)\" ;;"
    echo "    lp*)     envs=\"GTC_TARGET=local-prob GTC_LOCAL_PCT=\${base#lp}\" ;;"
    echo "    laws)    envs=\"GTC_TARGET=laws GTC_NEAR_PEEK=1 GTC_FAR_DIR=1\" ;;"
    echo "    lawsm1)  envs=\"GTC_TARGET=laws GTC_NEAR_PEEK=1 GTC_FAR_DIR=0\" ;;"
    echo "    lawsnp)  envs=\"GTC_TARGET=laws GTC_NEAR_PEEK=0 GTC_FAR_DIR=1\" ;;"
    echo "    laws*)   # laws[NN][e][pMM]: NN = GTC_LAWS_NEAR_PCT (default 100), e = GTC_LAWS_EAGER=1, MM = GTC_LAWS_POLL"
    echo "             local x=\${base#laws} e=0 p=16"
    echo "             if [[ \$x =~ ^(.*)p([0-9]+)\$ ]]; then x=\${BASH_REMATCH[1]}; p=\${BASH_REMATCH[2]}; fi"
    echo "             if [[ \$x == *e ]]; then e=1; x=\${x%e}; fi"
    echo "             if [[ -n \$x && ! \$x =~ ^[0-9]+\$ ]]; then echo \"unknown policy \$pol\" >&2; return 1; fi"
    echo "             envs=\"GTC_TARGET=laws GTC_NEAR_PEEK=1 GTC_FAR_DIR=1 GTC_LAWS_NEAR_PCT=\${x:-100} GTC_LAWS_EAGER=\$e GTC_LAWS_POLL=\$p\" ;;"
    echo "    sweep)   envs=\"GTC_TARGET=local-sweep\" ;;"
    echo "    sweep*)  envs=\"GTC_TARGET=local-sweep GTC_SWEEP_FAR=\${base#sweep}\" ;;"
    echo "    *)       echo \"unknown policy \$pol\" >&2; return 1 ;;"
    echo "  esac"
    echo "  for o in \${opts//-/ }; do"
    echo "    case \$o in"
    echo "      nf)      envs=\"\$envs GTC_LAWS_DOORS=near-first\" ;;"
    echo "      no)      envs=\"\$envs GTC_LAWS_DOORS=near-only\" ;;"
    echo "      any)     envs=\"\$envs GTC_LAWS_DOORS=any\" ;;"
    echo "      r[0-9]*) envs=\"\$envs GTC_LAWS_RHO=\$(echo \"scale=3; \${o#r}/100\" | bc)\" ;;"
    echo "      norf)    envs=\"\$envs GTC_LAWS_REFILL=0\" ;;"
    echo "      nors)    envs=\"\$envs GTC_LAWS_RESPLIT=0\" ;;"
    echo "      fs)      envs=\"\$envs GTC_LAWS_FAR_SYNC=1\" ;;"
    echo "      *)       echo \"unknown policy option \$o in \$pol\" >&2; return 1 ;;"
    echo "    esac"
    echo "  done"
    echo "  source \$here/env.sh \$v || return"
    echo "  { echo \"# e0 suite=$suite input=\$label variant=\$v policy=\$pol queue=\$q npes=$np nodes=$n rep=\$rep\""
    echo "    echo \"# job \$SLURM_JOB_ID nodelist \$SLURM_JOB_NODELIST start \$(date -Is)\""
    echo "    echo \"# env \$envs\""
    echo "    echo \"# cmd \$E0_BUILD/\$bin \$*\"; } > \$f"
    echo "  local t0=\$(date +%s.%N) rc try"
    echo "  for try in 1 2; do"
    echo "    # SOS occasionally hangs in start-up before gtc_init prints (rare with block placement):"
    echo "    # if no SCIOTO build line appears within E0_HANG_SEC (90) s, kill the step and retry once."
    echo "    env \$envs srun --mpi=pmix --cpu-bind=cores --time=${E0_STEP_MIN:-15} \$E0_BUILD/\$bin \"\$@\" > \$f.run 2>&1 &"
    echo "    local pid=\$! waited=0 hung=0 checked=0"
    echo "    while kill -0 \$pid 2>/dev/null; do"
    echo "      sleep 2; waited=\$((waited+2))"
    echo "      if (( !checked && waited >= ${E0_HANG_SEC:-90} )); then"
    echo "        checked=1; if ! grep -q 'SCIOTO build' \$f.run; then hung=1; kill \$pid; fi"
    echo "      fi"
    echo "    done"
    echo "    wait \$pid; rc=\$?"
    echo "    cat \$f.run >> \$f; rm -f \$f.run"
    echo "    if (( hung )); then echo \"# start-up hang on try \$try; retrying\" >> \$f; sleep 5; continue; fi"
    echo "    break"
    echo "  done"
    echo "  echo \"# exit \$rc wall \$(echo \"\$(date +%s.%N) - \$t0\" | bc)\" >> \$f"
    echo "}"
    echo
    for rep in $(seq -w 1 $reps); do
      while IFS='|' read -r label bin args; do
        while read -r v q pol; do
          # UTS selects the queue with "-Q B|H|L"; the others take "-B|-H|-L"
          if [[ $bin == */uts-scioto ]]; then qflag="-Q $q"; else qflag="-$q"; fi
          echo "run $label $v $pol $q $rep $bin $args $qflag"
        done < <(combos $suite)
      done < <(inputs $suite $n)
    done
  } > "$f"
  chmod +x "$f"
  echo "$f"
done
