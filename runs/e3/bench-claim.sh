#!/bin/bash
# runs/e3/bench-claim.sh -- E3.2 (H1): claim latency on one target, N node-mate initiators,
# two-door near door vs SAWS.  sbatch -N1 --ntasks-per-node=48 --exclusive runs/e3/bench-claim.sh
here=${SLURM_SUBMIT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}/runs/e0
source "$here/env.sh" xpmem >/dev/null || exit 1
echo "# e3.2 bench-claim: build $E0_BUILD ($(cat $E0_BUILD/libtc/buildinfo.h | tr '\n' ' '))"
echo "# job ${SLURM_JOB_ID:-none} nodes ${SLURM_JOB_NODELIST:-?} start $(date -Is)"
for args in "-t 1024 -p 1" "-t 1024 -p 0" "-t 64 -p 1"; do
  echo "## bench-claim $args"
  srun --mpi=pmix --cpu-bind=cores --time=5 $E0_BUILD/tests/bench-claim -d 2 $args 2>&1 | grep -v "^SCIOTO\|calibrated"
done
echo "# done $(date -Is)"
