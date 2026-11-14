#!/usr/bin/env python3
"""readrandom_variance.py — short-window noise probe for the arena's readrandom
series (the nightly red-night investigation). Stdlib only; no network.

WHY: readrandom is the arena's shortest timed window (~tens of ms: 50k
in-memory reads), and ratio-normalization cancels the CPU-model lottery but
NOT bursty shared-runner contention that lands on the numerator's window and
misses the baselines'. This probe runs the SAME arena binary back-to-back on
ONE runner and reports the ops/s distribution, so the maintainer can decide
whether the window is intrinsically below the runner noise floor (environment)
or stable (which would reopen the engine/microarch question).

INPUT: a directory of arena TSVs produced by the dispatch-only CI job:
  rr_<i>.tsv     — `--workloads readrandom` (window variance on a fresh prep)
  order_<i>.tsv  — `--workloads fillrandom,readrandom` (the ledger ordering)
  steal_<i>.txt / steal_<i>_after.txt — optional /proc/stat aggregate cpu line

OUTPUT: a markdown report on stdout and a one-line verdict. ALWAYS exit 0 —
a MEASUREMENT vehicle, not a gate (the writer-sweep / LOO convention).

Verdict thresholds are HEURISTIC calibration knobs, not claimed truths;
override with --wide-ratio / --wide-cv / --tight-ratio / --tight-cv.
"""
import argparse
import glob
import math
import os
import re
import sys


def parse_row(path, workload):
    """Return (ops_per_sec, p50_us, p99_us, seconds) for a workload row, or None."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                p = line.rstrip("\n").split("\t")
                if len(p) < 7 or p[0].strip() != workload:
                    continue
                def num(x):
                    x = x.strip()
                    if x in ("-", ""):
                        return None
                    try:
                        return float(x)
                    except ValueError:
                        return None
                ops = num(p[3])
                if ops is None or ops <= 0:
                    return None
                return ops, num(p[4]), num(p[5]), num(p[2])
    except OSError:
        return None
    return None


def steal_jiffies(path):
    """Aggregate cpu steal field (8th column) from a /proc/stat snapshot."""
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("cpu "):
                    parts = line.split()
                    if len(parts) >= 9:
                        return int(parts[8])
    except (OSError, ValueError):
        pass
    return None


def stats(xs):
    xs = sorted(xs)
    n = len(xs)
    mean = sum(xs) / n
    med = xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])
    sd = math.sqrt(sum((x - mean) ** 2 for x in xs) / n)
    def pct(q):
        return xs[min(n - 1, int(q * (n - 1) + 0.5))]
    gap, gi = 0.0, -1
    for i in range(1, n):
        g = xs[i] - xs[i - 1]
        if g > gap:
            gap, gi = g, i
    rng = xs[-1] - xs[0]
    bimodal = (rng > 0 and gap > 0.4 * rng and n >= 6
               and gi >= 0.2 * n and (n - gi) >= 0.2 * n)
    return {"n": n, "mean": mean, "median": med, "min": xs[0], "max": xs[-1],
            "sd": sd, "cv": sd / mean if mean else 0.0,
            "ratio": xs[-1] / xs[0] if xs[0] else float("inf"),
            "p10": pct(0.10), "p90": pct(0.90), "bimodal": bimodal}


def verdict(st, args):
    if st["n"] < args.min_iters:
        return "INSUFFICIENT", f"only {st['n']} valid iterations (< {args.min_iters}); raise RR_ITERS or check for crashed legs"
    if st["ratio"] >= args.wide_ratio or st["cv"] >= args.wide_cv:
        return "WIDE", ("ops/s spread exceeds the noise-floor thresholds on an UNCHANGED binary: "
                        "the short readrandom window is below this runner's noise floor "
                        "(environmental variance). Recalibration / a longer window / a warmup "
                        "is justified — as a dedicated ledger-format commit, never in-flight.")
    if st["ratio"] <= args.tight_ratio and st["cv"] <= args.tight_cv:
        return "TIGHT", ("ops/s is stable on this runner: the nightly collapses are NOT intrinsic "
                         "short-window noise. Reopen the engine/microarch question; do NOT "
                         "recalibrate to absorb it.")
    return "INCONCLUSIVE", ("spread is between the tight and wide thresholds; increase iterations "
                            "or pin the runner class before deciding")


def fmt_series(name, st, v, why):
    return [f"### {name}", "",
            f"- n={st['n']}  mean={st['mean']:.0f}  median={st['median']:.0f} ops/s",
            f"- min={st['min']:.0f}  p10={st['p10']:.0f}  p90={st['p90']:.0f}  max={st['max']:.0f} ops/s",
            f"- max/min={st['ratio']:.3f}  CV={st['cv']:.3f}  sd={st['sd']:.0f}"
            + ("  bimodal-hint=YES" if st["bimodal"] else ""),
            f"- **verdict: {v}** — {why}", ""]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir")
    ap.add_argument("--min-iters", type=int, default=10)
    ap.add_argument("--wide-ratio", type=float, default=1.5)
    ap.add_argument("--wide-cv", type=float, default=0.15)
    ap.add_argument("--tight-ratio", type=float, default=1.15)
    ap.add_argument("--tight-cv", type=float, default=0.08)
    args = ap.parse_args()

    print("# readrandom short-window variance probe")
    print(f"# dir={args.dir}  thresholds: wide(ratio>={args.wide_ratio} or cv>={args.wide_cv}) "
          f"tight(ratio<={args.tight_ratio} and cv<={args.tight_cv})  min-iters={args.min_iters}")
    print("# MEASUREMENT vehicle, not a gate — exit 0 always; the verdict is a maintainer input.")
    print()

    groups = {"readrandom (fresh prep)": ("rr_*.tsv", "readrandom"),
              "readrandom (after fillrandom — ledger ordering)": ("order_*.tsv", "readrandom")}
    overall = None
    for label, (pat, wl) in groups.items():
        paths = sorted(glob.glob(os.path.join(args.dir, pat)))
        ops, p50s, p99s, steals = [], [], [], []
        skipped = 0
        for p in paths:
            row = parse_row(p, wl)
            if row is None:
                skipped += 1
                continue
            o, p50, p99, _sec = row
            ops.append(o)
            if p50 is not None:
                p50s.append(p50)
            if p99 is not None:
                p99s.append(p99)
            m = re.search(r"(\d+)", os.path.basename(p))
            if m:
                idx = m.group(1)
                before = steal_jiffies(os.path.join(args.dir, f"steal_{idx}.txt"))
                after = steal_jiffies(os.path.join(args.dir, f"steal_{idx}_after.txt"))
                if before is not None and after is not None:
                    steals.append((after - before, o))
        if not ops:
            print(f"### {label}\n\n- no valid iterations ({skipped} skipped) — INSUFFICIENT\n")
            continue
        st = stats(ops)
        v, why = verdict(st, args)
        for line in fmt_series(label, st, v, why):
            print(line)
        if p50s and p99s:
            p50st, p99st = stats(p50s), stats(p99s)
            print(f"- latency across iterations: p50 median={p50st['median']:.1f}us "
                  f"(max/min={p50st['ratio']:.2f}), p99 median={p99st['median']:.1f}us "
                  f"(max/min={p99st['ratio']:.2f})")
            if p50st["ratio"] <= 1.3 and p99st["ratio"] >= 2.0:
                print("- **stall signature**: p50 stable while p99 swings — a few long stalls "
                      "inflate the wall (scheduling/steal/writeback), not a uniform per-op slowdown.")
            elif p50st["ratio"] >= 1.5:
                print("- **uniform-slowdown signature**: p50 itself swings — investigate per-op cost.")
            print()
        if steals:
            steals.sort()
            lo = [o for _s, o in steals[:max(1, len(steals) // 3)]]
            hi = [o for _s, o in steals[-max(1, len(steals) // 3):]]
            print(f"- steal correlation: lowest-steal third mean={sum(lo)/len(lo):.0f} ops/s, "
                  f"highest-steal third mean={sum(hi)/len(hi):.0f} ops/s "
                  f"(n={len(steals)} iterations with steal snapshots)")
            if sum(lo) / len(lo) > 1.2 * (sum(hi) / len(hi)):
                print("- **steal anti-correlation**: high-steal iterations are slower — "
                      "direct evidence of shared-runner contention.")
            print()
        if label.startswith("readrandom (fresh"):
            overall = (st, v, why)

    print("## Bottom line")
    if overall:
        st, v, why = overall
        print(f"- primary series verdict: **{v}**. {why}")
    print("- This probe does not change any band or gate. If WIDE, the remedy is a dedicated")
    print("  recalibration / longer-window / warmup commit (ledger-format change), per policy.")
    print("  If TIGHT, do not recalibrate — investigate the engine/microarch path instead.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
