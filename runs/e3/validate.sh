#!/bin/bash
# runs/e3/validate.sh -- E3.0/E3.1 validation of the two-door queue on the current allocation.
#
#   sbatch -N <1|2> --ntasks-per-node=48 --exclusive -t 01:00:00 runs/e3/validate.sh [quick]
#
# Uses $E0_ROOT/build-xpmem$E0_BUILD_SUFFIX.  Every run sets GTC_LAWS_CHECK=1 (the queue aborts on a
# broken index or epoch invariant).  One line per run: PASS/FAIL, seconds, settings; failures keep
# their output in $out/<n>.log.
#   1. test-laws-shrb (scripted unit test + randomized stress, exactly-once check per task id):
#      rho {0.25, 0.5, 0.75} x refill/re-split {11, 00, 10, 01} under random targets, and the same
#      x door policy {near-first, near-only, any} under laws (eager, near share 50); rho = 1 once.
#      Each setting runs twice: every PE creating tasks, and only every 12th PE creating them (so
#      the rest contend for a few owners' doors).
#   2. tree tests (-L) and test-laws -L under random, lp50pk, laws50e x rho {0.25, 0.5, 0.75, 1},
#      and the three door policies at rho 0.5.
set -u
here=${SLURM_SUBMIT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}/runs/e0   # sbatch runs a copy of this script; submit from the source tree
set +u
source "$here/env.sh" xpmem >/dev/null || exit 1
set -u
bin=$E0_BUILD
out=${E3_OUT:-$E0_ROOT/results/e3-validate/${SLURM_JOB_ID:-local}}
mkdir -p "$out"
nodes=${SLURM_JOB_NUM_NODES:-1}
quick=${1:-}
n=0 fails=0

run() {  # run <label> <envs> <binary and args...>
  local label=$1 envs=$2; shift 2
  n=$((n+1))
  local log=$out/$n.log t0=$(date +%s.%N) ok
  env GTC_LAWS_CHECK=1 SCIOTO_DISABLE_PERNODE_STATS=1 $envs \
    timeout 300 srun --mpi=pmix --cpu-bind=cores --time=5 "$@" > "$log" 2>&1
  local rc=$?
  if   grep -q "LAWS_SHRB test: SUCCESS\|LAWS test: SUCCESS" "$log"; then ok=PASS
  elif grep -q "Tree test: SUCCESS\|SUCCESS" "$log" && [ $rc -eq 0 ]; then ok=PASS
  else ok=FAIL; fi
  [ $rc -ne 0 ] && ok=FAIL
  printf "%-4s %6.1fs rc=%d  %-22s %s\n" "$ok" "$(echo "$(date +%s.%N) - $t0" | bc)" $rc "$label" "$envs"
  if [ $ok = PASS ]; then
    grep -h "^stress:\|^unit:" "$log" | sed 's/^/        /'
    rm -f "$log"
  else
    fails=$((fails+1)); tail -5 "$log" | sed 's/^/        | /'
  fi
}

echo "# e3 validate: $nodes node(s), build $bin ($(cat $bin/libtc/buildinfo.h | tr '\n' ' '))"
echo "# job ${SLURM_JOB_ID:-none} nodes ${SLURM_JOB_NODELIST:-?} start $(date -Is)"

laws50e="GTC_TARGET=laws GTC_NEAR_PEEK=1 GTC_FAR_DIR=1 GTC_LAWS_NEAR_PCT=50 GTC_LAWS_EAGER=1"
stress="$bin/tests/test-laws-shrb -n 4000 -q 64 -s 60"
stress_c="$bin/tests/test-laws-shrb -n 16000 -c 12 -q 256 -s 90"
rhos="0.25 0.5 0.75"
[ "$quick" = quick ] && rhos=0.5

# 1. queue stress
run "unit+stress q256" "GTC_LAWS_RHO=0.5" $bin/tests/test-laws-shrb -n 4000 -q 256 -s 60
for rho in $rhos; do
  for rr in 11 00 10 01; do
    rf=${rr:0:1} rs=${rr:1:1}
    for st in "$stress" "$stress_c"; do
      run "stress random" "GTC_TARGET=random GTC_LAWS_RHO=$rho GTC_LAWS_REFILL=$rf GTC_LAWS_RESPLIT=$rs GTC_LAWS_POLL=1" $st
      for d in near-first near-only any; do
        run "stress laws50e" "$laws50e GTC_LAWS_DOORS=$d GTC_LAWS_RHO=$rho GTC_LAWS_REFILL=$rf GTC_LAWS_RESPLIT=$rs GTC_LAWS_POLL=1" $st
      done
    done
  done
done
run "stress rho=1" "GTC_TARGET=random GTC_LAWS_RHO=1" $stress
run "stress rho=1 c12" "GTC_TARGET=random GTC_LAWS_RHO=1" $stress_c

# 2. tree tests and test-laws on the two-door queue
for rho in $rhos 1; do
  for pol in random lp50pk laws50e; do
    case $pol in
      random)  e="GTC_TARGET=random" ;;
      lp50pk)  e="GTC_TARGET=local-prob GTC_NEAR_PEEK=1 GTC_LOCAL_PCT=50" ;;
      laws50e) e="$laws50e" ;;
    esac
    run "tasktree -L $pol"   "$e GTC_LAWS_RHO=$rho" $bin/tests/test-tasktree -L
    run "twotc -L $pol"      "$e GTC_LAWS_RHO=$rho" $bin/tests/test-tasktree-twotc -L
  done
  run "test-laws -L"         "GTC_LAWS_RHO=$rho" $bin/tests/test-laws -L
done
for d in near-only any; do
  run "tasktree -L laws50e" "$laws50e GTC_LAWS_DOORS=$d GTC_LAWS_RHO=0.5" $bin/tests/test-tasktree -L
  run "test-laws -L"        "GTC_LAWS_DOORS=$d GTC_LAWS_RHO=0.5" $bin/tests/test-laws -L
done
# SAWS unchanged
run "tasktree -H laws50e" "$laws50e" $bin/tests/test-tasktree -H
run "test-laws (SAWS)"    "" $bin/tests/test-laws

echo "# done $(date -Is): $n runs, $fails failed"
