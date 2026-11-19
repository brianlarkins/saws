#!/usr/bin/env python3
"""Parse E0 result files into a CSV and a median summary.

    parse.py <results dir> [--csv out.csv]

Each result file is <input>.<variant>.<B|H>.<npes>.<rep>.out as written by
runs/e0/gen.sh.  Metrics come from the libtc stats block (common to all
apps), so UTS, BPC, and MADNESS are directly comparable.
"""
import argparse
import csv
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

PATTERNS = {
    # libtc summary (gtc_print_stats)
    "process_s":   r": process time ([\d.]+) s, passive time ([\d.]+) s \(([\d.]+)%\), search time ([\d.]+) ms",
    "tasks":       r": tasks completed (\d+), ([\d.]+) tasks/sec",
    "dispersion":  r": dispersion +([\d.]+)ms/ *([\d.]+)ms/ *([\d.]+)ms",
    "imbalance":   r": imbalance +([\d.]+)ms/ *([\d.]+)ms/ *([\d.]+)ms +termination attempts: (\d+)",
    "stealops":    r": steal ops +ok (\d+) avg ([\d.]+) us, failed (\d+) avg ([\d.]+) us",
    "scope":       (r": steal scope near ok (\d+) avg ([\d.]+) us fail (\d+) avg ([\d.]+) us tasks (\d+) probes (\d+)"
                    r" \| far ok (\d+) avg ([\d.]+) us fail (\d+) avg ([\d.]+) us tasks (\d+) probes (\d+) \| target (\S+)"),
    "laws":        (r": laws bitmap reads (\d+) empty (\d+) \| peeks (\d+) miss (\d+) \| fardir reads (\d+) empty (\d+)"
                    r" \| blind (\d+) \| bit writes (\d+) \| occ% ((?:\d+ ?){11})"),
    "laws_e2c":    r"\| near skip (\d+) \| eager clears (\d+)",          # E2c and later
    "laws_dry":    r"\| dry with bit (\d+) stealable (\d+)",
    # queue gstats (both queues print these lines)
    "release":     r": release +([\d.]+)/ *\d+/ *\d+ time .*? per +([\d.]+)us",
    "reacquire":   r": reacquire +([\d.]+)/ *\d+/ *\d+ time .*? per +([\d.]+)ms",
    "fails_un":    r":   fails un +(\d+)",
    "fails_lock":  r":   fails lock +(\d+)",
    # provenance
    "build":       r"SCIOTO build : libtc (\S+), compiled against (\S+)",
    "onnode":      r"SCIOTO onnode: SHMEM_TEAM_SHARED (\d+) PEs, shmem_ptr (\w+)",
    "exit":        r"^# exit (-?\d+) wall ([\d.]+)",
    # app-specific checks
    "uts_size":    r"Tree size = (\d+)",
    "bpc_ok":      r"Total tasks processed = +\d+, expected = +\d+: (\w+)",
}

FIELDS = ["file", "suite", "input", "variant", "policy", "queue", "npes", "nodes", "rep", "exit", "wall_s",
          "process_s", "passive_s", "passive_pct", "search_ms", "tasks", "tasks_per_s",
          "disp_avg_ms", "disp_max_ms", "imb_avg_ms", "imb_max_ms", "td_attempts",
          "steal_ok", "steal_ok_us", "steal_fail", "steal_fail_us",
          "release_calls", "release_us", "reacquire_calls", "reacquire_ms",
          "fails_un", "fails_lock",
          "near_ok", "near_ok_us", "near_fail", "near_fail_us", "near_tasks", "near_probes",
          "far_ok", "far_ok_us", "far_fail", "far_fail_us", "far_tasks", "far_probes", "target",
          "bm_reads", "bm_empty", "peeks", "peek_miss", "fd_reads", "fd_empty", "blind", "bit_writes", "occ", "near_skip", "eager_clears", "dry_bit", "dry_stealable",
          "libtc_rev", "shmem_prefix", "shared_team", "shmem_ptr",
          "check"]


def parse_file(path):
    text = path.read_text(errors="replace")
    row = {"file": path.name}
    m = re.search(r"^# e0 (.*)$", text, re.M)
    if not m:
        return None
    for kv in m.group(1).split():
        k, v = kv.split("=", 1)
        row[k] = v
    g = {k: re.search(p, text, re.M) for k, p in PATTERNS.items()}
    if g["exit"]:
        row["exit"], row["wall_s"] = g["exit"].groups()
    if g["process_s"]:
        row["process_s"], row["passive_s"], row["passive_pct"], row["search_ms"] = g["process_s"].groups()
    if g["tasks"]:
        row["tasks"], row["tasks_per_s"] = g["tasks"].groups()
    if g["dispersion"]:
        row["disp_avg_ms"], _, row["disp_max_ms"] = g["dispersion"].groups()
    if g["imbalance"]:
        row["imb_avg_ms"], _, row["imb_max_ms"], row["td_attempts"] = g["imbalance"].groups()
    if g["stealops"]:
        row["steal_ok"], row["steal_ok_us"], row["steal_fail"], row["steal_fail_us"] = g["stealops"].groups()
    if g["release"]:
        row["release_calls"], row["release_us"] = g["release"].groups()
    if g["reacquire"]:
        row["reacquire_calls"], row["reacquire_ms"] = g["reacquire"].groups()
    if g["scope"]:
        (row["near_ok"], row["near_ok_us"], row["near_fail"], row["near_fail_us"], row["near_tasks"],
         row["near_probes"], row["far_ok"], row["far_ok_us"], row["far_fail"], row["far_fail_us"],
         row["far_tasks"], row["far_probes"], row["target"]) = g["scope"].groups()
    if g["laws"]:
        (row["bm_reads"], row["bm_empty"], row["peeks"], row["peek_miss"], row["fd_reads"], row["fd_empty"],
         row["blind"], row["bit_writes"], occ) = g["laws"].groups()
        row["occ"] = " ".join(occ.split())
    if g["laws_e2c"]:
        row["near_skip"], row["eager_clears"] = g["laws_e2c"].groups()
    if g["laws_dry"]:
        row["dry_bit"], row["dry_stealable"] = g["laws_dry"].groups()
    row.setdefault("policy", "random")   # runs before E1 had no policy dimension
    if g["fails_un"]:
        row["fails_un"] = g["fails_un"].group(1)
    if g["fails_lock"]:
        row["fails_lock"] = g["fails_lock"].group(1)
    if g["build"]:
        row["libtc_rev"], row["shmem_prefix"] = g["build"].groups()
    if g["onnode"]:
        row["shared_team"], row["shmem_ptr"] = g["onnode"].groups()
    # correctness checks where the app reports them
    if g["bpc_ok"]:
        row["check"] = g["bpc_ok"].group(1)
    elif g["uts_size"]:
        row["check"] = "size=" + g["uts_size"].group(1)
    return row


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results", type=Path)
    ap.add_argument("--csv", type=Path)
    args = ap.parse_args()

    rows = [r for f in sorted(args.results.glob("*.out")) if (r := parse_file(f))]
    if not rows:
        sys.exit(f"no E0 result files in {args.results}")

    if args.csv:
        with args.csv.open("w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=FIELDS, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)

    bad = [r["file"] for r in rows
           if r.get("exit") != "0" or ("process_s" not in r and r.get("check") != "SUCCESS")
           or r.get("check") == "FAILURE"]
    groups = defaultdict(list)
    for r in rows:
        ok, nok = num(r.get("steal_ok")), num(r.get("near_ok"))
        if ok and nok is not None:
            r["near_pct"] = f"{100.0 * nok / ok:.1f}"
        fail = num(r.get("steal_fail"))
        if fail is not None and num(r.get("npes")):
            r["fail_per_pe"] = f"{fail / num(r['npes']):.0f}"
        groups[(r["input"], int(r["npes"]), r["variant"], r["policy"], r["queue"])].append(r)

    cols = [("process_s", "proc s"), ("passive_pct", "pass%"), ("near_pct", "near%"),
            ("near_ok_us", "nok us"), ("far_ok_us", "fok us"), ("steal_fail_us", "fail us"),
            ("fail_per_pe", "fails/PE"), ("td_attempts", "td")]
    hdr = (f"{'input':<12} {'npes':>5} {'var':<5} {'policy':<7} {'q':<4} {'n':>2} "
           + " ".join(f"{c[1]:>9}" for c in cols))
    print(hdr)
    print("-" * len(hdr))
    for key in sorted(groups):
        rs = groups[key]
        meds = []
        for c, _ in cols:
            vals = [v for r in rs if (v := num(r.get(c))) is not None]
            meds.append(f"{statistics.median(vals):9.3f}" if vals else f"{'-':>9}")
        q = "SDC" if key[4] == "B" else "SAWS"
        print(f"{key[0]:<12} {key[1]:>5} {key[2]:<5} {key[3]:<7} {q:<4} {len(rs):>2} " + " ".join(meds))
    if bad:
        print("\nincomplete or failed runs:", *bad, sep="\n  ")


if __name__ == "__main__":
    main()
