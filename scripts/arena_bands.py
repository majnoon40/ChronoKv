#!/usr/bin/env python3
"""arena_bands.py — noise-band computation and regression checking for the
v29 M1 arena ledger (bench/results/*.tsv artifacts).

Pre-built ahead of the green week's completion so the M1 remainder step
"noise bands + gate enablement" is a calibration exercise, not a design
one. Stdlib only; no network access (inputs are local directories of
ledger TSVs, e.g. extracted arena-ledger-<run_id> artifacts).

THE DESIGN FACT THAT SHAPED THIS TOOL (measured from the first three
banked nights, runs #82/#88/#91, 2026-10-11..13): the nightly runner CPU
DIFFERS NIGHT TO NIGHT (AMD EPYC 9V45 / Intel Xeon Platinum 8573C / AMD
EPYC 7763 across those three) even on the re-merged single-job vehicle.
Same-night cross-engine comparisons are fair by construction (one job,
one machine — the fairness protocol's whole product); CROSS-night raw
numbers are not. Bands are therefore computed in two tiers:

  PRIMARY   ratio bands — per workload, ChronoKV ops/s divided by the
            geometric mean of that night's baseline engines. The ratio is
            hardware-normalized (numerator and denominator ran on the
            same CPU), so its cross-night spread IS the noise band, and
            a drop below the floor is an engine regression rather than a
            runner lottery outcome.
  SECONDARY raw bands — ops/s grouped by (cpu model, engine, workload).
            With one night per CPU these stay degenerate/PROVISIONAL and
            are reported honestly as such; they become useful only if the
            fleet converges or self-hosted runners land.
  RSS       rss_mb is hardware-independent enough to band cross-night
            directly; the deletechurn rss delta is the P1 before-column
            and gets a ratchet check (WARN above band max x factor) that
            M3's acceptance ("stabilizes instead of ratcheting") will
            tighten into a gate.

Provenance discipline (rule 9): every band records the nights, hardware
and geometry it was computed from; geometry mismatches REFUSE to mix
("geometry changes are ledger-format changes" — the ci.yml arena job's
own comment). Bands from fewer than --min-nights (default 7 = the green
week) are stamped PROVISIONAL: reportable, not gateable.

Subcommands:
  band    --nights DIR [DIR ...] [--out bands.json]   compute + print bands
  check   --bands bands.json --night DIR [--tolerance F] [--rss-factor F]
                                                      verdicts + exit code
                                                      (1 = REGRESSION: CI-ready)
  report  --nights DIR [DIR ...]                      cross-night digest

Ledger row format (bench/arena.cpp, "# columns:" header line):
  workload \t ops \t seconds \t ops_per_sec \t p50_us \t p99_us \t rss_mb \t note
with '#' methodology/comment lines carrying cpu model, geometry, version.
"""

import argparse
import json
import math
import os
import re
import sys

ENGINES_CKV = ("chronokv-group", "chronokv-async")
GEOMETRY_KEYS = ("threads", "keys", "valsize", "seed", "ops")


def _num(s):
    s = s.strip()
    if s in ("-", ""):
        return None
    try:
        return float(s)
    except ValueError:
        return None


def parse_night(d):
    """Parse one ledger directory -> {'meta': per-file headers, 'rows': engine -> workload -> metrics}."""
    meta, rows = {}, {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".tsv"):
            continue
        engine = fn[:-4]
        rows[engine] = {}
        m = {"file": fn}
        with open(os.path.join(d, fn), encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if line.startswith("#"):
                    if "=" in line:
                        k, _, v = line.lstrip("# ").partition("=")
                        m[k.strip()] = v.strip()
                    elif "model name" in line:
                        m["cpu"] = line.split(":", 1)[-1].strip()
                    elif "MemTotal" in line:
                        m["mem"] = line.split()[-2] if len(line.split()) > 1 else "?"
                    continue
                parts = line.split("\t")
                if len(parts) < 7:
                    continue
                w = parts[0].strip()
                rows[engine][w] = {
                    "ops": _num(parts[1]),
                    "seconds": _num(parts[2]),
                    "ops_per_sec": _num(parts[3]),
                    "p50_us": _num(parts[4]),
                    "p99_us": _num(parts[5]),
                    "rss_mb": _num(parts[6]),
                    "note": parts[7] if len(parts) > 7 else "",
                }
        meta[engine] = m
    if not rows:
        sys.exit(f"error: no .tsv ledgers found in {d}")
    return {"meta": meta, "rows": rows, "dir": d}


def geometry_of(night):
    """Extract the pinned geometry from any engine header of one night."""
    m = next(iter(night["meta"].values()))
    geo = {}
    dur_line = m.get("durability", "")
    if dur_line:  # "# durability=group threads=4 keys=... valsize=... seed=... ops=..."
        # the header parser split on the FIRST '=' only: the value's leading
        # word is durability's own value ("group"/"async"), the rest are k=v.
        toks = dur_line.split()
        if toks and "=" not in toks[0]:
            geo["durability"] = toks[0]
        for tok in toks:
            if "=" in tok:
                k, _, v = tok.partition("=")
                geo[k] = v
    # durability is per-FILE (group vs async), not a shared geometry knob:
    # the pinned CI geometry is the rest; keep durability out of the refusal key
    geo.pop("durability", None)
    return {k: geo.get(k) for k in GEOMETRY_KEYS}


def cpu_of(night):
    cpus = {m.get("cpu", "?") for m in night["meta"].values()}
    if len(cpus) > 1:
        sys.exit(f"error: night {night['dir']} mixes CPUs within one job: {cpus} "
                 "(fairness protocol violated — single-job same-machine is the ledger's product)")
    return cpus.pop()


def baselines_geo_mean(night, workload):
    vals = []
    for eng, wl in night["rows"].items():
        if eng in ENGINES_CKV:
            continue
        r = wl.get(workload)
        if r and r["ops_per_sec"] and r["ops_per_sec"] > 0:
            vals.append(r["ops_per_sec"])
    if not vals:
        return None
    return math.exp(sum(math.log(v) for v in vals) / len(vals))


def ratios_of(night):
    """engine -> workload -> ratio vs same-night baseline geomean."""
    out = {}
    for eng in ENGINES_CKV:
        if eng not in night["rows"]:
            continue
        out[eng] = {}
        for w, r in night["rows"][eng].items():
            if not r["ops_per_sec"]:
                continue
            base = baselines_geo_mean(night, w)
            if base:
                out[eng][w] = r["ops_per_sec"] / base
    return out


def _stats(xs):
    xs = sorted(xs)
    n = len(xs)
    med = xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])
    return {"n": n, "median": med, "min": xs[0], "max": xs[-1],
            "spread_pct": (100.0 * (xs[-1] - xs[0]) / med) if med else 0.0}


def compute_bands(nights, min_nights):
    for night in nights:
        night["geometry"] = geometry_of(night)
        night["cpu"] = cpu_of(night)
    g0 = nights[0]["geometry"]
    for night in nights[1:]:
        if night["geometry"] != g0:
            sys.exit(f"error: geometry mismatch between {nights[0]['dir']} {g0} "
                     f"and {night['dir']} {night['geometry']} — refusing to mix "
                     "(geometry changes are ledger-format changes)")
    bands = {"geometry": g0, "min_nights": min_nights,
             "nights": [{"dir": n["dir"], "cpu": n["cpu"],
                         "version": next(iter(n["meta"].values())).get("chronokv_version", "?")}
                        for n in nights],
             "ratio": {}, "raw_by_cpu": {}, "rss": {}}
    provisional = len(nights) < min_nights
    # primary: ratio bands
    per = {}
    for night in nights:
        for eng, wl in ratios_of(night).items():
            for w, r in wl.items():
                per.setdefault((eng, w), []).append(r)
    for (eng, w), xs in sorted(per.items()):
        st = _stats(xs)
        st["provisional"] = provisional or st["n"] < min_nights
        bands["ratio"][f"{eng}/{w}"] = st
    # secondary: raw ops/s by cpu
    rawp = {}
    for night in nights:
        for eng, wl in night["rows"].items():
            for w, r in wl.items():
                if r["ops_per_sec"]:
                    rawp.setdefault((night["cpu"], eng, w), []).append(r["ops_per_sec"])
    for (cpu, eng, w), xs in sorted(rawp.items()):
        st = _stats(xs)
        st["provisional"] = True  # one night per CPU in practice until the fleet converges
        bands["raw_by_cpu"][f"{cpu}|{eng}/{w}"] = st
    # rss bands (hardware-independent enough; ratchet-watch)
    rssp = {}
    for night in nights:
        for eng, wl in night["rows"].items():
            for w, r in wl.items():
                if r["rss_mb"] is not None:
                    rssp.setdefault((eng, w), []).append(r["rss_mb"])
    for (eng, w), xs in sorted(rssp.items()):
        st = _stats(xs)
        st["provisional"] = provisional or st["n"] < min_nights
        bands["rss"][f"{eng}/{w}"] = st
    return bands


def cmd_band(args):
    nights = [parse_night(d) for d in args.nights]
    bands = compute_bands(nights, args.min_nights)
    if args.out:
        with open(args.out, "w") as f:
            json.dump(bands, f, indent=1, sort_keys=True)
    tag = "PROVISIONAL" if bands["ratio"] and next(iter(bands["ratio"].values()))["provisional"] else "CALIBRATED"
    print(f"# bands over {len(nights)} night(s) — {tag} (min-nights={args.min_nights})")
    print(f"# geometry: {bands['geometry']}")
    for n in bands["nights"]:
        print(f"#   night {n['dir']}  cpu={n['cpu']}  version={n['version']}")
    print("# PRIMARY ratio bands (chronokv ops/s / same-night baseline geomean):")
    print(f"#   {'series':<34} {'n':>2} {'median':>8} {'min':>8} {'max':>8} {'spread%':>8}")
    for k, st in bands["ratio"].items():
        print(f"    {k:<34} {st['n']:>2} {st['median']:>8.3f} {st['min']:>8.3f} {st['max']:>8.3f} {st['spread_pct']:>8.1f}")
    print("# RSS bands (mb):")
    for k, st in bands["rss"].items():
        if k.startswith("chronokv"):
            print(f"    {k:<34} {st['n']:>2} {st['median']:>8.1f} {st['min']:>8.1f} {st['max']:>8.1f} {st['spread_pct']:>8.1f}")
    deg = sum(1 for st in bands["raw_by_cpu"].values() if st["n"] == 1)
    print(f"# raw-by-cpu bands: {len(bands['raw_by_cpu'])} groups, {deg} degenerate (n=1) — "
          "secondary tier; cross-night CPUs differ (fleet lottery), raw bands stay hardware-conditional")
    if args.out:
        print(f"# written: {args.out}")


def cmd_check(args):
    with open(args.bands) as f:
        bands = json.load(f)
    night = parse_night(args.night)
    geo = geometry_of(night)
    if geo != bands["geometry"]:
        print(f"GEOMETRY-MISMATCH: night {geo} vs bands {bands['geometry']} — refuse to judge")
        return 2
    cpu = cpu_of(night)
    verdicts, worst = [], 0
    for eng, wl in ratios_of(night).items():
        for w, r in wl.items():
            st = bands["ratio"].get(f"{eng}/{w}")
            if st is None:
                verdicts.append(("NEW", f"{eng}/{w}", r, None, "no band for this series"))
                continue
            floor = st["min"] * (1.0 - args.tolerance)
            if r < floor:
                verdicts.append(("REGRESSION", f"{eng}/{w}", r, st, f"below floor {floor:.3f} (band min {st['min']:.3f} -{args.tolerance:.0%})"))
                worst = max(worst, 2)
            elif r < st["median"] * (1.0 - args.tolerance):
                verdicts.append(("WARN", f"{eng}/{w}", r, st, "below median - tolerance (inside floor)"))
                worst = max(worst, 1)
            else:
                verdicts.append(("OK", f"{eng}/{w}", r, st, ""))
    for eng, wl in night["rows"].items():
        for w, r in wl.items():
            if r["rss_mb"] is None:
                continue
            st = bands["rss"].get(f"{eng}/{w}")
            if st and r["rss_mb"] > st["max"] * args.rss_factor:
                verdicts.append(("RSS-RATCHET", f"{eng}/{w}", r["rss_mb"], st,
                                 f"rss {r['rss_mb']:.1f} > band max {st['max']:.1f} x {args.rss_factor}"))
                worst = max(worst, 1)
    prov = any(st.get("provisional") for st in bands["ratio"].values())
    print(f"# check night={args.night} cpu={cpu} bands={'PROVISIONAL' if prov else 'calibrated'}")
    for v, k, val, st, why in sorted(verdicts):
        vs = f"{val:.3f}" if isinstance(val, float) else str(val)
        med = f" band-median={st['median']:.3f}" if st and "median" in st else ""
        print(f"  {v:<11} {k:<34} {vs:>10}{med}  {why}")
    nreg = sum(1 for v in verdicts if v[0] == "REGRESSION")
    print(f"# {len(verdicts)} series checked: {nreg} REGRESSION, "
          f"{sum(1 for v in verdicts if v[0]=='WARN')} WARN, "
          f"{sum(1 for v in verdicts if v[0]=='RSS-RATCHET')} RSS-RATCHET")
    if prov and worst < 2:
        print("# NOTE: bands PROVISIONAL — verdicts are advisory until the green week calibrates them")
    return 1 if worst >= 2 else 0


def cmd_report(args):
    nights = [parse_night(d) for d in args.nights]
    print(f"# cross-night digest ({len(nights)} nights) — ratios vs same-night baseline geomean")
    series = {}
    for i, night in enumerate(nights):
        for eng, wl in ratios_of(night).items():
            for w, r in wl.items():
                series.setdefault(f"{eng}/{w}", {})[i] = r
    hdr = "  ".join(f"{os.path.basename(n['dir'].rstrip('/')):>12}" for n in nights)
    print(f"    {'series':<34} {hdr}")
    for k in sorted(series):
        cells = "  ".join(f"{series[k].get(i, float('nan')):>12.3f}" for i in range(len(nights)))
        print(f"    {k:<34} {cells}")
    for i, night in enumerate(nights):
        print(f"#   night {i}: {night['dir']} cpu={cpu_of(night)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("band")
    b.add_argument("--nights", nargs="+", required=True)
    b.add_argument("--out", default=None)
    b.add_argument("--min-nights", type=int, default=7)
    c = sub.add_parser("check")
    c.add_argument("--bands", required=True)
    c.add_argument("--night", required=True)
    c.add_argument("--tolerance", type=float, default=0.15,
                   help="fraction below band min (floor) / below median (warn)")
    c.add_argument("--rss-factor", type=float, default=1.25)
    r = sub.add_parser("report")
    r.add_argument("--nights", nargs="+", required=True)
    args = ap.parse_args()
    if args.cmd == "band":
        cmd_band(args)
        return 0
    if args.cmd == "check":
        return cmd_check(args)
    cmd_report(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
