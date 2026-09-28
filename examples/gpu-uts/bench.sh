#!/usr/bin/env bash
# Benchmark gpu-uts V2: records shape, capacity, timing, and correctness in LOG.
#
#   ./bench.sh
#   TREES="T1 T3" WARPS="8" BLOCKS="0 1 2" REPS=3 ./bench.sh

set -euo pipefail
cd "$(dirname "$0")"
source ../uts/sample_trees.sh

TREES=${TREES:-"T1 T3 T5 T2"}
WARPS=${WARPS:-"8"}
BLOCKS=${BLOCKS:-"0 1 2"}
REPS=${REPS:-3}
WARMUP=${WARMUP:-1}
SEQ=${SEQ:-1}
LOG=${LOG:-bench.log}

printf "%-5s %-10s %11s %9s %6s %6s err cons\n" tree run tasks Mtasks/s lanes% error ok
for t in $TREES; do
  args=${!t}
  if [ "$SEQ" != 0 ]; then
    ./uts-seq $args | awk -v t="$t" '
      /Tree size/  { gsub(",", ""); tasks = $4 }
      /Wallclock/  { rate = $(NF - 5) }
      END { printf "%-5s %-10s %11d %9.2f\n", t, "cpu", tasks, rate / 1e6 }'
  fi
  for w in $WARPS; do
    for b in $BLOCKS; do
      for ((r = 0; r < WARMUP + REPS; ++r)); do
        out=$(./gpu-uts $args -W "$w" -B "$b")
        kv=$(echo "$out" | grep '^gputc_kv' || true)
        [ -n "$kv" ] || { echo "bench: missing gputc_kv for $t W=$w B=$b" >&2; exit 1; }
        if [ "$r" -ge "$WARMUP" ]; then
          echo "tree=$t $kv" >> "$LOG"
          echo "$kv" | awk -v t="$t" -v tag="W=${w}B=${b}" '{
            for (i = 2; i <= NF; ++i) { split($i, p, "="); v[p[1]] = p[2] }
            cons = (v["error"] == 0 && v["spawned"] == v["tasks"]) ? "ok" : "FAIL"
            printf "%-5s %-10s %11d %9.2f %6.1f %6d %s\n", t, tag,
              v["tasks"], v["tasks_per_s"] / 1e6, 100 * v["lane_fill"], v["error"], cons
          }'
        fi
      done
    done
  done
done
