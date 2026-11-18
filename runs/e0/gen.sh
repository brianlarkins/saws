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
#
# Every run gets a per-step time limit (srun --time, minutes; E0_STEP_MIN,
# default 15) so one runaway input cannot consume the rest of the job.
#
# One job per node count.  Each job runs every (input, SOS variant, queue)
# combination on the same allocation, interleaved within each repetition, so
# variants and queues see the same nodes and the same background load.
# Output: $E0_ROOT/results/<suite>/<input>.<variant>.<queue>.<npes>.<rep>.out
set -euo pipefail

suite=${1:?usage: gen.sh <smoke|small|scale|e0b> [nodes ...]}; shift
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
    echo "here=$here"
    echo "out=$outdir"
    echo
    echo "run() {  # run <label> <variant> <queue> <rep> <binary> <args...>"
    echo "  local label=\$1 v=\$2 q=\$3 rep=\$4 bin=\$5; shift 5"
    echo "  local f=\$out/\$label.\$v.\$q.$pnp.\$rep.out"
    echo "  source \$here/env.sh \$v || return"
    echo "  { echo \"# e0 suite=$suite input=\$label variant=\$v queue=\$q npes=$np nodes=$n rep=\$rep\""
    echo "    echo \"# job \$SLURM_JOB_ID nodelist \$SLURM_JOB_NODELIST start \$(date -Is)\""
    echo "    echo \"# cmd \$E0_BUILD/\$bin \$*\"; } > \$f"
    echo "  local t0=\$(date +%s.%N)"
    echo "  srun --mpi=pmix --cpu-bind=cores --time=${E0_STEP_MIN:-15} \$E0_BUILD/\$bin \"\$@\" >> \$f 2>&1"
    echo "  local rc=\$?"
    echo "  echo \"# exit \$rc wall \$(echo \"\$(date +%s.%N) - \$t0\" | bc)\" >> \$f"
    echo "}"
    echo
    for rep in $(seq -w 1 $reps); do
      while IFS='|' read -r label bin args; do
        for v in $variants; do
          for q in $queues; do
            # UTS selects the queue with "-Q B|H"; the others take "-B|-H"
            if [[ $bin == */uts-scioto ]]; then qflag="-Q $q"; else qflag="-$q"; fi
            echo "run $label $v $q $rep $bin $args $qflag"
          done
        done
      done < <(inputs $suite $n)
    done
  } > "$f"
  chmod +x "$f"
  echo "$f"
done
