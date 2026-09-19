#!/usr/bin/env python3
"""v27 M3 — per-fault-kind error-path coverage report.

Roadmap anchor: "publish per-fault-kind coverage — which lines are reached
under each armed fault. An error-path line no fault ever reaches is an
untested line, and that is exactly where the last four bugs were." (C1, H3,
H1, H2 all lived in code no test executed.)

Input: a root directory whose subdirectories each hold the *.gcov files of
ONE instrumented run (the CI coverage job produces cov/<Kind>/ for every
forced-fault run over the review-regression gate, plus cov/full/ for the
unforced full suite).

Output: a markdown report on stdout (the CI job appends it to
$GITHUB_STEP_SUMMARY) and, optionally, the full ledger of UNTESTED
error-path lines to a file (uploaded as a CI artifact).

Error-path classification is a documented heuristic: an EXECUTABLE line
(gcov gave it a count) whose text matches any of ERR_PATTERNS below — the
shapes the historical bugs had (fail-stop flips, throws, error returns,
FATAL/cerr logging, errno handling). The heuristic over/under-inclusion is
accepted; the ledger is meant to be READ, and every entry is a line a
human should be able to answer "which fault reaches this?" for.

Exit codes: 0 = report produced; 2 = no usable gcov data (pipeline broken —
the CI job gates on this, NOT on the gap count: publishing the ledger is
the deliverable, failing PRs on it would gate unrelated changes).
"""
import os
import re
import sys

ERR_PATTERNS = [
    r"\bfailed_\s*=\s*true\b",       # WAL/engine fail-stop flips
    r"(\.|->)failed\s*=\s*true",     # batch fail-stop flips
    r"\bthrow\b",                    # exception raises (C1 lived here)
    r"\breturn false\b",             # error returns
    r"\bWalFailure\b",
    r"\bDatabaseFailed\b",
    r"\bStatus::Failed\b",
    r"\bFATAL\b",
    r"\berrno\b",                    # syscall error handling
    r"\bfail-stop\b",
    r"\bstd::cerr\b",                # error logging paths
]
ERR_RE = re.compile("|".join(ERR_PATTERNS))

# The whole product is two files; CKV_COV_FILES overrides the filter
# (used by pipeline probes on small translation units).
ENGINE_FILES = tuple(
    os.environ.get("CKV_COV_FILES", "chronokv.hpp,main.cpp").split(","))


def parse_gcov_dir(d):
    """{srcfile: {lineno: (executed, text)}} from a directory of *.gcov."""
    out = {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".gcov"):
            continue
        src = fn[: -len(".gcov")]
        if not any(src.endswith(e) for e in ENGINE_FILES):
            continue  # system headers etc.
        lines = {}
        with open(os.path.join(d, fn), errors="replace") as f:
            for raw in f:
                parts = raw.split(":", 2)
                if len(parts) < 3:
                    continue
                cnt, no, text = parts[0].strip(), parts[1].strip(), parts[2].rstrip("\n")
                if not no.isdigit():
                    continue                      # gcov banner/metadata lines
                if cnt in ("-", ""):
                    continue                      # non-executable (comments, decls)
                executed = not (cnt.startswith("#####") or cnt.startswith("====="))
                lines[int(no)] = (executed, text)
        if lines:
            out[src] = lines
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    gaps_path = sys.argv[2] if len(sys.argv) > 2 else None

    runs = {}
    for label in sorted(os.listdir(root)):
        d = os.path.join(root, label)
        if os.path.isdir(d) and any(f.endswith(".gcov") for f in os.listdir(d)):
            parsed = parse_gcov_dir(d)
            if parsed:
                runs[label] = parsed
    if not runs:
        print(f"fault_coverage: no gcov data under {root}", file=sys.stderr)
        return 2

    # Universe of executable lines + error-path subset (from any run; the
    # executable-line set is identical across runs of the same binary).
    universe, err_set = {}, {}
    for files in runs.values():
        for src, lines in files.items():
            for no, (_, text) in lines.items():
                universe.setdefault(src, {})[no] = text
                if ERR_RE.search(text):
                    err_set[(src, no)] = text

    # Forced-fire counts from each run's log (the CI driver stores it as
    # cov/<label>/run.log). A kind with 0 fires under a vehicle is VACUOUS
    # for that vehicle — its "coverage" is just the suite's own — and the
    # report must show that instead of hiding it (measured: OpenFail and
    # DirFsyncFail fire 0 times under the review-gate vehicle because
    # their sites live in checkpoint paths the gate never enters; the
    # nightly full-suite vehicle covers them).
    fires = {}
    for label in runs:
        lg = os.path.join(root, label, "run.log")
        fires[label] = None
        if os.path.exists(lg):
            with open(lg, errors="replace") as f:
                m = re.findall(r"coverage-fault: fired (\d+) times", f.read())
                if m:
                    fires[label] = int(m[-1])

    exec_lines, exec_err = {}, {}
    for label, files in runs.items():
        ea, ee = set(), set()
        for src, lines in files.items():
            for no, (ex, _) in lines.items():
                if ex:
                    ea.add((src, no))
                    if (src, no) in err_set:
                        ee.add((src, no))
        exec_lines[label], exec_err[label] = ea, ee

    union_exec = set().union(*exec_lines.values())
    union_err = set().union(*exec_err.values())
    gaps = sorted(err_set.keys() - union_err)

    fault_labels = [l for l in runs if l != "full"]
    other_union = {}
    for label in fault_labels:
        rest = set().union(*[exec_err[o] for o in fault_labels if o != label]) \
            if len(fault_labels) > 1 else set()
        rest |= exec_err.get("full", set())
        other_union[label] = exec_err[label] - rest

    total_exec = sum(len(v) for v in universe.values())
    out = []
    out.append("## Error-path coverage (v27 M3 — per fault kind)")
    out.append("")
    out.append(f"Executable lines (engine files): **{total_exec}** · "
               f"error-path lines (heuristic): **{len(err_set)}** · "
               f"reached by >=1 run: **{len(union_err)}** "
               f"({100.0 * len(union_err) / max(1, len(err_set)):.1f}%) · "
               f"**UNTESTED error-path lines: {len(gaps)}**")
    out.append("")
    out.append("| run | forced fires | lines executed | line % | error-path executed | unique error-path |")
    out.append("|---|---:|---:|---:|---:|---:|")
    for label in sorted(runs):
        pct = 100.0 * len(exec_lines[label]) / max(1, total_exec)
        uniq = len(other_union.get(label, ())) if label != "full" else 0
        fc = fires.get(label)
        fcell = "—" if fc is None else (f"**{fc} (VACUOUS)**" if fc == 0 else str(fc))
        out.append(f"| `{label}` | {fcell} | {len(exec_lines[label])} | {pct:.1f}% | "
                   f"{len(exec_err[label])} | {uniq if label != 'full' else '—'} |")
    out.append("")
    out.append("_`unique error-path` = lines ONLY that fault kind reaches — the "
               "per-kind value the roadmap asks for; `full` is the unforced "
               "full-suite run._")
    if gaps:
        out.append("")
        out.append(f"First 15 of {len(gaps)} untested error-path lines "
                   "(full ledger in the artifact):")
        out.append("")
        out.append("```")
        for src, no in gaps[:15]:
            out.append(f"{src}:{no}: {err_set[(src, no)].strip()[:110]}")
        out.append("```")
    report = "\n".join(out)
    print(report)

    if gaps_path:
        with open(gaps_path, "w") as f:
            f.write(f"# UNTESTED error-path lines ({len(gaps)}) — no forced-fault "
                    f"run and no full-suite run executed these.\n"
                    f"# Heuristic: {', '.join(ERR_PATTERNS)}\n")
            for src, no in gaps:
                f.write(f"{src}:{no}: {err_set[(src, no)].strip()}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
