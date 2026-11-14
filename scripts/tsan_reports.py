#!/usr/bin/env python3
"""tsan_reports.py — report-class ratchet for the UNSUPPRESSED TSan leg.

WHY: docs/tsan.supp suppresses deadlock:chronokv_btree::LatchTable (the M4
page-latch role-flip artifact). Its recorded blind spot is that a REAL
inversion between a page latch and an engine mutex carries a LatchTable frame
too and is silently suppressed. This tool runs the unsuppressed log through a
committed signature baseline: the known artifact's classes are allowed, ANY
new class fails. Signatures may shrink, never grow (the -Wconversion ratchet's
rule, applied to TSan reports).

A signature is (report type, sorted set of chronokv::* symbols in the block).
Symbols are normalized (no addresses, paths, or line numbers) so the baseline
survives recompilation. Data races and other non-deadlock classes ALWAYS fail,
even if someone baselines them — the suppression cannot mask races, and this
leg must never be taught to ignore one.

  baseline --log FILE --out ci/tsan-unsuppressed.baseline   # CALIBRATION ONLY
  check    --log FILE --baseline ci/tsan-unsuppressed.baseline

Calibration is a maintainer act, exactly like the arena bands: run once,
READ every signature, confirm each is the documented LatchTable artifact,
commit the baseline in a dedicated commit citing this file and docs/tsan.supp.
"""
import argparse
import hashlib
import json
import re
import sys

WARN = re.compile(r"^WARNING: ThreadSanitizer: (.+?)(?:\s+\(pid=|$)")
FRAME = re.compile(r"^\s*#\d+\s+(\S+)")
ALWAYS_FAIL = ("data race", "thread leak", "use-after-free", "heap-", "signal-unsafe")


def blocks(path):
    """Yield (type, [symbols]) for each ThreadSanitizer report block."""
    cur_type, cur_syms = None, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = WARN.match(line)
            if m:
                if cur_type is not None:
                    yield cur_type, cur_syms
                cur_type, cur_syms = m.group(1).strip(), []
                continue
            if cur_type is None:
                continue
            fm = FRAME.match(line)
            if fm:
                sym = fm.group(1)
                # keep engine symbols only; strip template args/addresses
                sym = sym.split("<")[0]
                if sym.startswith("chronokv"):
                    cur_syms.append(sym)
    if cur_type is not None:
        yield cur_type, cur_syms


def signatures(path):
    out = {}
    for typ, syms in blocks(path):
        key_syms = tuple(sorted(set(syms))[:12])
        raw = typ + "|" + ",".join(key_syms)
        h = hashlib.sha256(raw.encode()).hexdigest()[:16]
        rec = out.setdefault(h, {"type": typ, "symbols": list(key_syms), "count": 0})
        rec["count"] += 1
    return out


def is_always_fail(typ):
    t = typ.lower()
    return any(t.startswith(p) or p in t for p in ALWAYS_FAIL)


def cmd_baseline(args):
    sigs = signatures(args.log)
    bad = [s for s in sigs.values() if is_always_fail(s["type"])]
    if bad:
        print("error: refusing to baseline non-deadlock classes: "
              + ", ".join(sorted({s['type'] for s in bad})))
        return 1
    with open(args.out, "w") as f:
        json.dump({"signatures": sigs}, f, indent=1, sort_keys=True)
    print(f"# baseline written: {args.out} ({len(sigs)} signature(s))")
    for h, s in sorted(sigs.items()):
        print(f"  {h}  {s['type']}  x{s['count']}  latchtable={'LatchTable' in ' '.join(s['symbols'])}")
    print("# MAINTAINER: read every signature above and confirm it is the documented")
    print("# LatchTable role-flip artifact (docs/tsan.supp) before committing this file.")
    return 0


def cmd_check(args):
    with open(args.baseline) as f:
        base = json.load(f)["signatures"]
    sigs = signatures(args.log)
    print("# TSan unsuppressed report-class ratchet")
    print(f"# log={args.log} baseline={args.baseline} ({len(base)} allowed signature(s))")
    new, known, fatal = [], 0, []
    for h, s in sorted(sigs.items()):
        if is_always_fail(s["type"]):
            fatal.append((h, s))
        elif h not in base:
            new.append((h, s))
        else:
            known += 1
    print(f"- known artifact signatures seen: {known}")
    print(f"- NEW signatures: {len(new)}")
    print(f"- always-fail classes: {len(fatal)}")
    for h, s in new:
        print(f"  NEW    {h}  {s['type']}  x{s['count']}")
        print(f"         symbols: {', '.join(s['symbols'][:8])}")
    for h, s in fatal:
        print(f"  FATAL  {h}  {s['type']}  x{s['count']}")
    if not sigs:
        print("- NOTE: zero reports — the known artifact did not reproduce this run.")
        print("  Informative, not a failure; if it stays absent, the suppression may be retirable.")
    if fatal or new:
        print("# RATCHET FAILED: a new or always-fail TSan report class appeared.")
        print("# Do NOT widen the baseline to silence it. Investigate: a real page-latch")
        print("# vs engine-mutex inversion is exactly the blind spot this leg exists for.")
        return 1
    print("# RATCHET OK: only documented artifact classes present.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("baseline")
    b.add_argument("--log", required=True)
    b.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("--log", required=True)
    c.add_argument("--baseline", required=True)
    a = ap.parse_args()
    return cmd_baseline(a) if a.cmd == "baseline" else cmd_check(a)


if __name__ == "__main__":
    sys.exit(main())
