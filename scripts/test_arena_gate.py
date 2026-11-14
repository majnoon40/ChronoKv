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
  defect 5  half-dead BASELINE legs (found by the M1 owner reviewing the
            hardening itself): dropping ONE baseline workload row must fail
            verify-ledger (workload-set comparison against the calibrated
            reference) and make check REFUSE the affected ratios (BASELINE-
            GAP, fail tier) with ZERO REGRESSION verdicts — before the fix,
            probe 1 passed the whole gate while the ratio silently shifted
            -15.9%, and probe 2 (truncated rocksdb-sync) produced two FALSE
            fillrandom REGRESSIONs. Includes the masking direction (dropping
            a FAST baseline row) and the extras direction (an unexpected new
            workload is a ledger-format change and must fail too).
  defect 6  baseline VERSION identity: the nightly arena installs distro
            baseline packages while bench/third_party/versions.txt pins source
            recipes; a library version change moves the same-night geomean
            denominator and can fake or mask a REGRESSION. verify-ledger must
            fail on a drifted or missing baseline version header (naming the
            engine), must NOT gate ChronoKV's own version, and must hard-error
            on an internally inconsistent reference. The pristine night must
            report all five baselines version == calibrated (anti-vacuity).

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


def set_baseline_version(dst, engine, new_version):
    """Rewrite the '; version=...' tail of a baseline TSV's '# engine=' header."""
    p = os.path.join(dst, engine + ".tsv")
    out = []
    for line in open(p):
        if line.startswith("# engine=") and "version=" in line:
            head, _, _tail = line.rstrip("\n").rpartition("version=")
            line = head + "version=" + new_version + "\n"
        out.append(line)
    open(p, "w").writelines(out)

def strip_baseline_version(dst, engine):
    """Remove the version field from a baseline TSV's '# engine=' header."""
    p = os.path.join(dst, engine + ".tsv")
    out = []
    for line in open(p):
        if line.startswith("# engine=") and "version=" in line:
            head, _, _tail = line.rstrip("\n").rpartition("version=")
            line = head.rstrip("; ").rstrip() + "\n"
        out.append(line)
    open(p, "w").writelines(out)

def main():
    for need in (TOOL, BANDS, WEEK, N7):
        if not os.path.exists(need):
            print(f"FATAL: missing required path: {need}")
            return 1
    bands_hash_before = hashlib.sha256(open(BANDS, "rb").read()).hexdigest()
    week_hash_before = tree_hash(WEEK)

    print("# arena gate self-tests (controls for the six hardened defects)")

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

    # ---------------- defect 5: half-dead baseline legs ----------------
    print("## defect 5 — half-dead BASELINE legs (set comparison + ratio refusal)")

    def summary_field(out, field):
        for line in out.splitlines():
            if line.startswith("# ") and " series checked:" in line:
                for tok in line.split(":")[1].split(","):
                    tok = tok.strip()
                    if tok.endswith(field):
                        return int(tok.split()[0])
        return None

    # probe 1: drop ONE workload row from a baseline leg
    night = tmp_night(); drop_row(night, "sqlite-full", "fillseq")
    rc_v, out_v = run("verify-ledger", "--night", night)
    expect("probe 1 (dropped sqlite-full fillseq): verify-ledger exit 1 naming the engine+workload",
           rc_v == 1 and "sqlite-full" in out_v and "fillseq" in out_v, f"exit={rc_v}")
    rc_c, out_c = run("check", "--bands", BANDS, "--night", night)
    expect("probe 1: check refuses the ratios (exit 1, 2 BASELINE-GAP, 0 REGRESSION — no silent shift)",
           rc_c == 1 and summary_field(out_c, "BASELINE-GAP") == 2
           and summary_field(out_c, "REGRESSION") == 0,
           f"exit={rc_c} gaps={summary_field(out_c, 'BASELINE-GAP')} regs={summary_field(out_c, 'REGRESSION')}")
    shutil.rmtree(os.path.dirname(night))

    # probe 2: truncate a baseline leg to 1 row (the false-REGRESSION shape)
    night = tmp_night(); truncate(night, "rocksdb-sync", 1)
    rc_v, out_v = run("verify-ledger", "--night", night)
    expect("probe 2 (rocksdb-sync truncated to 1 row): verify-ledger exit 1 listing missing workloads",
           rc_v == 1 and "rocksdb-sync" in out_v and "missing=" in out_v, f"exit={rc_v}")
    rc_c, out_c = run("check", "--bands", BANDS, "--night", night)
    gaps = summary_field(out_c, "BASELINE-GAP")
    expect("probe 2: check exit 1 with BASELINE-GAPs and ZERO false REGRESSIONs (was: 2 false fillrandom REGRESSIONs)",
           rc_c == 1 and gaps is not None and gaps >= 28
           and summary_field(out_c, "REGRESSION") == 0,
           f"exit={rc_c} gaps={gaps} regs={summary_field(out_c, 'REGRESSION')}")
    shutil.rmtree(os.path.dirname(night))

    # masking direction: drop a FAST baseline's row (lmdb owns readrandom)
    night = tmp_night(); drop_row(night, "lmdb", "readrandom")
    rc_v, _ = run("verify-ledger", "--night", night)
    rc_c, out_c = run("check", "--bands", BANDS, "--night", night)
    gap_lines = [l for l in out_c.splitlines() if l.startswith("  BASELINE-GAP") and "readrandom" in l]
    ratio_lines = [l for l in out_c.splitlines()
                   if "readrandom" in l and not l.startswith("  BASELINE-GAP")
                   and not l.startswith("#")]
    expect("masking direction (lmdb readrandom row dropped): refused, never computed inflated",
           rc_v == 1 and rc_c == 1 and len(gap_lines) == 2 and not ratio_lines,
           f"verify={rc_v} check={rc_c} gaps={len(gap_lines)} computed_ratio_lines={len(ratio_lines)}")
    shutil.rmtree(os.path.dirname(night))

    # extras direction: an unexpected NEW workload row is a ledger-format change
    night = tmp_night()
    with open(os.path.join(night, "sqlite-normal.tsv"), "a") as f:
        f.write("brandnew\t1000\t1.0\t1000.0\t-\t-\t10.0\tinjected\n")
    rc_v, out_v = run("verify-ledger", "--night", night)
    expect("extras direction (unexpected workload row): verify-ledger exit 1 with 'unexpected='",
           rc_v == 1 and "unexpected=" in out_v, f"exit={rc_v}")
    shutil.rmtree(os.path.dirname(night))

    rc, out = run("check", "--bands", BANDS, "--night", N7)
    expect("pristine night under the strict geomean: exit 0, 0 BASELINE-GAP (no false refusals)",
           rc == 0 and summary_field(out, "BASELINE-GAP") == 0, f"exit={rc}")

    # ---------------- defect 6: baseline version identity ----------------
    print("## defect 6 — baseline VERSION identity (drift / missing / reference consistency)")

    # drift: a baseline library version change must fail verify-ledger, naming
    # the engine and both versions (the geomean denominator moved)
    night = tmp_night(); set_baseline_version(night, "sqlite-full", "3.46.0")
    rc_v, out_v = run("verify-ledger", "--night", night)
    drift_lines = [l for l in out_v.splitlines() if l.startswith("  VERSION-DRIFT")]
    expect("version drift (sqlite-full 3.45.1 -> 3.46.0): verify-ledger exit 1, exactly one VERSION-DRIFT naming the engine",
           rc_v == 1 and len(drift_lines) == 1 and "sqlite-full" in out_v
           and "3.46.0" in out_v and "3.45.1" in out_v,
           f"exit={rc_v} drift_lines={len(drift_lines)}")
    shutil.rmtree(os.path.dirname(night))

    # missing: a baseline TSV with no version header cannot be verified
    night = tmp_night(); strip_baseline_version(night, "lmdb")
    rc_v, out_v = run("verify-ledger", "--night", night)
    expect("missing version header (lmdb): verify-ledger exit 1 with VERSION-MISSING",
           rc_v == 1 and "VERSION-MISSING" in out_v and "lmdb" in out_v, f"exit={rc_v}")
    shutil.rmtree(os.path.dirname(night))

    # ChronoKV's own version is NOT gated (it legitimately changes between
    # recalibrations) — rewriting it must NOT fail verify-ledger
    night = tmp_night()
    for eng in ("chronokv-group", "chronokv-async"):
        p = os.path.join(night, eng + ".tsv")
        txt = open(p).read().replace("# chronokv_version=0.28.1", "# chronokv_version=0.29.0-dev")
        open(p, "w").write(txt)
    rc_v, out_v = run("verify-ledger", "--night", night)
    expect("chronokv version drift is NOT gated (engine versions change between recalibrations)",
           rc_v == 0, f"exit={rc_v} out={out_v.strip().splitlines()[-1:]}")
    shutil.rmtree(os.path.dirname(night))

    # internally inconsistent reference: two reference nights disagreeing on a
    # baseline version is a hard tool error, never a silent pick
    refdir = tempfile.mkdtemp(prefix="arena_gate_ref_")
    shutil.copytree(N7, os.path.join(refdir, "nightA"))
    shutil.copytree(N7, os.path.join(refdir, "nightB"))
    set_baseline_version(os.path.join(refdir, "nightB"), "rocksdb-sync", "8.10.0")
    night = tmp_night()
    rc_v, out_v = run("verify-ledger", "--night", night, "--expect-from", refdir)
    expect("inconsistent reference (two nights disagree on rocksdb-sync version): hard tool error",
           rc_v != 0 and "internally inconsistent" in out_v, f"exit={rc_v}")
    shutil.rmtree(os.path.dirname(night)); shutil.rmtree(refdir)

    # anti-vacuity: the pristine night must actually exercise the version check
    # (five baseline 'version == calibrated' ok lines), not pass by skipping it
    rc_v, out_v = run("verify-ledger", "--night", N7)
    nok = sum(1 for l in out_v.splitlines() if l.startswith("  ok") and "version == calibrated" in l)
    expect("pristine night: exit 0 AND all five baselines report version == calibrated (check ran)",
           rc_v == 0 and nok == 5, f"exit={rc_v} version_ok_lines={nok}")

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
