#!/usr/bin/env python3
"""test_arena_gate.py — self-tests (positive + negative controls) for the v29 M1
arena regression gate. Stdlib only; no network; no runner hardware needed — the
fixtures are built from the committed promoted green-week TSVs, so this runs on
every push/PR (ci.yml job `arena-gate-selftest`) and nightly.

If this suite is red, the arena gate's verdicts are not trustworthy until it is
green again. Each section pins one hardened defect (the BEFORE evidence for all
four is in the gate-hardening commit's message and the CHANGELOG entry):

  defect 1  verify-ledger: empty / comment-only / stderr-only / missing-file
            fixtures must exit nonzero (the replaced shell count passed ALL of
            them green); a pristine promoted night must exit 0.
  defect 2  check MISSING verdicts: a dropped workload row, a deleted engine
            TSV, and a truncated TSV must each exit 1 with MISSING verdicts
            (before: fewer verdicts, exit 0); pristine nights exit 0 with
            0 MISSING (no false positives).
  defect 3  loo (leave-one-out) runs structurally: one line per held-out night
            plus the totals line. The REGRESSION NUMBERS are informational and
            deliberately NOT asserted — band policy forbids an automatic
            reaction, and the in-sample "0 REGRESSION on all calibration
            nights" claim they replace was vacuous by construction (each night
            is a sample of its own bands).
  defect 4  synthetic-regression controls: chronokv ops_per_sec x0.5 must exit
            1 with REGRESSION verdicts; x0.95 must exit 0 with none (WARN is
            acceptable).

Meta-controls: the committed inputs (bench/arena_bands.json and the promoted
week) are hashed before and after — the tests must never mutate what they
judge, and the bands/tolerance are never edited by anything in this file.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(HERE, "arena_bands.py")
BANDS = os.path.join(ROOT, "bench", "arena_bands.json")
WEEK = os.path.join(ROOT, "bench", "results", "promoted", "green-week-2026-10-11_17")
N7 = os.path.join(WEEK, "N7-2026-10-17-run99")

FAILURES = []


def run(*args):
    p = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def expect(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    line = f"  [{tag}] {name}"
    if detail and not cond:
        line += f"   << {detail}"
    print(line)
    if not cond:
        FAILURES.append(name)


def tree_hash(root):
    h = hashlib.sha256()
    for dirpath, _dirs, files in os.walk(root):
        for fn in sorted(files):
            p = os.path.join(dirpath, fn)
            h.update(os.path.relpath(p, root).encode())
            h.update(hashlib.sha256(open(p, "rb").read()).digest())
    return h.hexdigest()


def tmp_night(src=N7):
    d = tempfile.mkdtemp(prefix="arena_gate_selftest_")
    dst = os.path.join(d, "night")
    shutil.copytree(src, dst)
    return dst


def write_engine(dst, name, content):
    with open(os.path.join(dst, name + ".tsv"), "w") as f:
        f.write(content)


def drop_row(dst, engine, workload):
    p = os.path.join(dst, engine + ".tsv")
    lines = [l for l in open(p) if not l.startswith(workload + "\t")]
    open(p, "w").writelines(lines)


def truncate(dst, engine, keep_rows):
    p = os.path.join(dst, engine + ".tsv")
    head = [l for l in open(p) if l.startswith("#")]
    data = [l for l in open(p) if not l.startswith("#")][:keep_rows]
    open(p, "w").writelines(head + data)


def scale_ckv(dst, factor):
    for eng in ("chronokv-group", "chronokv-async"):
        p = os.path.join(dst, eng + ".tsv")
        out = []
        for line in open(p):
            if line.startswith("#"):
                out.append(line)
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 7:
                try:
                    v = float(parts[3])
                    if v:
                        parts[3] = repr(round(v * factor, 6))
                except ValueError:
                    pass
            out.append("\t".join(parts) + "\n")
        open(p, "w").writelines(out)


def main():
    for need in (TOOL, BANDS, WEEK, N7):
        if not os.path.exists(need):
            print(f"FATAL: missing required path: {need}")
            return 1
    bands_hash_before = hashlib.sha256(open(BANDS, "rb").read()).hexdigest()
    week_hash_before = tree_hash(WEEK)

    print("# arena gate self-tests (controls for the four hardened defects)")

    # ---------------- defect 1: verify-ledger ----------------
    print("## defect 1 — verify-ledger (positive controls must exit nonzero)")
    cases = {
        "empty TSV": lambda d: write_engine(d, "chronokv-group", ""),
        "comment-only TSV": lambda d: write_engine(
            d, "chronokv-async",
            "# chronokv-arena - methodology (rule 9)\n# chronokv_version=0.28.1\n"),
        "stderr-only TSV": lambda d: write_engine(
            d, "sqlite-full",
            "terminate called after throwing an instance of 'std::bad_alloc'\n"
            "  what():  std::bad_alloc\nAborted (core dumped)\n"),
        "missing file": lambda d: os.remove(os.path.join(d, "lmdb.tsv")),
    }
    for name, mutate in cases.items():
        night = tmp_night()
        mutate(night)
        rc, out = run("verify-ledger", "--night", night)
        expect(f"verify-ledger rejects: {name}", rc != 0, f"exit={rc}")
        shutil.rmtree(os.path.dirname(night))
    rc, out = run("verify-ledger", "--night", N7)
    expect("verify-ledger accepts pristine promoted night (negative control)",
           rc == 0, f"exit={rc} out={out.strip().splitlines()[-1:]}")

    # ---------------- defect 2: MISSING verdicts ----------------
    print("## defect 2 — check MISSING verdicts (positive controls must exit 1)")
    night = tmp_night(); drop_row(night, "chronokv-group", "ycsb_c.read")
    rc, out = run("check", "--bands", BANDS, "--night", night)
    expect("check flags a dropped workload row (exit 1 + MISSING)",
           rc == 1 and "MISSING" in out, f"exit={rc}")
    shutil.rmtree(os.path.dirname(night))

    night = tmp_night(); os.remove(os.path.join(night, "chronokv-async.tsv"))
    rc, out = run("check", "--bands", BANDS, "--night", night)
    expect("check flags a deleted chronokv TSV (exit 1 + MISSING)",
           rc == 1 and "MISSING" in out, f"exit={rc}")
    shutil.rmtree(os.path.dirname(night))

    night = tmp_night(); truncate(night, "chronokv-group", 3)
    rc, out = run("check", "--bands", BANDS, "--night", night)
    expect("check flags a truncated TSV (3 data rows; exit 1 + MISSING)",
           rc == 1 and "MISSING" in out, f"exit={rc}")
    shutil.rmtree(os.path.dirname(night))

    rc, out = run("check", "--bands", BANDS, "--night", N7)
    expect("pristine night: exit 0 and 0 MISSING (no false positives)",
           rc == 0 and " 0 MISSING" in out, f"exit={rc}")

    # ---------------- defect 3: loo structure ----------------
    print("## defect 3 — loo (leave-one-out) runs; numbers are informational")
    nights = sorted(os.path.join(WEEK, d) for d in os.listdir(WEEK))
    rc, out = run("loo", "--nights", *nights)
    ok = rc == 0 and "total held-out REGRESSIONs:" in out
    per_night = sum(1 for d in nights if os.path.basename(d) in out)
    expect(f"loo completes and reports all {len(nights)} held-out nights",
           ok and per_night == len(nights), f"exit={rc} nights_in_output={per_night}")
    for line in out.splitlines():   # echo the table into the CI log (the record)
        if line.startswith("    N") or line.startswith("# total"):
            print("   |" + line)

    # ---------------- defect 4: synthetic regression ----------------
    print("## defect 4 — synthetic-regression controls")
    night = tmp_night(); scale_ckv(night, 0.5)
    rc, out = run("check", "--bands", BANDS, "--night", night)
    nreg = out.count("REGRESSION ")
    expect("x0.5 synthetic regression: exit 1 with REGRESSION verdicts",
           rc == 1 and nreg >= 1, f"exit={rc} reg_lines={nreg}")
    shutil.rmtree(os.path.dirname(night))

    night = tmp_night(); scale_ckv(night, 0.95)
    rc, out = run("check", "--bands", BANDS, "--night", night)
    expect("x0.95 near-miss: exit 0, zero REGRESSION verdicts (WARN allowed)",
           rc == 0 and "REGRESSION " not in out.replace("0 REGRESSION", ""),
           f"exit={rc}")
    shutil.rmtree(os.path.dirname(night))

    # ---------------- meta controls ----------------
    print("## meta — the tests never mutate what they judge")
    expect("bench/arena_bands.json unchanged by this suite",
           hashlib.sha256(open(BANDS, "rb").read()).hexdigest() == bands_hash_before)
    expect("promoted green-week TSVs unchanged by this suite",
           tree_hash(WEEK) == week_hash_before)

    print(f"# {'SELFTEST FAILED: ' + str(len(FAILURES)) + ' control(s): ' + '; '.join(FAILURES) if FAILURES else 'arena gate self-tests: ALL CONTROLS PASS'}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
