#!/usr/bin/env bash
# Benchmark gpu-uts V2: records shape, capacity, timing, and correctness in LOG.

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

printf "%-5s %-10s %11s %9s %6s %6s %s\n" tree run tasks Mtasks/s lanes% error ok
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
        set +e
        out=$(./gpu-uts $args -W "$w" -B "$b" 2>&1)
        status=$?
        set -e
        kv=$(echo "$out" | grep '^gputc_kv' || true)
        if [ "$status" -ne 0 ] || [ -z "$kv" ]; then
          echo "bench: FAIL tree=$t W=$w B=$b exit=$status" >> "$LOG"
          echo "$out" >> "$LOG"
          printf "%-5s %-10s %11s %9s %6s %6s %s\n" "$t" "W=${w}B=${b}" "-" "-" "-" "$status" "FAIL"
          continue
        fi
        if [ "$r" -ge "$WARMUP" ]; then
          echo "tree=$t exit=0 $kv" >> "$LOG"
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
