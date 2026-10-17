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
  band           --nights DIR [DIR ...] [--out bands.json]  compute + print bands
  check          --bands bands.json --night DIR [--tolerance F] [--rss-factor F]
                     verdicts + exit code (1 = REGRESSION or MISSING series,
                     2 = geometry mismatch: CI-ready). MISSING (gate hardening,
                     defect 2): every band series absent from the night's
                     ledger is a failing verdict — a leg that dies mid-run
                     used to just yield fewer verdicts and exit 0.
  verify-ledger  --night DIR
                     completeness gate (hardening, defect 1): every one of the
                     seven engine TSVs must exist and carry >= 1 VALID data row
                     (>= 7 tab-separated fields, numeric ops_per_sec > 0).
                     Replaces the ci.yml shell count, which passed green on
                     empty/comment-only files (grep -c prints 0 AND exits 1, so
                     `|| echo 0` produced "0\n0" and `[ -lt ]` errored to rc=2
                     = false) and counted crash-stderr lines as data rows.
                     Exit 1 on any engine without valid rows.
  loo            --nights DIR [DIR ...] [--min-nights N] [--tolerance F]
                     leave-one-out cross-validation (hardening, defect 3):
                     bands from the other nights, check the held-out one.
                     In-sample self-consistency is vacuous BY CONSTRUCTION —
                     each night is a sample of its own bands, so its ratio is
                     >= the band min > floor(min x (1-tol)) and REGRESSION
                     cannot fire. LOO is the honest cross-validation.
                     INFORMATIONAL: always exit 0; the numbers are a maintainer
                     decision input (band policy forbids silent widening).
  report         --nights DIR [DIR ...]               cross-night digest

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
# the seven ledger TSVs one complete night must carry (gate hardening, defect 1)
ENGINE_FILES = ("chronokv-group", "chronokv-async", "sqlite-full", "sqlite-normal",
                "lmdb", "rocksdb-sync", "rocksdb-tuned")


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


def run_check(bands, night, tolerance, rss_factor):
    """Core of `check`, shared with `loo`. Returns (verdicts, worst).

    verdicts: (verdict, series, value, band-stats|None, why); worst: 0 clean,
    1 warn-tier (WARN / RSS-RATCHET), 2 fail-tier (REGRESSION / MISSING).
    """
    verdicts, worst = [], 0
    seen_ratio = set()
    for eng, wl in ratios_of(night).items():
        for w, r in wl.items():
            key = f"{eng}/{w}"
            seen_ratio.add(key)
            st = bands["ratio"].get(key)
            if st is None:
                verdicts.append(("NEW", key, r, None, "no band for this series"))
                continue
            floor = st["min"] * (1.0 - tolerance)
            if r < floor:
                verdicts.append(("REGRESSION", key, r, st,
                                 f"below floor {floor:.3f} (band min {st['min']:.3f} -{tolerance:.0%})"))
                worst = max(worst, 2)
            elif r < st["median"] * (1.0 - tolerance):
                verdicts.append(("WARN", key, r, st, "below median - tolerance (inside floor)"))
                worst = max(worst, 1)
            else:
                verdicts.append(("OK", key, r, st, ""))
    # Gate hardening (defect 2): a band series ABSENT from the night is a
    # failing verdict, not silence. Before this, a leg that died mid-run just
    # produced fewer verdicts and the check exited 0 (measured: dropping one
    # workload row -> "29 series checked", deleting chronokv-async.tsv ->
    # "15 series checked", truncating chronokv-group.tsv to 3 rows ->
    # "18 series checked" — all exit 0).
    for key, st in bands["ratio"].items():
        if key not in seen_ratio:
            verdicts.append(("MISSING", key, None, st,
                             "band series absent from this night's ledger (dead or truncated leg?)"))
            worst = max(worst, 2)
    seen_rss = set()
    for eng, wl in night["rows"].items():
        for w, r in wl.items():
            if r["rss_mb"] is None:
                continue
            key = f"{eng}/{w}"
            if eng in ENGINES_CKV:
                seen_rss.add(key)
            st = bands["rss"].get(key)
            if st and r["rss_mb"] > st["max"] * rss_factor:
                verdicts.append(("RSS-RATCHET", key, r["rss_mb"], st,
                                 f"rss {r['rss_mb']:.1f} > band max {st['max']:.1f} x {rss_factor}"))
                worst = max(worst, 1)
    for key, st in bands["rss"].items():
        if key.split("/", 1)[0] in ENGINES_CKV and key not in seen_rss:
            verdicts.append(("MISSING-RSS", key, None, st,
                             "rss band series absent from this night's ledger"))
            worst = max(worst, 2)
    return verdicts, worst


def cmd_check(args):
    with open(args.bands) as f:
        bands = json.load(f)
    night = parse_night(args.night)
    geo = geometry_of(night)
    if geo != bands["geometry"]:
        print(f"GEOMETRY-MISMATCH: night {geo} vs bands {bands['geometry']} — refuse to judge")
        return 2
    cpu = cpu_of(night)
    verdicts, worst = run_check(bands, night, args.tolerance, args.rss_factor)
    prov = any(st.get("provisional") for st in bands["ratio"].values())
    print(f"# check night={args.night} cpu={cpu} bands={'PROVISIONAL' if prov else 'calibrated'}")
    for v, k, val, st, why in sorted(verdicts):
        vs = f"{val:.3f}" if isinstance(val, float) else ("-" if val is None else str(val))
        med = f" band-median={st['median']:.3f}" if st and "median" in st else ""
        print(f"  {v:<11} {k:<34} {vs:>10}{med}  {why}")
    nreg = sum(1 for v in verdicts if v[0] == "REGRESSION")
    nmiss = sum(1 for v in verdicts if v[0].startswith("MISSING"))
    print(f"# {len(verdicts)} series checked: {nreg} REGRESSION, "
          f"{sum(1 for v in verdicts if v[0]=='WARN')} WARN, "
          f"{sum(1 for v in verdicts if v[0]=='RSS-RATCHET')} RSS-RATCHET, "
          f"{nmiss} MISSING")
    if prov and worst < 2:
        print("# NOTE: bands PROVISIONAL — verdicts are advisory until the green week calibrates them")
    return 1 if worst >= 2 else 0


def valid_data_rows(path):
    """Count lines that could be genuine ledger rows (gate hardening, defect 1):
    non-comment, non-blank, >= 7 tab-separated fields, field 4 (ops_per_sec)
    numeric and > 0. Crash-stderr text written into a TSV by a dead engine leg
    has no tabs and no numeric ops field — it does NOT count. Rows with
    ops_per_sec == 0 (the `memory` workload's shape) are not throughput rows
    and do not count either; every engine carries many valid ones."""
    n = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 7:
                continue
            v = _num(parts[3])
            if v is not None and v > 0:
                n += 1
    return n


def cmd_verify_ledger(args):
    print(f"# verify-ledger night={args.night}")
    bad = []
    for eng in ENGINE_FILES:
        path = os.path.join(args.night, eng + ".tsv")
        if not os.path.isfile(path):
            print(f"  MISSING-FILE     {eng}.tsv")
            bad.append(eng)
            continue
        n = valid_data_rows(path)
        if n < 1:
            with open(path, encoding="utf-8", errors="replace") as f:
                head = [ln.rstrip()[:80] for ln in f if ln.strip()][:2]
            print(f"  ZERO-VALID-ROWS  {eng}.tsv  first lines: {head}")
            bad.append(eng)
        else:
            print(f"  ok               {eng}.tsv  {n} valid data rows")
    if bad:
        print(f"# VERIFY-LEDGER FAILED: {len(bad)} engine(s) without valid data rows: {', '.join(bad)}")
        return 1
    print(f"# verify-ledger: all {len(ENGINE_FILES)} engines carry >= 1 valid data row")
    return 0


def cmd_loo(args):
    nights = [parse_night(d) for d in args.nights]
    if len(nights) < 2:
        sys.exit("error: loo needs >= 2 nights")
    min_nights = args.min_nights or (len(nights) - 1)
    print(f"# leave-one-out cross-validation — {len(nights)} nights, bands from the other "
          f"{len(nights) - 1} (min-nights={min_nights}), tolerance={args.tolerance}, "
          f"rss-factor={args.rss_factor}")
    print("# WHY LOO: in-sample, every night is one of its own bands' samples, so its ratio")
    print("# is >= the band min > floor(min x (1-tol)) — the in-sample '0 REGRESSION on all")
    print("# calibration nights' self-consistency claim was VACUOUS by construction (recorded")
    print("# in the gate-hardening changelog entry). LOO is the honest cross-validation.")
    print(f"    {'night':<34} {'REG':>4} {'WARN':>5} {'MISS':>5}   regressed series (ratio < floor)")
    total_reg = 0
    for i, held in enumerate(nights):
        others = nights[:i] + nights[i + 1:]
        bands = compute_bands(others, min_nights)
        verdicts, _ = run_check(bands, held, args.tolerance, args.rss_factor)
        regs = [(k, val, st) for v, k, val, st, _why in verdicts if v == "REGRESSION"]
        nwarn = sum(1 for v, *_ in verdicts if v == "WARN")
        nmiss = sum(1 for v, *_ in verdicts if v.startswith("MISSING"))
        total_reg += len(regs)
        name = os.path.basename(held["dir"].rstrip("/"))
        detail = "; ".join(f"{k} {val:.3f}<{st['min'] * (1 - args.tolerance):.3f}"
                           for k, val, st in regs[:6])
        if len(regs) > 6:
            detail += f"; +{len(regs) - 6} more"
        print(f"    {name:<34} {len(regs):>4} {nwarn:>5} {nmiss:>5}   {detail}")
    print(f"# total held-out REGRESSIONs: {total_reg} across {len(nights)} nights")
    print("# INFORMATIONAL by design: band policy forbids widening tolerance in-flight;")
    print("# a held-out REGRESSION is a maintainer decision (hardware-conditional bands,")
    print("# fleet convergence, or an engine-side investigation), never an automatic widen.")
    return 0


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
    vl = sub.add_parser("verify-ledger")
    vl.add_argument("--night", required=True)
    lo = sub.add_parser("loo")
    lo.add_argument("--nights", nargs="+", required=True)
    lo.add_argument("--min-nights", type=int, default=None,
                    help="default: nights-1 (six for the seven-night green week)")
    lo.add_argument("--tolerance", type=float, default=0.15)
    lo.add_argument("--rss-factor", type=float, default=1.25)
    args = ap.parse_args()
    if args.cmd == "band":
        cmd_band(args)
        return 0
    if args.cmd == "check":
        return cmd_check(args)
    if args.cmd == "verify-ledger":
        return cmd_verify_ledger(args)
    if args.cmd == "loo":
        return cmd_loo(args)
    cmd_report(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
