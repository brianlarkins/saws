#!/usr/bin/env bash
# Run gpu-uts over sample trees and block sizes, one table row per run, with
# the sequential CPU reference (uts-seq) for comparison. Raw gputc_kv lines
# are appended to $LOG for later analysis.
#
#   ./bench.sh
#   TREES="T1 T3" WARPS="8 16 32" REPS=3 ./bench.sh
#
# Columns: throughput, lanes holding a task, then where warp time goes
# (task code, scheduler overhead, idle), then where chunks came from.

set -euo pipefail
cd "$(dirname "$0")"
source ../uts/sample_trees.sh

TREES=${TREES:-"T1 T3 T5 T2"}
WARPS=${WARPS:-"8"}
REPS=${REPS:-1}
SEQ=${SEQ:-1}
LOG=${LOG:-bench.log}

printf "%-5s %-6s %11s %9s %6s %6s %6s %6s %6s %6s\n" \
  tree run tasks Mtasks/s lanes% task% sched% idle% steal% glob%
for t in $TREES; do
  args=${!t}
  if [ "$SEQ" != 0 ]; then
    ./uts-seq $args | awk -v t="$t" '
      /Tree size/  { gsub(",", ""); tasks = $4 }
      /Wallclock/  { rate = $(NF - 5) }
      END { printf "%-5s %-6s %11d %9.2f\n", t, "cpu", tasks, rate / 1e6 }'
  fi
  for w in $WARPS; do
    for ((r = 0; r < REPS; ++r)); do
      kv=$(./gpu-uts $args -W "$w" | grep '^gputc_kv')
      echo "tree=$t $kv" >> "$LOG"
      echo "$kv" | awk -v t="$t" -v w="$w" '{
        for (i = 2; i <= NF; ++i) { split($i, p, "="); v[p[1]] = p[2] }
        sched = 1 - v["time_task"] - v["time_idle"]
        printf "%-5s %-6s %11d %9.2f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f\n", t, "W=" w,
          v["tasks"], v["tasks_per_s"] / 1e6, 100 * v["lane_fill"],
          100 * v["time_task"], 100 * sched, 100 * v["time_idle"],
          100 * v["sibling_steals"] / v["chunks"], 100 * v["global_pops"] / v["chunks"]
      }'
    done
  done
done
