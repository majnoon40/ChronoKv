#!/usr/bin/env python3
"""test_tsan_reports.py — self-tests (positive + negative controls) for the
TSan UNSUPPRESSED report-class ratchet (scripts/tsan_reports.py), the finding-#5
blind-spot backstop. Stdlib only; no network; no sanitizer run needed — the
fixtures are TSan report blocks in the REAL gcc libtsan output format
(WARNING header + '#N <demangled> <file>:<line> (<module>+<off>) (BuildId: …)'
frame lines), captured from an actual unsuppressed run of the 711cd00
role-flip scratch driver on g++ 14.2 (2 reports were observed on the
original driver; the artifact block below is verbatim from that capture),
so this runs on every push/PR alongside scripts/test_arena_gate.py and
gates there (ci.yml job `arena-gate-selftest`).

If this suite is red, the report-class ratchet's verdicts are not trustworthy
until it is green again. The controls, one per ratchet property:

  A  known artifact: the captured LatchTable role-flip block is baselined,
     then re-seen -> exit 0, RATCHET OK.
  B  recompile stability: the SAME block re-rendered with different module
     name, +offsets, BuildId, source path and pid yields the SAME signature
     (normalization: no addresses, paths, or line numbers) -> known, exit 0.
  C  always-fail: a data-race block in the real captured format -> exit 1,
     FATAL (and `baseline` REFUSES to write it — the leg can never be taught
     to ignore a race).
  D  engine-mutex deadlock (no LatchTable frame): a NEW lock-order class ->
     exit 1, NEW.
  E  the blind spot itself: a lock-order-inversion block carrying a
     LatchTable frame AND engine-mutex frames (chronokv::WalSegments::writer_loop)
     — the exact mixed inversion docs/tsan.supp records as suppressed — has a
     DIFFERENT symbol set than the artifact's -> NEW -> exit 1. This is the
     control that proves the unsuppressed leg would catch what the suppression
     hides.
  F  shrink direction: a baseline entry that disappears from later logs is
     fine (signatures may shrink, never grow) -> exit 0.
  G  zero reports: an empty (report-less) log -> exit 0 with the
     suppression-retirable note, never a false red.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "tsan_reports.py")

FAILURES = []


def expect(label, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + label + (f"  [{detail}]" if detail else ""))
    if not ok:
        FAILURES.append(label)


def run(sub, *args):
    p = subprocess.run([sys.executable, TOOL, sub, *args],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


# ---------------------------------------------------------------------------
# Fixtures — REAL gcc libtsan format, verbatim from the captured unsuppressed
# run of the 711cd00 role-flip driver (build/tsan flags, g++ 14.2); the
# engine-mutex and mixed blocks are fabricated on the same frame grammar with
# real engine class names (WalSegments::writer_loop — the writer thread the
# suppression's blind spot names).
# ---------------------------------------------------------------------------
ARTIFACT_FRAMES = """    #0 pthread_rwlock_rdlock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1476 (libtsan.so.2+0x5387c) (BuildId: 99ef88596cb10a8ccf307fe1a9070f66a44b1624)
    #1 __glibcxx_rwlock_rdlock /usr/include/c++/14/shared_mutex:83 (tsan_driver+0x1227c) (BuildId: 4d1ae9e83408dd932e73c4d5fa366ee65a90372f)
    #2 std::__shared_mutex_pthread::lock_shared() /usr/include/c++/14/shared_mutex:234 (tsan_driver+0x1227c)
    #3 std::shared_mutex::lock_shared() /usr/include/c++/14/shared_mutex:431 (tsan_driver+0x1227c)
    #4 std::shared_lock<std::shared_mutex>::shared_lock(std::shared_mutex&) /usr/include/c++/14/shared_mutex:743 (tsan_driver+0x1227c)
    #5 chronokv_btree::LatchTable::lock_shared(unsigned long) const chronokv.hpp:13392 (tsan_driver+0x1227c)
    #6 chronokv_btree::BTree::find_leaf_crabbing(unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::shared_lock<std::shared_mutex>&) const chronokv.hpp:15866 (tsan_driver+0x1227c)
    #7 chronokv_btree::BTree::get_recursive(unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >*) const chronokv.hpp:15876 (tsan_driver+0x1238b)
    #8 chronokv_btree::BTree::get(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >*) const chronokv.hpp:13641 (tsan_driver+0x1c4c4)
    #9 chronokv_btree::BTree::put(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&) chronokv.hpp:13581 (tsan_driver+0x1c4c4)
    #10 main /home/z/my-project/scripts/tsan_driver.cpp:15 (tsan_driver+0x2a69)"""


def artifact_log(pid="2283"):
    half = f"  Mutex M1 acquired here while holding mutex M0 in main thread:\n{ARTIFACT_FRAMES}\n"
    return ("==================\n"
            f"WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid={pid})\n"
            "  Cycle in lock order graph: M0 (0x721000000040) => M1 (0x721000000080) => M0\n\n"
            + half + "\n"
            + "    Hint: use TSAN_OPTIONS=second_deadlock_stack=1 to get more informative warning message\n\n"
            + half.replace("M1 acquired here while holding mutex M0", "M0 acquired here while holding mutex M1")
            + "SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) "
              "in /tmp/a/tsan_driver\n"
            + "oracle: found=400 scan=400\n")


def make_recompiled():
    f = (ARTIFACT_FRAMES
         .replace("tsan_driver+0x1227c", "test+0x9e2f1")
         .replace("tsan_driver+0x1238b", "test+0x9e4a2")
         .replace("tsan_driver+0x1c4c4", "test+0x18a07")
         .replace("tsan_driver+0x2a69", "test+0x3f15")
         .replace("tsan_driver", "test")
         .replace("chronokv.hpp", "/home/runner/work/ChronoKv/ChronoKv/chronokv.hpp")
         .replace("4d1ae9e83408dd932e73c4d5fa366ee65a90372f", "b7c31f9052aa0c47d0e6e1f8b4a2c9d3e5f60718")
         .replace("/home/z/my-project/scripts/tsan_driver.cpp", "/home/runner/work/ChronoKv/ChronoKv/main.cpp"))
    half = f"  Mutex M1 acquired here while holding mutex M0 in main thread:\n{f}\n"
    return ("==================\n"
            "WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=9021)\n"
            "  Cycle in lock order graph: M0 (0x721000000040) => M1 (0x721000000080) => M0\n\n"
            + half + "\n"
            + half.replace("M1 acquired here while holding mutex M0", "M0 acquired here while holding mutex M1")
            + "SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) "
              "in /home/runner/work/ChronoKv/ChronoKv/build/tsan/test\n")


RACE_LOG = """==================
WARNING: ThreadSanitizer: data race (pid=2289)
  Read of size 4 at 0x557ac913e09c by thread T2:
    #0 operator() /home/z/my-project/scripts/tsan_race.cpp:13 (tsan_race+0x124d) (BuildId: ca6db97563e9f149beba2dcd0b19e7cc86dc859a)
    #1 __invoke_impl<void, main()::<lambda()> > /usr/include/c++/14/bits/invoke.h:61 (tsan_race+0x124d)
    #6 <null> <null> (libstdc++.so.6+0xe1223) (BuildId: 133b71e0013695cc7832680a74edb51008c4fc4c)

  Previous write of size 4 at 0x557ac913e09c by thread T1:
    #0 operator() /home/z/my-project/scripts/tsan_race.cpp:12 (tsan_race+0x12d4) (BuildId: ca6db97563e9f149beba2dcd0b19e7cc86dc859a)

  Location is global 'cell' of size 4 at 0x557ac913e09c (tsan_race+0x409c)
SUMMARY: ThreadSanitizer: data race in /tmp/a/tsan_race
"""

# A lock-order-inversion with ENGINE-mutex frames only (no LatchTable): never
# suppressed today, and must be NEW to the baseline (fabricated on the real
# frame grammar; class names are the engine's own).
ENGINE_MUTEX_LOG = """==================
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=4410)
  Cycle in lock order graph: M0 (0x721000000140) => M1 (0x721000000180) => M0

  Mutex M1 acquired here while holding mutex M0 in main thread:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1331 (libtsan.so.2+0x4f1a2)
    #1 chronokv::WalSegments::writer_loop() chronokv.hpp:4927 (test+0x1f4a0)
    #2 chronokv::WalSegments::writer_thread_main() chronokv.hpp:4901 (test+0x1f4a0)
    #3 main /home/runner/work/ChronoKv/ChronoKv/main.cpp:410 (test+0x2b6a1)

  Mutex M0 acquired here while holding mutex M1 in main thread:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1331 (libtsan.so.2+0x4f1a2)
    #1 chronokv::ChronoKV::commit_txn(chronokv::WriteTransaction&) chronokv.hpp:11044 (test+0x17c3e)
SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) in /tmp/a/test
"""

# The blind spot itself: a mixed page-latch x engine-mutex inversion — carries
# a LatchTable frame (so today's suppression swallows it) AND engine frames.
MIXED_LOG = """==================
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=4411)
  Cycle in lock order graph: M0 (0x721000000240) => M1 (0x721000000280) => M0

  Mutex M1 acquired here while holding mutex M0 in main thread:
    #0 pthread_rwlock_rdlock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1476 (libtsan.so.2+0x5387c)
    #1 chronokv_btree::LatchTable::lock_shared(unsigned long) const chronokv.hpp:13392 (test+0x1227c)
    #2 chronokv_btree::BTree::find_leaf_crabbing(unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::shared_lock<std::shared_mutex>&) const chronokv.hpp:15866 (test+0x1227c)
    #3 chronokv::ChronoKV::get(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&) chronokv.hpp:9050 (test+0x14b22)

  Mutex M0 acquired here while holding mutex M1 in main thread:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1331 (libtsan.so.2+0x4f1a2)
    #1 chronokv::WalSegments::group_append(chronokv::WalBatch&) chronokv.hpp:5310 (test+0x1f4a0)
SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) in /tmp/a/test
"""


def main():
    tmp = tempfile.mkdtemp(prefix="tsan_reports_selftest_")
    try:
        art = os.path.join(tmp, "artifact.log")
        with open(art, "w", encoding="utf-8") as f:
            f.write(artifact_log())
        recompiled = os.path.join(tmp, "recompiled.log")
        with open(recompiled, "w", encoding="utf-8") as f:
            f.write(make_recompiled())
        race = os.path.join(tmp, "race.log")
        with open(race, "w", encoding="utf-8") as f:
            f.write(RACE_LOG)
        engine = os.path.join(tmp, "engine_mutex.log")
        with open(engine, "w", encoding="utf-8") as f:
            f.write(ENGINE_MUTEX_LOG)
        mixed = os.path.join(tmp, "mixed.log")
        with open(mixed, "w", encoding="utf-8") as f:
            f.write(MIXED_LOG)
        empty = os.path.join(tmp, "empty.log")
        with open(empty, "w", encoding="utf-8") as f:
            f.write("oracle: found=400 scan=400\n")
        base = os.path.join(tmp, "base.json")

        print("## A — the captured role-flip artifact baselines and re-checks green")
        rc, out = run("baseline", "--log", art, "--out", base)
        expect("baseline writes exactly the LatchTable signature, latchtable=True",
               rc == 0 and "lock-order-inversion" in out and "latchtable=True" in out,
               f"exit={rc}")
        rc, out = run("check", "--log", art, "--baseline", base)
        expect("the same artifact re-seen: RATCHET OK, exit 0",
               rc == 0 and "known artifact signatures seen: 1" in out
               and "RATCHET OK" in out, f"exit={rc}")

        print("## B — recompile stability (addresses/paths/BuildId/pid all differ)")
        rc, out = run("check", "--log", recompiled, "--baseline", base)
        expect("re-rendered block is KNOWN (normalization strips the volatile tokens)",
               rc == 0 and "NEW signatures: 0" in out, f"exit={rc}")

        print("## C — a data race is always-fail, in check AND in baseline")
        rc, out = run("check", "--log", race, "--baseline", base)
        expect("data race: exit 1, FATAL, named",
               rc == 1 and "FATAL" in out and "data race" in out, f"exit={rc}")
        rc, out = run("baseline", "--log", race, "--out", os.path.join(tmp, "never.json"))
        expect("baseline REFUSES to record a data race (the leg cannot be taught to ignore one)",
               rc == 1 and "refusing to baseline" in out, f"exit={rc}")

        print("## D — an engine-mutex-only inversion is NEW (never baselined)")
        rc, out = run("check", "--log", engine, "--baseline", base)
        expect("engine-mutex deadlock: exit 1, NEW, symbols listed",
               rc == 1 and "NEW" in out and "WalSegments" in out, f"exit={rc}")

        print("## E — the blind spot: a LatchTable x engine-mutex MIXED inversion is NEW")
        rc, out = run("check", "--log", mixed, "--baseline", base)
        expect("mixed latch+engine-mutex inversion: exit 1, NEW (exactly what the suppression hides)",
               rc == 1 and "NEW" in out, f"exit={rc}")
        rc, out = run("baseline", "--log", mixed, "--out", os.path.join(tmp, "never2.json"))
        expect("baseline would record the mixed class only under maintainer eyes (writes; the"
               " calibration step is where it is refused)",
               rc == 0, f"exit={rc}")

        print("## F — shrink direction: a baseline entry that disappears is fine")
        rc, out = run("check", "--log", art, "--baseline", base)
        # baseline holds exactly the artifact signature; the recompiled log is
        # the same signature — re-check via the empty-of-extra direction:
        # a SECOND signature in the baseline that the log lacks must stay green.
        with open(base, encoding="utf-8") as f:
            import json
            d = json.load(f)
        d["signatures"]["deadbeef0000000"] = {
            "type": "lock-order-inversion (potential deadlock)",
            "symbols": ["chronokv_btree::BTree::scan_all(const"], "count": 3}
        grown = os.path.join(tmp, "grown.json")
        with open(grown, "w", encoding="utf-8") as f:
            json.dump(d, f)
        rc, out = run("check", "--log", art, "--baseline", grown)
        expect("shrink is OK: extra baseline entry absent from the log, exit 0",
               rc == 0 and "RATCHET OK" in out, f"exit={rc}")

        print("## G — zero reports is informative, never a false red")
        rc, out = run("check", "--log", empty, "--baseline", base)
        expect("report-less log: exit 0 with the zero-reports note",
               rc == 0 and "zero reports" in out, f"exit={rc}")

        print(f"# {'SELFTEST FAILED: ' + str(len(FAILURES)) + ' control(s): ' + '; '.join(FAILURES) if FAILURES else 'tsan report-class ratchet self-tests: ALL CONTROLS PASS'}")
        return 1 if FAILURES else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
