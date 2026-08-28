// chronokv.hpp — ChronoKV engine and public C++ API.
// v23 shipped: epoch-pinned deferred reclamation replaced the exclusive
// gc_scan_mu_ GC/scan lock entirely (see the v23 design section further
// below for the full E1-E16 safety argument and Phase A-D history).
// v24 SHIPPED: closure/verification release -- confirming v23 solved
// the latency problem it was built for, closing gaps found during review
// (stale documentation, build-config issues, partial fixes), no new
// architecture. All twelve v24 fixes are applied and verified under
// Release / ASan+UBSan / TSan / Stress; the hooks-off public-API smoke
// (public_api_smoke.cpp, 28 checks) passes against the same header.
//
// v24 fix list (closed):
//   #1  WAL cts-reservation freeze + #1c multi-key aggregate-size residual
//   #2  leader-path exception safety
//   #3  torn WAL tail truncation
//   #4  mixed Sync/Async batch truncation
//   #5  recover_on_open clock-seeding
//   #6  start_gc TOCTOU
//   #7  wake_gc lost-wakeup
//   #8  sscanf over-matching (replaced with strict parse_seg_filename)
//   #9  verify_checkpoint_chain gap-detection via directory scan +
//       orphaned-.tmp directory-scan cleanup (second half)
//   #10 checkpoint nk plausibility check
//   #11 Transaction move-assignment (Option C: target's own abort())
//   #12 phantom-reader registration race (Shape C: acquire_slot_with_phantom
//       + prune_phantom_tracker consulting reader_slots_ under reader_mu_)
//
// Baseline: v21 (public C++ API, Python bindings, docs, packaging) plus the
//           post-M4 ConflictError/InvalidState fix and the M0 hygiene patch,
//           v22 M0/M2/M3/M4.1 (v22 M1 was a measured NEGATIVE RESULT -- see
//           the "v22 M1 historical record" section below -- superseded by
//           v23's epoch-pinned reclamation), and v23 Phases A-D.
//           All tests clean under Release / ASan+UBSan / TSan / Stress; the
//           full pytest suite is green.
//
// v22 THEME: every item is anchored to a measurement already in-tree or a
// limitation already written into README.md. No speculative work. v20 made
// the engine invariant-centric; v21 productized it; v22 finishes the arc by
// fixing the one measured performance gap, retiring the documented API
// limitations, and surfacing the engine's existing diagnostics through the
// public API and bindings.
//
// v21 LESSONS (carried forward):
//   - READ THE SOURCE BEFORE PLANNING (v19/v20 lesson, re-proven in v21).
//   - Measure before you predict: after the hygiene patch switched the
//     bindings' durability parameter from int to DurabilityMode, plain ints
//     were expected to be rejected (pybind11 enum casters are not generally
//     int-convertible), but the installed pybind11 accepted both the int and
//     the enum form empirically. The measured result is taken as ground truth
//     and logged in docs/CHANGELOG.md; the behavior may differ across
//     pybind11 versions, so callers relying on the int form should re-verify.
//   - Bench-gated tests are invisible to the sanitizer matrix: three v21 M1
//     acceptance tests only ran under CHRONOKV_BENCH; M0 relocated them into
//     the always-run suite.
//
// v22 MILESTONES:
//   M0: Hygiene. [DONE — fix_v21_hygiene.sh]
//       - Header comment corrected; three bench-gated M1 tests relocated into
//         the always-run suite; final banner corrected; README stale
//         "Known gap" replaced and ConflictError added to the error table;
//         bindings durability parameter switched from int to DurabilityMode
//         (verified empirically: both int and enum accepted on the installed
//         pybind11 — see docs/CHANGELOG.md).
//
//   M1: GC/range-scan tail latency. [SUPERSEDED — see v23 below]
//       - Originally scoped as: split gc_once() into sub-batches and
//         release/re-acquire gc_scan_mu_ at key boundaries. Three variants
//         of this were implemented and measured in v22 M1; all three
//         REGRESSED max scan latency (up to 13.7x worse) due to
//         reader-preferring lock starvation or per-acquire overhead. This
//         is documented as a negative result, not a bug — see the v22 M1
//         section below for the three measured attempts and root causes.
//       - v23 replaced the mechanism entirely: gc_scan_mu_ no longer
//         exists. Reclamation uses epoch-pinned deferred retirement
//         (reclaim_epoch_ / retired_nodes_ / min_active_pin_epoch()) —
//         see the v23 section below for the full design (E1-E16).
//
//   ---- v22 M1 historical record (negative result, kept for reference) ----
//   Three lock-tuning variants were implemented against the v20 M5
//   baseline (max scan stall 238,358 ns) and measured:
//     A1: shared_mutex, release every 16 keys -> 2,021,994 ns (8.5x worse).
//         GC starved: reader-preferring shared_mutex let concurrent scans
//         keep winning the shared lock, so GC's re-acquire after release
//         kept losing.
//     A2: FairSharedMutex, release every 16 keys -> 439,084 ns (1.8x
//         worse), p50 regressed 3.3x. Per-acquire mutex+condvar overhead
//         on the scan hot path outweighed the reduced hold time.
//     A3: shared_mutex, single release per pass -> 3,262,642 ns (13.7x
//         worse, the worst of the three). A single release point still
//         starved under continuous scan load, and the longer hold between
//         the one release made the worst-case stall larger, not smaller.
//   Conclusion: no amount of lock-release tuning fixed this without either
//   starving GC or adding unacceptable per-acquire overhead. This result
//   is what motivated the v23 epoch-pinned redesign (remove the lock
//   entirely) rather than continuing to tune it. All three attempts were
//   reverted; none of this code exists in the current file.
//
//   M2: Retire the Transaction::range_scan read-your-writes limitation.
//       [README limitation #1]
//       - Problem: Transaction::range_scan() delegates to the snapshot-only
//         engine scan and omits the transaction's own buffered writes.
//       - Fix: overlay the transaction write-set (ws_) onto the engine scan
//         inside ReadWriteTransaction::range_scan: suppress buffered deletes,
//         insert/override buffered writes, keep lexicographic order. The
//         range is still registered in range_reads_ for phantom detection
//         exactly as today; the overlay is read-side only and does not alter
//         SSI validation.
//       - Acceptance: buffered insert visible / delete hidden / override
//         returns new value, each tested at the inclusive-hi boundary; the
//         phantom regression suite (v17 WS-A + v20.1 blocker #3) stays green;
//         differential test vs an oracle with the write-set applied; README
//         limitation #1 removed.
//       - New invariant: T1 (read-your-writes) — within an active
//         transaction, range_scan reflects the current buffered write-set
//         overlaid on the snapshot.
//
//   M3: Expose diagnostics through the public API + bindings.
//       [completes the v20 M1 arc]
//       - Problem: v20 M1 built per-instance wal_stats/gc_stats/epoch_stats/
//         health/published_watermark on the engine, but none of it is
//         reachable through chronokv::Database or Python.
//       - Fix: add read-only stats()/health()/published_watermark() to
//         chronokv::Database returning small value structs; mirror them in
//         Python. Pure plumbing over existing tested methods — no engine
//         changes, no new locking.
//       - Acceptance: stats-accuracy test through the public API (v20 M1
//         pattern at the Database level); Python test asserting sane
//         health()/stats(); regression green.
//
//   M4: Fail-fast inter-process guard. [DELIVERED IN v22.1]
//       - README limitation #2 (no inter-process locking), resolved via
//         an advisory flock() acquired in the WalSegments constructor
//         (see the inter-process guard comment there). A second open on
//         the same wal_dir now throws instead of silently corrupting
//         the WAL.
//
//   Release: CHRONOKV_VERSION bumped to 0.22.0 for the M1-M3 arc (minor:
//       M3 added API surface, M2 changed range_scan behavior — logged in
//       docs/CHANGELOG.md), then to 0.22.1 (M4 inter-process guard) and
//       0.22.2 (post-release hygiene fixes). The CHRONOKV_VERSION constant
//       below is the source of truth for the currently shipped version;
//       v23 work is in progress on top of the 0.22.2 baseline.
//
// VALIDATION METHODOLOGY (mapped to milestones):
//   M1  -> before/after benchmark + TSan gate + regression
//   M2  -> differential test + phantom regression + new unit tests
//   M3  -> stats-accuracy + bindings test
//   all -> full regression gate at every milestone; four-config sanitizer
//          matrix at release.
//
// NEW CANONICAL INVARIANT (M2):
//   T1 (read-your-writes): within an active transaction, range_scan reflects
//       the current buffered write-set overlaid on the snapshot. (Existing
//       invariants R1, I2, I3, I6, D1, R-REBASE are unchanged — see the v20
//       section below.)
//
// IN-FLIGHT BUG CONVENTION (carried from v20):
//   1. Correctness/durability bugs found during a milestone are in-place
//      blockers — fixed within v22, never deferred.
//   2. Scope-expanding fixes get their own sub-milestone number (M2a, ...),
//      not silently absorbed.
//   3. v22.1 is reserved for bugs found after v22 ships, and for the deferred
//      M4 inter-process guard.
//   4. Any on-disk format change requires a documented migration rule before
//      implementation. (No format changes planned in v22.)
//
// DEFERRED TO v23+ (unchanged from v20):
//   - WAL partitioning (high risk, LSN contiguity complications).
//   - Lock-free skiplist / B+tree index (unmeasured bottleneck).
//   - EBR / hazard-pointer GC rewrite (would discard the TSan-clean audit).
//   - Raft replication with leader election/failover (multi-version project).
//   - Full mechanism/policy separation + state-machine lifecycle.
//
// =====================================================================
// v25.1 M1.5 — DOCUMENTED GAPS (not bugs; scope decisions, tracked here so
// they surface in every milestone review until closed):
//
//   GAP-EF1 (pybind11 Python bindings): The v25.1 plan stated M1.5 would
//     "update pybind11 bindings + pytest". The two-file project (this header
//     + main.cpp) contains NO pybind11 module — no PYBIND11_MODULE, no
//     #include <pybind11/...>, no .py file. The Makefile compiles only
//     main.cpp with no pybind11 target. The v21-era bindings referenced in
//     the header comments above are not present in this working copy.
//     DECISION: defer entirely. Building pybind11 infrastructure as a side
//     effect of M1.5 would expand scope into a third file + build target
//     + Python test harness, none of which belongs in the async-first API
//     milestone. Bindings should track every milestone per the v25.1 intent,
//     so this is a real gap against that intent — flagged as the FIRST item
//     to address before or during whichever milestone next requires them.
//     M8 (ecosystem: schema/indexes/planner) explicitly needs them; an
//     earlier natural point (e.g. M5 async public API finalization, or M6
//     observability surfacing) is preferable. DO NOT let this gap reach M8
//     without being closed.
//
//   GAP-EF2 (Python async decision): The v25.1 plan stated M1.5 would make
//     "an explicit Python async decision (sync bridge now, awaitables
//     deferred — documented, not silently decided)". No such documentation
//     exists — it was never written. This gap is coupled to GAP-EF1: the
//     decision can't be documented in-code until bindings exist. When
//     bindings are added (closing GAP-EF1), the Python async decision MUST
//     be written at that point: sync bridge over the C++ sync API now;
//     Python awaitables deferred until the C++ coroutine Task<T> layer lands
//     (M1.6/M4 — see the run_async_benchmark docblock in main.cpp for why
//     Task<T> was deferred). The sync bridge is the right M1.5 answer
//     because the C++ async path itself is a thin std::async layer whose
//     benefit is limited for fast in-memory ops (diagnosed empirically,
//     ratio=0.256 for writes).
//
// =====================================================================
// v20: Operational maturity release — invariant-centric hardening.
//
// Baseline (historical, v20): chrono_kv_v19.cpp (RELEASE, all 68 tests
//           clean under Release / ASan+UBSan / TSan / Stress). The engine
//           has since been consolidated into this single chronokv.hpp file;
//           chrono_kv_v19.cpp no longer exists as a separate build target.
//
// v20 THEME: shift from feature-centric to invariant-centric. v19 proved the
// engine is correct; v20 makes the implicit specification explicit, the
// diagnostics per-instance and queryable, and the rebase path hardened and
// crash-tested. Every change answers one question: "which invariant does this
// preserve, and how do we verify it?"
//
// v19 LESSONS (carried forward):
//   - Differential testing catches silent correctness bugs.
//   - Soak testing catches latent concurrency races.
//   - Crash matrices catch durability gaps.
//   - Fuzzing catches decoder/parser robustness gaps.
//   - Each technique catches a different bug class; none is sufficient alone.
//   - READ THE SOURCE BEFORE PLANNING. v20 was scoped by fact-checking v19,
//     not by assuming gaps. (This corrected two false premises: the multi-key
//     transaction API already exists; the delta chain is already bounded by
//     REBASE_AFTER_DELTAS = 100.)
//
// v20 MILESTONES:
//   M0: Baseline freeze from v19 RELEASE (68 tests, 4 configurations clean).
//
//   M1: Per-instance diagnostics + introspection API.
//       - Migrate diag:: global atomic counters into ChronoKV member state.
//         (Pure refactor, no new concurrency. Prerequisite for M1's API.)
//       - ChronoKV::stats()  -> structured snapshot: WAL bytes/records,
//         checkpoint base + delta count, GC passes/max-pass-us, publication
//         watermark, version-chain depth, reader-slot occupancy, epoch and
//         phantom-tracker entries, rebase fallback count.
//       - ChronoKV::health() -> healthy / degraded / failing, with reasons.
//       - Multi-instance isolation test: two instances, independent counters.
//       - Acceptance: stats accuracy test after known operation sequences.
//
//   M2: INVARIANTS + per-subsystem verify().  [RE-WEIGHTED: equal to M1.]
//       [DELIVERED — see the CANONICAL INVARIANTS section below.]
//       - Canonical invariants recorded in this header (the single-file
//         equivalent of INVARIANTS.md): R1, I2, I3, I6, D1, R-REBASE.
//       - Debug-only verify() functions IMPLEMENTED and exercised by tests:
//           verify_publication()      — R1 (published < clock).
//           verify_wal_dir()          — all WAL segments parse cleanly.
//           verify_checkpoint_chain() — base + deltas structurally valid.
//       - Adversarial acceptance tests prove the verifiers detect deliberate
//         WAL-byte and checkpoint-byte corruption (they are not no-ops).
//       - Not delivered in v20 (candidate follow-ups): verify_version_chains(),
//         verify_reader_slots(), a single aggregate verify_all(), and running a
//         verify pass after every test / fuzz iteration.
//
//   M3: Compaction (rebase) hardening.  [DELIVERED — scoped to what was built]
//       - R-REBASE written into the canonical invariants before code changes.
//       - Rebase trigger made byte-size-aware as well as delta-count-driven
//         (rebase_bytes_threshold_; a few huge deltas rebase sooner than many
//         tiny ones).
//       - Rebase installs a new full base via tmp-write + fsync + ATOMIC RENAME
//         over the old base, then fsyncs the directory and deletes old deltas.
//         At any instant exactly one whole base exists — never a torn base. NO
//         footer and NO previous-generation fallback: once rename atomicity was
//         confirmed, the earlier footer/fallback design was rejected as
//         unnecessary. A corrupt or missing base fails loud (see D1, R-REBASE).
//       - Recovery (R-REBASE): orphaned .tmp files are deleted on recovery;
//         deltas whose snapshot cts is not newer than the base are stale
//         leftovers of an interrupted rebase and are skipped then deleted,
//         never applied.
//       - Tests: differential rebase (compacted base == base + delta chain),
//         stale-delta simulation, missing-active-WAL fail-loud, and
//         truncated-checkpoint fail-loud. rebase_base_installed stress_point
//         added. The 6-boundary crash matrix from the original plan was NOT
//         built; rename atomicity + the fail-loud recovery tests cover the
//         crash window instead.
//
//   M4: Multi-key transaction validation coverage extension.
//       (The ReadWriteTransaction API already exists; what's missing is
//        concurrent validation coverage, especially interaction with rebase.)
//       - Soak: 8 concurrent ReadWriteTransaction threads (mixed read / write /
//         range_scan / commit / abort) for 60s.
//       - INTERACTION SOAK VARIANT: 8 transaction threads WITH byte-size-aware
//         rebase triggering concurrently (the M3 trigger). This targets the
//         component-interaction bugs that isolated soaks miss (the v19 M2
//         lesson).
//       - Crash matrix at multi-key commit boundaries (WAL durable before
//         install, install before publish).
//       - Differential test: concurrent multi-key transactions vs serial oracle.
//       - Extend stress_point() to transaction-commit linearization points.
//
//   M5 (stretch): measured GC/range_scan contention. [DONE: measured, no fix in v20]
//       - Benchmarked gc_scan_mu_ contention with a dedicated harness:
//         concurrent range scanners + background GC (start_gc()) reclamation.
//       - RESULT (Release -O2): range_scans=45273 p50=5741ns p99=10710ns
//         max=181121ns; gc_passes_during=34; gc_reclaimed_during=18801;
//         gc_max_pass_ns=11136118 (~11.1ms).
//       - FINDING: contention is real but well-bounded. Worst scan stall
//         (181us) is ~31x p50 yet only ~1.6% of the longest GC pass, so the
//         GC_KEYS_PER_PASS / GC_STEPS_PER_PASS budgets already cap the
//         exclusive-lock window. Throughput impact is negligible (p99 ~1.9x p50).
//       - DECISION: no fix in v20 (measure-first gate). The 31x tail is logged
//         as v21 candidate #1 (see DEFERRED TO v21+ below).
//       - GOTCHA found while building the harness: the constructor does NOT
//         start the GC thread; callers must invoke start_gc() explicitly.
//
//   M6: Release — full sanitizer matrix (Release / ASan+UBSan / TSan / Stress),
//       benchmarks vs v19 baseline, no regressions, commit.
//
// VALIDATION METHODOLOGY (v19-proven, applied to M1-M5, not bolted on):
//   differential testing  -> correctness   (M3, M4)
//   soak testing          -> races         (M4, and M1/M3 under load)
//   crash matrix          -> durability    (M3, M4)
//   fuzzing               -> decoder/parser robustness (M2, M3)
//   benchmarks            -> regressions   (M6, and M5 gate)
//   regression gate       -> ALL 68 v19 tests pass at every milestone.
//
// CANONICAL INVARIANTS (M2 — the implicit spec, made explicit):
//
//   R1 (publication): published_ < clock_ always. Publication never
//       outruns allocation. Verified by verify_publication().
//   I2 (WAL contiguity): replayed records are sorted by timestamp and
//       contiguous in the committed prefix (no gaps).
//   I3 (WAL ordering): replay timestamps are non-decreasing.
//   I6 (GC safety): the memory-ordering audit justifies why the chosen
//       atomic orders suffice for safe version reclamation.
//   D1 (fail-loud durability): corruption of any artifact recovery depends on
//       — a WAL segment, the MANIFEST, or the checkpoint base/deltas — fails
//       loud (throws) rather than silently loading partial or wrong state. The
//       rebase base is installed by fsync + atomic rename, so a base is wholly
//       present or wholly absent; a corrupt or missing base is NOT silently
//       worked around (there is no fallback generation — see R-REBASE).
//   R-REBASE (rebase crash safety, M3): the rebase atomically installs a new
//       full base via tmp-write + fsync + rename. Recovery treats the newest
//       valid base as authoritative. Orphaned .tmp files (an interrupted write
//       that never renamed) are deleted on recovery. Deltas whose snapshot cts
//       is not newer than the base are stale leftovers from an interrupted
//       rebase and are SKIPPED, never applied — the base already supersedes
//       them. This closes the crash window between the base rename and the
//       old-delta deletion.
//
//   Per-subsystem verifiers (debug/test only, not on hot path):
//     verify_wal_dir()          — all WAL segments parse cleanly
//     verify_checkpoint_chain() — base + deltas structurally valid
//     verify_publication()      — R1 holds
//
// IN-FLIGHT BUG CONVENTION (agreed before implementation):
//   1. Correctness/durability bugs found during M3/M4 are in-place blockers,
//      fixed within v20 as part of that milestone. Never deferred.
//   2. Large format-change fixes get their own sub-milestone number (M3a/M4a)
//      so the scope expansion is visible, not silently absorbed.
//   3. v20.1 is reserved for bugs found AFTER v20 ships (matching v18's .1/.2
//      post-release hardening). In-flight fixes stay in-flight.
//   4. Any format change requires a migration rule documented in INVARIANTS.md
//      before implementation, not after.
//
// DEFERRED TO v21+ (measured/quantified items first, then speculative):
//   1. [MEASURED in M5] GC/range_scan tail latency. Worst-case scan stall is
//      181us (~31x p50) because GC holds gc_scan_mu_ exclusively across a full
//      key batch. Candidate fix: release/re-acquire gc_scan_mu_ between smaller
//      sub-batches so a range scan never waits for a whole GC_KEYS_PER_PASS
//      sweep; target bringing max scan latency down toward p99 (~11us) without
//      hurting GC throughput. Keep the memory-ordering audit intact; do NOT jump
//      to EBR/hazard pointers unless this cheaper, measured fix proves
//      insufficient. (This is the "make that result better in the future" item.)
//   - WAL partitioning (high risk, LSN contiguity complications).
//   - Lock-free skiplist / B+tree index (unmeasured bottleneck, huge surface).
//   - EBR / hazard-pointer GC rewrite (would discard the TSan-clean audit).
//   - Raft replication with leader election/failover (multi-version project).
//   - Full mechanism/policy separation + state-machine lifecycle (ChatGPT's
//     Stages 3 & 5) — take the cheap wins now (spec, verify(), hooks), defer
//     the architectural split until something concrete is blocked.
//
// v18 milestones:
//   M1: Incremental checkpoints
//       - Checkpoint chain: base file + delta files (base.ckpt.delta.N).
//       - First checkpoint writes a full base; subsequent checkpoints write
//         only dirty keys to a delta file.
//       - Recovery loads base + delta chain in order.
//       - A new full base supersedes and removes old delta files.
//       - Checkpoint time scales with delta size, not database size.
//
//   M2: GC incremental sweep
//       - gc_cursor_ tracks the next key index to process.
//       - Each pass processes at most GC_KEYS_PER_PASS (256) keys.
//       - gc_once() returns true if more keys remain; GC loop yields between
//         passes and re-acquires gc_active_mu_ so checkpoints interleave.
//       - Diagnostics: passes, last_keys, last_steps, max_pass_us.
//
//   M3: Batch commit optimization
//       - Batch LSN allocation: one atomic fetch_add(batch_size) per batch
//         instead of N individual fetch_add(1) calls.
//       - Pre-sized wal_frame() output buffer to avoid incremental realloc.
//       - LSN contiguity verified by recovery (gaps are rejected).
//
//   M4: Code modularization
//       - Eight logical section markers (SECTION_1 through SECTION_8).
//       - Table of contents at the top of the file.
//       - Runtime test verifies all section boundaries are intact.
//
//   M5: Replication readiness spike
//       - extract_wal_records(): reads all WAL records in LSN order.
//       - replay_records(): applies records to a follower, skipping
//         already-applied CTS values (idempotent).
//       - Replication semantics: CTS watermark determines applied records.
//
// Recovery policy: STRICT, read-only, with expected torn-tail tolerance.
//   (Inherited from v17; see v17 header for full policy.)
//
// Invariant R1 (publication):
//   published_ equals the greatest contiguous durable commit timestamp
//   beginning at the recovery base. Every timestamp in [base, published_]
//   exists exactly once in the durable log — no gaps, no duplicates.
//
// Incremental checkpoint contract (v18):
//   The checkpoint chain is: base.ckpt, base.ckpt.delta.1, .delta.2, ...
//   Each delta contains only keys modified since the previous checkpoint.
//   Recovery applies base first, then deltas in order. The final ckpt_cts
//   is the timestamp of the last applied checkpoint (base or delta).
//   A new full base checkpoint removes all existing delta files.
//
// GC budget contract (v18):
//   Each GC pass processes at most GC_KEYS_PER_PASS keys. The cursor
//   advances and wraps. Full-key atomicity: once a key is started, its
//   entire version chain is walked and old versions deleted.
//
// Batch LSN contract (v18):
//   LSNs are allocated in contiguous batches. Recovery rejects any LSN gap
//   or duplicate, ensuring batch allocation correctness.
//
// Replication contract (v18):
//   replay_records() is idempotent: records with CTS <= published are
//   skipped. Records are applied in CTS order. Gaps halt replay.
//
// Build (current, single-file: chronokv.hpp + tests/main.cpp):
//   Release: g++ -std=c++20 -pthread -O2 -DCHRONOKV_TEST_HOOKS -DCHRONOKV_FAULT_INJECTION -I. tests/main.cpp -o ckv_release && ./ckv_release
//   ASan:    g++ -std=c++20 -pthread -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DCHRONOKV_TEST_HOOKS -DCHRONOKV_FAULT_INJECTION -I. tests/main.cpp -o ckv_asan && ./ckv_asan
//   TSan:    g++ -std=c++20 -pthread -O1 -g -fsanitize=thread -DCHRONOKV_TEST_HOOKS -DCHRONOKV_FAULT_INJECTION -I. tests/main.cpp -o ckv_tsan && ./ckv_tsan
//   Stress:  g++ -std=c++20 -pthread -O2 -DCHRONOKV_STRESS -DCHRONOKV_TEST_HOOKS -DCHRONOKV_FAULT_INJECTION -I. tests/main.cpp -o ckv_stress && ./ckv_stress
//
//   NOTE (v24 M-cleanup): -DCHRONOKV_TEST_HOOKS is REQUIRED for all four of
//   the above -- tests/main.cpp calls the v23 Phase D adversarial tests
//   (test_phase_d_stalled_reader, test_phase_d_e16_reachability) inside an
//   #ifdef CHRONOKV_TEST_HOOKS guard matching their definitions in this
//   header. Building any of the four configs WITHOUT this flag skips those
//   two tests silently rather than failing to compile -- if you're
//   validating v23's reclamation safety property specifically, confirm the
//   flag is set. A build with the flag OFF approximates what a real
//   library consumer's shipping build looks like, but is not itself
//   validated by this project's own test suite yet (tracked as a v24+ gap:
//   no config currently confirms the header compiles cleanly hooks-off in
//   a multi-TU consumer scenario).

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <tuple>
#include <cstdint>
#include <climits>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <functional>
#include <future>
#include <cassert>
// v25.1 M2 Phase 2 closing: io_uring wrapper merged into this header (was
// chronokv_iouring.hpp). The system includes the wrapper needed are pulled
// in here directly so the merged code at [SECTION_2B_IO_URING] compiles
// cleanly. No behavioral change; the io_uring code lives in its own
// namespace (chronokv_iouring) and is used only by WalSegments at
// [SECTION_3_WAL_SEGMENTS].
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <sys/file.h>
#include <filesystem>
#include <random>

// ================================================================
// ChronoKV v18 — TABLE OF CONTENTS (Code Modularization)
// ================================================================
//   [SECTION_1_CRC_IO_PRIMITIVES]   CRC32, I/O helpers, fault injection
//   [SECTION_2_WAL_FRAMING]         WAL record framing and decoding
//   [SECTION_2B_IO_URING]           io_uring wrapper (merged from
//                                   chronokv_iouring.hpp at M2 close)
//   [SECTION_3_WAL_SEGMENTS]        WalSegments class (segmented WAL)
//   [SECTION_4_PUBLICATION_TRACKER] PublicationTracker (R1 invariant)
//   [SECTION_5_PHANTOM_TRACKER]     PhantomTracker (SSI phantom detection)
//   [SECTION_6_MVCC_TYPES]          Version, KeyEntry (MVCC version chains)
//   [SECTION_7_CHRONOKV_CORE]       ChronoKV main class (txn, GC, ckpt)
//   [SECTION_8_TEST_SUITE]          Test suite (main)
// ================================================================
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <sys/wait.h>
#include <cstring>



// Phase 3 item 12: benchmark directory override. Defaults to /tmp (tmpfs) so
// normal runs are unchanged; point it at a real disk with
// -DCHRONOKV_BENCH_DIR='"/abs/path"' to measure real fsync cost. Only the
// performance tests use it; correctness tests stay on /tmp.
#ifndef CHRONOKV_BENCH_DIR
#define CHRONOKV_BENCH_DIR "/tmp"
#endif

// Sanitizer detection: fork()+threads crash tests are unreliable under ASan/TSan,
// and sanitizers model memory/thread errors rather than process crashes.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
#define CKV_UNDER_SANITIZER 1
#else
#define CKV_UNDER_SANITIZER 0
#endif

// ======================== Fault injection (Phase 0 item 5) ========================
// Armed faults make specific I/O operations fail so the LIVE process must handle
// the failure (unlike crash-injection, which kills the process). Deliberately
// targets the checkpoint/rotation path (item 1's fsync_dir fix) in addition to
// the generic WAL-append path. Gated behind CHRONOKV_FAULT_INJECTION.
#ifdef CHRONOKV_FAULT_INJECTION
namespace fault {
    enum class Kind : int { None = 0, FsyncFail, WriteFail, WriteShort,
                            RenameFail, OpenFail, DirFsyncFail };
    inline std::atomic<int> armed{0};
    inline std::atomic<int> remaining{0};

    inline bool fire(Kind k) {
        if (armed.load(std::memory_order_relaxed) != static_cast<int>(k)) return false;
        int r = remaining.load(std::memory_order_relaxed);
        if (r <= 0) return false;
        remaining.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
    inline void arm(Kind k, int times = 1) {
        armed.store(static_cast<int>(k), std::memory_order_relaxed);
        remaining.store(times, std::memory_order_relaxed);
    }
    inline void disarm() {
        armed.store(0, std::memory_order_relaxed);
        remaining.store(0, std::memory_order_relaxed);
    }
}
#endif

// ======================== Deterministic stress mode ========================
// Phase 0 item 0.5: sparse, seeded yields at narrow race windows.
// Active only with -DCHRONOKV_STRESS. Contract: call stress::set_seed()
// before spawning any thread, so per-thread yield decisions reproduce
// from the seed. (Full operation+interleaving replay is Phase 2.)
#ifdef CHRONOKV_STRESS
namespace stress {
    inline std::atomic<uint64_t> g_seed{0xC0FFEE};
    inline std::atomic<uint64_t> g_thread_counter{0};

    struct ThreadRng {
        uint64_t state;
        ThreadRng() {
            uint64_t base = g_seed.load(std::memory_order_relaxed);
            uint64_t tid  = g_thread_counter.fetch_add(1, std::memory_order_relaxed);
            state = base ^ (tid * 0x9E3779B97F4A7C15ULL);
            if (state == 0) state = 0xDEADBEEFCAFEULL;
        }
        uint64_t next() {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return state;
        }
    };

    inline void set_seed(uint64_t s) {
        g_seed.store(s, std::memory_order_relaxed);
        g_thread_counter.store(0, std::memory_order_relaxed);
    }

    // v25.1 M1.6: deterministic hooks for gap tests. Two separate hooks:
    // gap_callback_put_leaf fires at "put_leaf_gap" (fence re-check test).
    // gap_callback_ensure fires at "ensure_index_put_gap" (double-allocation test).
    // Using separate hooks avoids one test consuming the other's hook.
    inline std::atomic<bool> gap_hook{false};
    inline std::function<void()> gap_callback_put_leaf{nullptr};
    inline std::function<void()> gap_callback_ensure{nullptr};
}

static inline void stress_point(const char* name) {
    (void)name;
    thread_local stress::ThreadRng rng;
    // Sparse: yield ~1/8 of the time. Widens the target window without
    // exploding the interleaving state space.
    if ((rng.next() & 7) == 0) std::this_thread::yield();
    // v25.1 M1.6: deterministic hooks for gap tests. Separate callbacks
    // for put_leaf_gap (fence re-check) and ensure_index_put_gap (double-
    // allocation race).
    if (stress::gap_hook.load(std::memory_order_acquire)) {
        if (name[0] == 'p' && stress::gap_callback_put_leaf) stress::gap_callback_put_leaf();
        else if (name[0] == 'e' && stress::gap_callback_ensure) stress::gap_callback_ensure();
    }
}
#else
static inline void stress_point(const char*) {}
#endif

// ======================== Linearizability history recorder ========================
// Phase 0 item 0.75: records every operation with begin/end wall-clock timestamps
// (the concurrency window a linearizability checker needs), thread id, logical
// timestamp (cts for writes, read_ts for reads), keys, and result. The engine
// never interprets this log; it is consumed offline. Active only with
// -DCHRONOKV_RECORD_HISTORY. NOTE: dump format is space-delimited; keys/values
// containing spaces need escaping (current test keys are simple).
#ifdef CHRONOKV_RECORD_HISTORY
namespace history {
    struct Event {
        uint64_t tid_hash;
        const char* op;
        uint64_t begin_ns;
        uint64_t end_ns;
        uint64_t lts;      // cts for writes, read_ts for reads
        std::string keys;
        std::string result;
    };
    inline std::mutex mu;
    inline std::vector<Event> log;

    inline uint64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    inline uint64_t tid_hash() {
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
    }
    inline void record(Event e) {
        std::lock_guard<std::mutex> lk(mu);
        log.push_back(std::move(e));
    }
    inline size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return log.size();
    }
    inline void dump(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu);
        std::ofstream f(path);
        f << "# tid op begin_ns end_ns lts keys result\n";
        for (auto& e : log)
            f << e.tid_hash << ' ' << e.op << ' ' << e.begin_ns << ' '
              << e.end_ns << ' ' << e.lts << ' ' << e.keys << ' '
              << e.result << '\n';
    }
}

// RAII: captures begin_ns on construction, records end_ns + event on destruction.
struct HistScope {
    history::Event ev;
    HistScope(const char* op, std::string keys = "") {
        ev.tid_hash = history::tid_hash();
        ev.op = op;
        ev.begin_ns = history::now_ns();
        ev.end_ns = 0;
        ev.lts = 0;
        ev.keys = std::move(keys);
    }
    ~HistScope() {
        ev.end_ns = history::now_ns();
        history::record(std::move(ev));
    }
    void set_lts(uint64_t v) { ev.lts = v; }
    void set_result(const std::string& r) { ev.result = r; }
};
#endif

// ======================== Passive diagnostics (Phase 0) ========================
// Cheap atomic counters incremented inline. They answer "is the system drifting
// toward a pathological state" (unbounded growth, GC falling behind, WAL
// failures), complementing the invariant assertions ("did an invariant break").
// Dumped once at the end of main.
//
// SCOPING (accepted tradeoff, decided Phase 0): these are PROCESS-WIDE globals,
// not per-ChronoKV-instance state. Every test constructs its own database, and
// the fork-based crash test's child runs in a separate address space, so dump()
// reports the high-water mark aggregated across all instances and processes in
// this binary's execution - NOT "this instance's current health." That is the
// right semantics for a test-binary smoke summary. If these ever back a live
// single-instance kv.dump_diagnostics(), the counters must first move to
// per-instance member state; until then, do not read dump() as one database's lag.
namespace diag {
    // Monotonic max: safe to call without a surrounding lock. A plain .store()
    // could regress the counter when two disjoint-key transactions interleave
    // their clock_.fetch_add and the subsequent store (the no-WAL commit path).
    inline void store_max(std::atomic<uint64_t>& a, uint64_t v) {
        uint64_t cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    inline std::atomic<uint64_t> gc_created{0}, gc_retired{0}, gc_reclaimed{0};
 inline std::atomic<uint64_t> gc_passes{0}, gc_last_keys{0}, gc_last_steps{0}, gc_last_pass_ns{0}, gc_max_pass_ns{0};
    inline std::atomic<uint64_t> pub_allocated{0}, pub_published{0};
    inline std::atomic<uint64_t> wal_batches{0}, wal_records{0}, wal_bytes{0};
    inline std::atomic<uint64_t> wal_fsyncs{0}, wal_write_fails{0}, wal_fsync_fails{0};
    inline std::atomic<uint64_t> wal_truncations{0}, wal_truncate_fails{0};
    inline std::atomic<uint64_t> async_committed_then_lost{0};  // item 8: async told success, then batch failed
    inline std::atomic<uint64_t> rec_records{0}, rec_torn{0}, rec_crc_fails{0};
    inline std::atomic<uint64_t> rec_dups{0}, rec_gaps{0};
    inline std::atomic<uint64_t> epoch_entries{0}, epoch_oldest{0}, epoch_newest{0};

    inline void dump() {
        uint64_t created = gc_created.load(), retired = gc_retired.load(),
                 reclaimed = gc_reclaimed.load();
        uint64_t allocated = pub_allocated.load(), published = pub_published.load();
        uint64_t batches = wal_batches.load(), records = wal_records.load();
        std::cout << "\n--- passive diagnostics ---\n"
            << "gc:       created=" << created << " retired=" << retired
         << " reclaimed=" << reclaimed
         << " live=" << (created - reclaimed)
         << " passes=" << gc_passes.load()
         << " last_keys=" << gc_last_keys.load()
         << " last_steps=" << gc_last_steps.load()
         << " max_pass_us=" << (gc_max_pass_ns.load() / 1000) << "\n"
            << "publish:  allocated=" << allocated << " published=" << published
            << " lag=" << (allocated > published ? allocated - published : 0) << "\n"
            << "wal:      batches=" << batches << " records=" << records
            << " avg_batch=" << (batches ? (double)records / batches : 0.0)
            << " bytes=" << wal_bytes.load() << " fsyncs=" << wal_fsyncs.load()
            << " write_fails=" << wal_write_fails.load()
            << " fsync_fails=" << wal_fsync_fails.load()
            << " truncations=" << wal_truncations.load()
            << " async_committed_then_lost=" << async_committed_then_lost.load()
            << " truncate_fails=" << wal_truncate_fails.load() << "\n"
            << "recovery: records=" << rec_records.load() << " torn_tails=" << rec_torn.load()
            << " crc_fails=" << rec_crc_fails.load()
            << " dups=" << rec_dups.load() << " gaps=" << rec_gaps.load() << "\n"
            << "epoch:    entries=" << epoch_entries.load()
            << " oldest=" << epoch_oldest.load() << " newest=" << epoch_newest.load() << "\n";
    }
}

// ======================== CRC-32 ========================

static uint32_t CRCT[256];

static void crc_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        CRCT[i] = c;
    }
}

static 
// ================================================================
// [SECTION_1_CRC_IO_PRIMITIVES]
// ================================================================
uint32_t crc32(const uint8_t* d, size_t n) {
    // Kimi review 1.1: crc_init() was only ever called by the test binary's
    // main(). A library consumer using chronokv::Database directly got a
    // silently all-zero CRC table (crc32() degenerates to a constant).
    // Fix: lazy, thread-safe init on first real use (C++11 magic statics
    // guarantee this runs exactly once, safely, under concurrent callers).
    static const bool crc_table_ready = (crc_init(), true);
    (void)crc_table_ready;
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; ++i)
        c = CRCT[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

// ======================== Checked I/O ========================

static bool write_all(int fd, const uint8_t* d, size_t n) {
    while (n > 0) {
#ifdef CHRONOKV_FAULT_INJECTION
        if (fault::fire(fault::Kind::WriteFail)) { errno = EIO; return false; }
        if (fault::fire(fault::Kind::WriteShort)) {
            if (n > 1) {  // inject a 1-byte short write; the loop retries the rest
                ssize_t sw = ::write(fd, d, 1);
                if (sw < 0) { if (errno == EINTR) continue; return false; }
                if (sw == 0) return false;
                d += sw; n -= static_cast<size_t>(sw); continue;
            }
        }
#endif
        ssize_t w = ::write(fd, d, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        d += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

static bool checked_fsync(int fd) {
#ifdef CHRONOKV_FAULT_INJECTION
    if (fault::fire(fault::Kind::FsyncFail)) { errno = EIO; return false; }
#endif
    while (::fsync(fd) != 0) { if (errno == EINTR) continue; return false; }
    return true;
}

static bool checked_close(int fd) {
    while (::close(fd) != 0) { if (errno == EINTR) continue; return false; }
    return true;
}

static bool fsync_dir(const std::string& path) {
#ifdef CHRONOKV_FAULT_INJECTION
    if (fault::fire(fault::Kind::DirFsyncFail)) { return false; }
#endif
    std::string d = path;
    size_t s = d.find_last_of('/');
    if (s == std::string::npos) d = ".";
    else if (s == 0) d = "/";
    else d = d.substr(0, s);
    int dfd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return false;
    bool ok = checked_fsync(dfd);
    checked_close(dfd);
    return ok;
}

static bool checked_rename(const std::string& from, const std::string& to) {
#ifdef CHRONOKV_FAULT_INJECTION
    if (fault::fire(fault::Kind::RenameFail)) { errno = EIO; return false; }
#endif
    return ::rename(from.c_str(), to.c_str()) == 0;
}

static void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return;
    write_all(fd, data.data(), data.size());
    checked_close(fd);
}

// ======================== WAL framing ========================
//
// v24 FIX (Group 1, Fix 1): single-source the max-record-size limit.
// MAX_KEY_BYTES and MAX_VALUE_BYTES are declared here, BEFORE wal_ser,
// so wal_ser can enforce the same limit at the serialization layer.
// commit_txn() also enforces them at the API layer (defense in depth);
// the two layers can never disagree. The previous code had MAX_VALUE_BYTES
// at 16 MiB but wal_ser's hard cap at 1 MiB, so a single 8 MiB value would
// pass the API check, throw inside wal_ser AFTER cts was reserved, and
// orphan the cts in the publication tracker (silent engine stall).
//
// wal_ser's hard cap is 1 MiB (1u << 20) including the 8-byte header.
// The largest payload that fits in 1 MiB - 8 = 0xFFFF8 bytes is:
//   8 (ts) + 4 (n) + per-entry {2 + key + 4 + value + 1}
// For a single entry with a 1-byte key, that's 8 + 4 + 2 + 1 + 4 + value + 1
// = 20 + value. So value can be at most 0xFFFF8 - 20 = 1048564 bytes.
// We use 1 MiB - 256 as a conservative cap that leaves room for the framing.
static constexpr size_t MAX_KEY_BYTES   = 65535;        // serialized as u16
static constexpr size_t MAX_VALUE_BYTES = (1u << 20) - 256;  // < 1 MiB - framing overhead
static constexpr size_t WAL_MAX_RECORD  = 1u << 20;     // hard cap on a serialized record (incl. header)

using WriteSet = std::vector<std::tuple<std::string, std::string, bool>>;

static std::vector<uint8_t> wal_ser(uint64_t ts, const WriteSet& ws) {
    std::vector<uint8_t> o;
    auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back((v >> (8*i)) & 0xFF); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((v >> (8*i)) & 0xFF); };
    auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) o.push_back((v >> (8*i)) & 0xFF); };
    u64(ts);
    u32(static_cast<uint32_t>(ws.size()));
    for (auto& [k, v, d] : ws) {
        if (k.size() > MAX_KEY_BYTES) throw std::runtime_error("key too large for serialization");
         u16(static_cast<uint16_t>(k.size()));
        for (char c : k) o.push_back(static_cast<uint8_t>(c));
        if (v.size() > MAX_VALUE_BYTES) throw std::runtime_error("value too large for serialization");
        u32(static_cast<uint32_t>(v.size()));
        for (char c : v) o.push_back(static_cast<uint8_t>(c));
        o.push_back(d ? 1 : 0);
    }
    if (o.size() + 8 > WAL_MAX_RECORD) throw std::runtime_error("WAL record too large");
    return o;
}


// ================================================================
// [SECTION_2_WAL_FRAMING]
// ================================================================
static std::vector<uint8_t> wal_frame(const std::vector<uint8_t>& p) {
    std::vector<uint8_t> o;
    o.reserve(8 + p.size());  // v18 M3: pre-size
    uint32_t len = static_cast<uint32_t>(p.size());

    std::vector<uint8_t> crc_input;
    crc_input.reserve(4 + p.size());
    for (int i = 0; i < 4; ++i) crc_input.push_back((len >> (8*i)) & 0xFF);
    crc_input.insert(crc_input.end(), p.begin(), p.end());

    uint32_t crc = crc32(crc_input.data(), crc_input.size());

    for (int i = 0; i < 4; ++i) o.push_back((len >> (8*i)) & 0xFF);
    for (int i = 0; i < 4; ++i) o.push_back((crc >> (8*i)) & 0xFF);
    o.insert(o.end(), p.begin(), p.end());
    return o;
}

// Phase 1 item 6: WAL record payload is [lsn:8][cts:8][n:4][entries].
//   lsn = physical write-order sequence, assigned by the WAL layer (WalSegments).
//   cts = MVCC commit timestamp, assigned by the clock.
// They track together today but are separate fields so future replication,
// segment merging, or a non-1-base snapshot import can diverge them without
// breaking recovery. wal_ser() still emits the MVCC part [cts][n][entries];
// the WAL layer prepends the lsn at physical-write time.
static std::vector<uint8_t> wal_prepend_lsn(uint64_t lsn, const std::vector<uint8_t>& mvcc_payload) {
    std::vector<uint8_t> full;
    for (int i = 0; i < 8; ++i) full.push_back((lsn >> (8*i)) & 0xFF);
    full.insert(full.end(), mvcc_payload.begin(), mvcc_payload.end());
    return full;
}
// Full framed record, used by tests that hand-build WAL bytes.
static std::vector<uint8_t> wal_make_record(uint64_t lsn, uint64_t cts, const WriteSet& ws) {
    return wal_frame(wal_prepend_lsn(lsn, wal_ser(cts, ws)));
}

enum class WalStatus { OK, TORN_TAIL, CORRUPT };

static std::pair<WalStatus, std::vector<std::pair<uint64_t, WriteSet>>>
wal_recover_buf(const std::vector<uint8_t>& buf) {
    std::vector<std::pair<uint64_t, WriteSet>> out;
    size_t pos = 0;

    auto parse = [&](size_t& p, uint64_t& lsn, uint64_t& ts, WriteSet& ws) -> bool {
        if (p + 8 > buf.size()) return false;

        uint32_t len = 0, crc = 0;
        for (int i = 0; i < 4; ++i) len |= static_cast<uint32_t>(buf[p+i]) << (8*i);
        for (int i = 0; i < 4; ++i) crc |= static_cast<uint32_t>(buf[p+4+i]) << (8*i);

        if (len < 20 || len > (1u << 20) || p + 8 + len > buf.size()) return false;

        {
            std::vector<uint8_t> crc_input;
            crc_input.reserve(4 + len);
            for (int i = 0; i < 4; ++i) crc_input.push_back(buf[p+i]);
            crc_input.insert(crc_input.end(), buf.begin() + p + 8, buf.begin() + p + 8 + len);
            if (crc32(crc_input.data(), crc_input.size()) != crc) return false;
        }

        size_t q = p + 8;
        const size_t end = p + 8 + len;

        auto need = [&](size_t n) {
            return q <= end && n <= end - q;
        };

        auto pu64 = [&]() {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= (uint64_t(buf[q++]) << (8*i));
            return v;
        };
        auto pu32 = [&]() {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= (uint32_t(buf[q++]) << (8*i));
            return v;
        };
        auto pu16 = [&]() {
            uint16_t v = 0;
            for (int i = 0; i < 2; ++i) v |= (uint16_t(buf[q++]) << (8*i));
            return v;
        };

        if (!need(8)) return false;
        lsn = pu64();

        if (!need(8)) return false;
        ts = pu64();

        if (!need(4)) return false;
        uint32_t n = pu32();

        ws.clear();
        for (uint32_t i = 0; i < n; ++i) {
            if (!need(2)) return false;
            uint16_t kl = pu16();

            if (!need(kl)) return false;
            std::string k;
            if (kl > 0) k.assign(reinterpret_cast<const char*>(&buf[q]), kl);
            q += kl;

            if (!need(4)) return false;
            uint32_t vl = pu32();

            if (!need(vl)) return false;
            std::string v;
            if (vl > 0) v.assign(reinterpret_cast<const char*>(&buf[q]), vl);
            q += vl;

            if (!need(1)) return false;
            bool dl = buf[q++] != 0;

            ws.push_back({k, v, dl});
        }

        if (q != end) return false;

        p += 8 + len;
        return true;
    };

    uint64_t expected_lsn = 0;
    bool have_lsn = false;

    while (pos < buf.size()) {
        uint64_t lsn, ts;
        WriteSet ws;
        size_t save = pos;

        if (parse(pos, lsn, ts, ws)) {
            if (have_lsn && lsn != expected_lsn) return {WalStatus::CORRUPT, out};
            expected_lsn = lsn + 1;
            have_lsn = true;
            out.push_back({ts, ws});
        } else {
            uint32_t len = 0;
            if (save + 4 <= buf.size())
                for (int i = 0; i < 4; ++i)
                    len |= static_cast<uint32_t>(buf[save+i]) << (8*i);

            bool valid_after = false;
            if (len >= 20 && len <= (1u << 20) && save + 8 + len <= buf.size()) {
                size_t nxt = save + 8 + len;
                uint64_t l2, t2;
                WriteSet w2;
                if (parse(nxt, l2, t2, w2)) valid_after = true;
            }

            return {valid_after ? WalStatus::CORRUPT : WalStatus::TORN_TAIL, out};
        }
    }

    return {WalStatus::OK, out};
}

static std::pair<WalStatus, std::vector<std::pair<uint64_t, WriteSet>>>
wal_recover_file(const std::string& path) {
    std::ifstream ff(path, std::ios::binary);
    if (!ff) return {WalStatus::OK, {}};
    std::vector<uint8_t> buf(
        (std::istreambuf_iterator<char>(ff)), std::istreambuf_iterator<char>());
    return wal_recover_buf(buf);
}


// ======================== TxnResult / TxnState ========================

// v17 Work Stream D: API-level size limits.
// (v24 FIX Group 1, Fix 1: MAX_KEY_BYTES / MAX_VALUE_BYTES / WAL_MAX_RECORD
// are now declared ONCE, before wal_ser, where the serialization layer can
// enforce them. See that block for the rationale. The previous 16 MiB
// MAX_VALUE_BYTES was unreachable: wal_ser throws at 1 MiB. Removing the
// duplicate definition here so the two layers cannot diverge again.)

enum class TxnResult : uint8_t {
    Committed, Conflict, InvalidState, TooLarge, WalFailure, DatabaseFailed,
    InvalidTransaction  // v20.1 blocker #6: malformed write set (duplicate keys)
};

enum class TxnState : uint8_t { Active, Committed, Aborted };

// Phase 1 item 8: durability modes.
//
// CLARIFIED (post-v23 review): Sync and Group provide the IDENTICAL
// durability guarantee through the IDENTICAL mechanism in group_append() --
// both wait for their record's batch fsync before the commit returns, and
// both participate in the same shared WAL batching/group-commit path. There
// is no per-commit "Sync fsyncs immediately, Group waits to batch" split in
// the implementation; that would be a different design this codebase does
// not currently have. An earlier version of this comment described Sync as
// an "explicit conservative mode," which incorrectly implied stricter,
// non-batched per-commit behavior. It does not exist. Measured: under a
// 4-thread contention workload, Sync and Group produced nearly identical
// records-per-fsync ratios (2.08 vs 2.25) -- consistent with both using the
// same batching mechanism, not evidence of a bug.
//
//   Group (default): durable-before-return; participates in WAL group commit
//                    (may batch with concurrent commits' fsyncs).
//   Sync           : durable-before-return; SAME mechanism as Group today.
//                    Exists as an explicit, unambiguous spelling of the
//                    durable-before-return guarantee for callers who want to
//                    say so without depending on Group being the default.
//   Async          : commit waits only for the batch write (page cache), NOT the fsync.
//                    Durable vs process crash, NOT vs power loss within the window
//                    before the next successful group fsync. Weakens the invariant
//                    from Visible = Durable to Visible >= Durable (deliberate, documented).
//                    RISK: if the subsequent fsync fails (same process, no crash), the
//                    batch is truncated and async-reported commits are silently lost.
//                    The caller is not notified (it already received Committed). This
//                    is counted in diag::async_committed_then_lost. Use async mode only
//                    if you can tolerate silent loss on fsync failure (e.g., ephemeral
//                    data, or you have an external recovery mechanism).
enum class DurabilityMode : uint8_t { Sync, Group, Async };

static const char* to_string(TxnResult r) {
    switch (r) {
        case TxnResult::Committed:          return "Committed";
        case TxnResult::Conflict:           return "Conflict";
        case TxnResult::InvalidState:       return "InvalidState";
        case TxnResult::TooLarge:           return "TooLarge";
        case TxnResult::WalFailure:         return "WalFailure";
        case TxnResult::DatabaseFailed:     return "DatabaseFailed";
        case TxnResult::InvalidTransaction: return "InvalidTransaction";
    }
    return "Unknown";
}

// ======================== WAL segment manager ========================

struct Batch {
    std::atomic<bool> has_async{false};
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> records;
    bool written = false;  // item 8: records are in the page cache (pre-fsync)
    bool done = false;     // records are fsynced (durable vs power loss)
    bool failed = false;
};


// ================================================================
// [SECTION_2B_IO_URING]
// ================================================================
// v25.1 M2 Phase 2: Minimal io_uring wrapper (no liburing dependency).
// Uses raw syscalls: io_uring_setup, io_uring_enter, mmap.
// Supports: write at explicit offset, fsync, wait for CQE with timeout.
// Falls back to sync write/fsync if io_uring is unavailable.
//
// Key design: the fallback path uses pwrite (positioned write) to the
// exact batch_start offset, NOT append semantics. This prevents the
// "two copies at different offsets" bug if the original io_uring write
// was mid-flight when the leader failed.
//
// MockIoUring: test-only subclass that simulates io_uring behavior
// (CQE success/failure/timeout) with real pwrite I/O, so the full
// state machine (submit -> wait -> success/failure/timeout -> fallback
// or skip-sync-fsync) is exercised without real kernel io_uring.
//
// ---- merge note (v25.1 M2 close) ----
// This block was previously a separate header (chronokv_iouring.hpp)
// and was merged into chronokv.hpp as [SECTION_2B_IO_URING] at M2
// closing time. The system includes it required were hoisted into the
// top-of-file include block; the #pragma once guard was dropped
// (chronokv.hpp is itself a single-translation-unit header and does
// not use include guards); the namespace (chronokv_iouring) is kept
// as-is so all qualified references (chronokv::WalSegments using
// chronokv_iouring::IoUring, etc.) continue to resolve identically.
// No behavioral change. The file chronokv_iouring.hpp no longer
// exists; the Makefile build rules no longer reference it.
// =================================================================
namespace chronokv_iouring {

class IoUring {
public:
    IoUring() : ring_fd_(-1), available_(false) {
        // v25.1 M2: io_uring init. On systems where io_uring I/O ops are
        // blocked (seccomp EPERM), available_ stays false and the sync
        // pwrite+fsync fallback is used. The `early_return` below was a
        // workaround for the k8s container dev environment — it is now
        // controlled by CKV_IOURING_DISABLED so real io_uring is attempted
        // on every platform by default.
        //
        // To re-enable the container workaround, define CKV_IOURING_DISABLED.
#ifdef CKV_IOURING_DISABLED
        return;
#endif
        memset(&params_, 0, sizeof(params_));
        ring_fd_ = syscall(__NR_io_uring_setup, 8, &params_);
        if (ring_fd_ < 0) return;

        unsigned sq_entries = params_.sq_entries;
        unsigned cq_entries = params_.cq_entries;

        sq_sz_ = params_.sq_off.array + sq_entries * sizeof(unsigned);
        sq_mmap_ = mmap(NULL, sq_sz_, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
        if (sq_mmap_ == MAP_FAILED) { ring_fd_ = -1; return; }

        sqe_sz_ = sq_entries * sizeof(struct io_uring_sqe);
        sqes_ = (struct io_uring_sqe*)mmap(NULL, sqe_sz_, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, ring_fd_, IORING_OFF_SQES);
        if (sqes_ == MAP_FAILED) { ring_fd_ = -1; return; }

        cq_sz_ = params_.cq_off.cqes + cq_entries * sizeof(struct io_uring_cqe);
        cq_mmap_ = mmap(NULL, cq_sz_, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
        if (cq_mmap_ == MAP_FAILED) { ring_fd_ = -1; return; }

        sq_head_ = (unsigned*)((char*)sq_mmap_ + params_.sq_off.head);
        sq_tail_ = (unsigned*)((char*)sq_mmap_ + params_.sq_off.tail);
        sq_mask_ = (unsigned*)((char*)sq_mmap_ + params_.sq_off.ring_mask);
        sq_array_ = (unsigned*)((char*)sq_mmap_ + params_.sq_off.array);

        cq_head_ = (unsigned*)((char*)cq_mmap_ + params_.cq_off.head);
        cq_tail_ = (unsigned*)((char*)cq_mmap_ + params_.cq_off.tail);
        cq_mask_ = (unsigned*)((char*)cq_mmap_ + params_.cq_off.ring_mask);
        cqes_ = (struct io_uring_cqe*)((char*)cq_mmap_ + params_.cq_off.cqes);

        available_ = true;
    }

    virtual ~IoUring() {
        if (sq_mmap_ && sq_mmap_ != MAP_FAILED) munmap(sq_mmap_, sq_sz_);
        if (sqes_ && sqes_ != MAP_FAILED) munmap(sqes_, sqe_sz_);
        if (cq_mmap_ && cq_mmap_ != MAP_FAILED) munmap(cq_mmap_, cq_sz_);
        if (ring_fd_ >= 0) close(ring_fd_);
    }

    virtual bool available() const { return available_; }

    // Submit a write at an explicit offset (pwrite semantics).
    virtual int submit_write(int fd, const void* buf, size_t len, off_t offset, int user_data) {
        if (!available_) return -1;
        unsigned tail = *sq_tail_;
        unsigned idx = tail & *sq_mask_;
        memset(&sqes_[idx], 0, sizeof(struct io_uring_sqe));
        sqes_[idx].opcode = IORING_OP_WRITE;
        sqes_[idx].fd = fd;
        sqes_[idx].addr = (unsigned long)buf;
        sqes_[idx].len = len;
        sqes_[idx].off = offset;
        sqes_[idx].user_data = user_data;
        sq_array_[idx] = idx;
        __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);
        return user_data;
    }

    // Submit an fsync.
    virtual int submit_fsync(int fd, int user_data) {
        if (!available_) return -1;
        unsigned tail = *sq_tail_;
        unsigned idx = tail & *sq_mask_;
        memset(&sqes_[idx], 0, sizeof(struct io_uring_sqe));
        sqes_[idx].opcode = IORING_OP_FSYNC;
        sqes_[idx].fd = fd;
        sqes_[idx].user_data = user_data;
        sq_array_[idx] = idx;
        __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);
        return user_data;
    }

    // Enter: submit pending SQEs and optionally wait for completions.
    virtual int enter(int to_submit, int min_complete, unsigned flags) {
        if (!available_) return -1;
        return syscall(__NR_io_uring_enter, ring_fd_, to_submit, min_complete,
                       flags | IORING_ENTER_GETEVENTS, NULL, 0);
    }

    // Enter with a timeout.
    virtual int enter_with_timeout(int to_submit, int min_complete, int timeout_ms) {
        if (!available_) return -1;
        // v25.1 M2: kernel 5.10 doesn't support the timeout parameter.
        return enter(to_submit, min_complete, IORING_ENTER_GETEVENTS);
    }

    // Pop a CQE.
    virtual bool pop_cqe(int* res, int* user_data) {
        if (!available_) return false;
        unsigned head = *cq_head_;
        unsigned tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
        if (head == tail) return false;
        unsigned idx = head & *cq_mask_;
        *res = cqes_[idx].res;
        *user_data = cqes_[idx].user_data;
        __atomic_store_n(cq_head_, head + 1, __ATOMIC_RELEASE);
        return true;
    }

protected:
    // v25.1 M2: protected so MockIoUring can set available_ = true.
    IoUring(bool /*mock*/) : ring_fd_(-1), available_(true) {}

    int ring_fd_;
    bool available_;
    struct io_uring_params params_;

    void *sq_mmap_ = nullptr;
    struct io_uring_sqe *sqes_ = nullptr;
    void *cq_mmap_ = nullptr;
    size_t sq_sz_ = 0, sqe_sz_ = 0, cq_sz_ = 0;

    unsigned *sq_head_ = nullptr;
    unsigned *sq_tail_ = nullptr;
    unsigned *sq_mask_ = nullptr;
    unsigned *sq_array_ = nullptr;

    unsigned *cq_head_ = nullptr;
    unsigned *cq_tail_ = nullptr;
    unsigned *cq_mask_ = nullptr;
    struct io_uring_cqe *cqes_ = nullptr;
};

// ============================================================================
// MockIoUring: test-only backend that simulates io_uring behavior.
//
// Tests the full state machine (submit -> wait -> success/failure/timeout ->
// fallback or skip-sync-fsync) with REAL pwrite I/O but INJECTABLE CQE
// results. This proves the state machine is correct without real kernel
// io_uring — which is blocked by seccomp on this VM.
//
// Mock modes:
//   MOCK_SUCCESS: submit_write does a real pwrite, pop_cqe returns res >= 0.
//     Tests: the "skip sync fsync" branch (used_iouring = true, fsync skipped).
//   MOCK_FAILURE: submit_write does a real pwrite, pop_cqe returns res < 0.
//     Tests: the "fallback pwrite at batch_start" branch.
//   MOCK_TIMEOUT: enter_with_timeout returns -1 (simulated timeout).
//     Tests: the "timeout -> fallback" branch.
//   MOCK_PARTIAL: submit_write does a PARTIAL pwrite (half the data), then
//     pop_cqe returns res < 0. Tests: the fallback pwrite must overwrite
//     the partial data at the exact same offset (offset-correctness test).
// ============================================================================
class MockIoUring : public IoUring {
public:
    enum Mode { MOCK_SUCCESS, MOCK_FAILURE, MOCK_TIMEOUT, MOCK_PARTIAL };

    MockIoUring(Mode mode) : IoUring(true), mode_(mode) {
        mock_write_res_ = (mode == MOCK_SUCCESS) ? 0 : -1;
        mock_fsync_res_ = (mode == MOCK_SUCCESS) ? 0 : -1;
    }

    // In mock mode, submit_write actually writes the data via pwrite
    // (so the file gets real content), and we record the write for
    // CQE injection.
    int submit_write(int fd, const void* buf, size_t len, off_t offset, int user_data) override {
        mock_last_fd_ = fd;
        mock_last_offset_ = offset;
        mock_last_len_ = len;

        if (mode_ == MOCK_PARTIAL) {
            // Write only half the data — simulates a partial io_uring write.
            size_t half = len / 2;
            pwrite_all_internal(fd, (const uint8_t*)buf, half, offset);
            mock_write_res_ = -1;  // report failure (partial write)
        } else if (mode_ == MOCK_SUCCESS || mode_ == MOCK_FAILURE) {
            // Write the full data via pwrite — the file gets real content.
            // This simulates io_uring successfully writing the data (even
            // if we report failure via CQE — the write may have completed
            // in the kernel before the CQE error was reported).
            pwrite_all_internal(fd, (const uint8_t*)buf, len, offset);
        }
        // MOCK_TIMEOUT: don't write at all (simulates io_uring that never completed).
        return user_data;
    }

    int submit_fsync(int fd, int user_data) override {
        if (mode_ == MOCK_SUCCESS) {
            // Do a real fsync.
            fsync(fd);
            mock_fsync_res_ = 0;
        }
        return user_data;
    }

    int enter(int to_submit, int min_complete, unsigned flags) override {
        (void)to_submit; (void)min_complete; (void)flags;
        if (mode_ == MOCK_TIMEOUT) return -1;  // simulate timeout
        return 2;  // 2 CQEs available (write + fsync)
    }

    int enter_with_timeout(int to_submit, int min_complete, int timeout_ms) override {
        return enter(to_submit, min_complete, 0);
    }

    bool pop_cqe(int* res, int* user_data) override {
        // Pop write CQE first, then fsync CQE.
        if (!mock_write_popped_) {
            *res = mock_write_res_;
            *user_data = 1;
            mock_write_popped_ = true;
            return true;
        }
        if (!mock_fsync_popped_) {
            *res = mock_fsync_res_;
            *user_data = 2;
            mock_fsync_popped_ = true;
            return true;
        }
        return false;
    }

    // Getters for test verification.
    off_t last_write_offset() const { return mock_last_offset_; }
    size_t last_write_len() const { return mock_last_len_; }

private:
    void pwrite_all_internal(int fd, const uint8_t* d, size_t n, off_t offset) {
        while (n > 0) {
            ssize_t w = ::pwrite(fd, d, n, offset);
            if (w < 0) { if (errno == EINTR) continue; return; }
            if (w == 0) return;
            d += w; n -= static_cast<size_t>(w); offset += w;
        }
    }

    Mode mode_;
    int mock_write_res_;
    int mock_fsync_res_;
    bool mock_write_popped_ = false;
    bool mock_fsync_popped_ = false;
    int mock_last_fd_ = -1;
    off_t mock_last_offset_ = -1;
    size_t mock_last_len_ = 0;
};

// pwrite_all: positioned write (not append). Used by the fallback path.
static bool pwrite_all(int fd, const uint8_t* d, size_t n, off_t offset) {
    while (n > 0) {
        ssize_t w = ::pwrite(fd, d, n, offset);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        d += w; n -= static_cast<size_t>(w); offset += w;
    }
    return true;
}

} // namespace chronokv_iouring


// ================================================================
// [SECTION_3_WAL_SEGMENTS]
// ================================================================
// v20 M1: read-only snapshot structs for the stats()/health() API.
struct WalInstanceStats {
    uint64_t batches, records, bytes, fsyncs, write_fails, fsync_fails,
             truncations, truncate_fails, async_lost, pub_allocated_max;
};
struct EpochInstanceStats { uint64_t entries, oldest, newest; };
struct GcInstanceStats {
    uint64_t created, retired, reclaimed, retired_pending, reclaim_epoch,
             oldest_active_pin_epoch, passes, last_keys, last_steps,
             last_pass_ns, max_pass_ns;
};
struct HealthInfo {
    int level;  // 0 = healthy, 1 = degraded, 2 = failing
    std::vector<std::string> reasons;
};

// RAII guard for a file descriptor.  Member destructors run even when the
// enclosing constructor throws, so this reliably closes the fd in all
// code paths — including throws from read_manifest() after the flock
// v25.1 M2 Phase 2: forward-declare chronokv::Database for friend access
// (needed before WalSegments, which is defined below).
namespace chronokv { class Database; }

// has been acquired.
struct FdGuard {
    int fd = -1;
    FdGuard() = default;
    ~FdGuard() { if (fd >= 0) ::close(fd); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

class WalSegments {
    friend class ::chronokv::Database;  // v25.1 M2: test injection access
    std::string dir_;
    int active_fd_ = -1;
    FdGuard lock_fd_;  // v22.1 M4: inter-process guard (flock), v24: RAII
    uint64_t active_id_ = 1;
    std::atomic<uint64_t> lsn_{0};  // physical write-order sequence (item 6)
    std::mutex batch_mu_;
    std::condition_variable batch_cv_;
    std::atomic<bool> failed_{false};
    std::shared_ptr<Batch> cur_batch_;
    bool leader_active_ = false;

    // v25.1 M2 Phase 1: multi-segment WAL. Fixed-size segments (64 MiB).
    static constexpr size_t SEGMENT_MAX_BYTES = 64ULL * 1024 * 1024;
    size_t active_segment_bytes_ = 0;

    // v25.1 M2 Phase 2: io_uring for WAL writes + offset-pinned fallback.
    // io_uring submits write+fsync as a chain. If io_uring is unavailable
    // or the CQE times out (500ms), the leader falls back to sync
    // pwrite at the exact batch_start offset (not append). This prevents
    // the "two copies at different offsets" bug if the original io_uring
    // write was mid-flight when the leader failed.
    std::unique_ptr<chronokv_iouring::IoUring> iouring_;

    // v25.1 M2 Phase 2: test-only method to inject a mock IoUring.
    // Used by the io_uring state-machine tests to exercise the submit →
    // wait → success/failure/timeout → fallback/skip paths without real
    // kernel io_uring (which is blocked by seccomp on this VM).
    void inject_iouring_for_test(std::unique_ptr<chronokv_iouring::IoUring> mock) {
        iouring_ = std::move(mock);
    }
    // Test-only: check if the last batch used io_uring (skipped sync fsync).
    std::atomic<bool> last_batch_used_iouring_{false};
    // v25.1 M2: stress_point hook for the CQE-wait gap test.
    // When set, the leader calls this BEFORE waiting for the CQE, allowing
    // the test to simulate a leader failure (set leader_timed_out_).
    std::function<void()> cqe_wait_hook_;
    std::atomic<bool> leader_timed_out_{false};

    // v20 M1: per-instance WAL diagnostics (dual-written with diag:: globals
    // so diag::dump() and existing tests keep working unchanged).
    std::atomic<uint64_t> i_wal_batches{0}, i_wal_records{0}, i_wal_bytes{0};
    std::atomic<uint64_t> i_wal_fsyncs{0}, i_wal_write_fails{0}, i_wal_fsync_fails{0};
    std::atomic<uint64_t> i_wal_truncations{0}, i_wal_truncate_fails{0};
    std::atomic<uint64_t> i_async_lost{0};
    std::atomic<uint64_t> i_pub_allocated_max{0};

public:
    // v20 M1: read-only snapshot; no mutable counter leakage across the boundary.
    struct WalStats {
        // v25.1 M0.6: default initializers so WalStats{} is zero-valued
        // (used by ChronoKV::dump_instance_diagnostics when wal_ is null).
        uint64_t batches = 0, records = 0, bytes = 0, fsyncs = 0,
                 write_fails = 0, fsync_fails = 0,
                 truncations = 0, truncate_fails = 0, async_lost = 0,
                 pub_allocated_max = 0;
    };
    WalStats wal_stats() const {
        return WalStats{
            i_wal_batches.load(std::memory_order_relaxed),
            i_wal_records.load(std::memory_order_relaxed),
            i_wal_bytes.load(std::memory_order_relaxed),
            i_wal_fsyncs.load(std::memory_order_relaxed),
            i_wal_write_fails.load(std::memory_order_relaxed),
            i_wal_fsync_fails.load(std::memory_order_relaxed),
            i_wal_truncations.load(std::memory_order_relaxed),
            i_wal_truncate_fails.load(std::memory_order_relaxed),
            i_async_lost.load(std::memory_order_relaxed),
            i_pub_allocated_max.load(std::memory_order_relaxed)};
    }

    // v25.1 M2 Phase 3: test-only public static accessors for inspecting the
    // on-disk WAL state without instantiating WalSegments (which would race
    // with the live engine's WalSegments and flock the WAL directory).
    //
    //   list_segment_ids_for_test(dir):
    //     Returns the sorted list of segment IDs ("wal_NNNNNN.log") currently
    //     on disk under `dir`. Used by the Phase 3 GC test to assert that
    //     segments fully covered by a checkpoint were deleted and that
    //     segments NOT yet covered survive.
    //
    //   read_manifest_for_test(dir):
    //     Returns {active_id, ckpt_ts} parsed from MANIFEST. Returns {0, 0}
    //     if MANIFEST does not exist (genuinely fresh database). Throws on
    //     corrupt MANIFEST (truncated / bad magic / CRC mismatch) — same
    //     contract as the private read_manifest() instance method.
    //
    // Both are deliberately stateless (no instance needed) so the test can
    // call them after the engine has been destroyed (post-close inspection).
    static std::vector<uint64_t> list_segment_ids_for_test(const std::string& dir) {
        std::vector<uint64_t> ids;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
            return ids;
        }
        for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            uint64_t sid = 0;
            if (parse_seg_filename(entry.path().filename().string(), &sid)) {
                ids.push_back(sid);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    static std::pair<uint64_t, uint64_t> read_manifest_for_test(const std::string& dir) {
        std::string mp = dir + "/MANIFEST";
        std::ifstream f(mp, std::ios::binary);
        if (!f) return {0, 0};  // no manifest
        std::vector<uint8_t> buf(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        if (buf.size() < 24)
            throw std::runtime_error("MANIFEST exists but is truncated: " + mp);
        uint32_t magic = 0, crc = 0;
        for (int i = 0; i < 4; ++i) magic |= static_cast<uint32_t>(buf[i]) << (8*i);
        for (int i = 0; i < 4; ++i) crc   |= static_cast<uint32_t>(buf[4+i]) << (8*i);
        if (magic != 0x4D414E46)
            throw std::runtime_error("MANIFEST exists but has bad magic: " + mp);
        if (crc32(&buf[8], buf.size() - 8) != crc)
            throw std::runtime_error("MANIFEST exists but CRC mismatch: " + mp);
        size_t q = 8;
        auto pu64 = [&]() { uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t(buf[q++])<<(8*i)); return v; };
        return {pu64(), pu64()};
    }

private:
    std::string seg_path(uint64_t id) const {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s/wal_%06llu.log",
                 dir_.c_str(), (unsigned long long)id);
        return buf;
    }

    // v24 FIX (Group 2, Fix 8): strict segment-filename match.
    // The previous sscanf("wal_%llu.log", &sid) accepted ANY number
    // of digits (including non-canonical forms like "wal_1.log" or
    // "wal_9999999999.log"), and %llu also skips leading whitespace.
    // The canonical format is "wal_NNNNNN.log" with EXACTLY 6
    // zero-padded digits (see seg_path above). A non-canonical
    // filename in the WAL directory indicates either a foreign file
    // (must be ignored) or a manual rename (must be rejected as
    // corruption during recovery). Strict matching prevents both
    // false positives (treating a foreign file as a segment) and
    // false negatives (missing segments whose names don't match the
    // sscanf pattern but would be valid).
    //
    // Returns true and sets *out_id if `fname` is exactly
    // "wal_NNNNNN.log" with NNNNNN being 6 decimal digits.
    static bool parse_seg_filename(const std::string& fname, uint64_t* out_id) {
        // Expected length: "wal_" (4) + 6 digits + ".log" (4) = 14.
        if (fname.size() != 14) return false;
        if (fname[0] != 'w' || fname[1] != 'a' || fname[2] != 'l' || fname[3] != '_') return false;
        if (fname[10] != '.' || fname[11] != 'l' || fname[12] != 'o' || fname[13] != 'g') return false;
        uint64_t id = 0;
        for (int i = 4; i < 10; ++i) {
            char c = fname[i];
            if (c < '0' || c > '9') return false;
            id = id * 10 + static_cast<uint64_t>(c - '0');
        }
        if (out_id) *out_id = id;
        return true;
    }

    std::string manifest_path() const { return dir_ + "/MANIFEST"; }

    bool write_manifest(uint64_t act_id, uint64_t ckpt_ts) {
        std::vector<uint8_t> pl;
        auto u64 = [&](uint64_t v) { for(int i=0;i<8;++i) pl.push_back((v>>(8*i))&0xFF); };
        u64(act_id);
        u64(ckpt_ts);
        uint32_t crc = crc32(pl.data(), pl.size());
        std::vector<uint8_t> buf;
        auto p32 = [&](uint32_t v) { for(int i=0;i<4;++i) buf.push_back((v>>(8*i))&0xFF); };
        p32(0x4D414E46);
        p32(crc);
        buf.insert(buf.end(), pl.begin(), pl.end());
        std::string tmp = manifest_path() + ".tmp";
        int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) return false;
        if (!write_all(fd, buf.data(), buf.size())) { checked_close(fd); return false; }
        if (!checked_fsync(fd)) { checked_close(fd); return false; }
        if (!checked_close(fd)) return false;
        if (!checked_rename(tmp, manifest_path())) return false;
        return fsync_dir(manifest_path());
    }

    std::pair<uint64_t, uint64_t> read_manifest() {
        // Kimi review 1.3: every failure mode used to collapse into the
        // same {1,0} "no manifest" sentinel, so a genuinely corrupt
        // MANIFEST was silently treated as a brand-new database. Only
        // "file does not exist" is legitimately {1,0}; any other failure
        // is corruption and must fail loud (D1), same as checkpoint files.
        std::ifstream f(manifest_path(), std::ios::binary);
        if (!f) return {1, 0};  // no manifest: genuinely new database
        std::vector<uint8_t> buf(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        if (buf.size() < 24)
            throw std::runtime_error("MANIFEST exists but is truncated: " + manifest_path());
        uint32_t magic = 0, crc = 0;
        for (int i = 0; i < 4; ++i) magic |= static_cast<uint32_t>(buf[i]) << (8*i);
        for (int i = 0; i < 4; ++i) crc   |= static_cast<uint32_t>(buf[4+i]) << (8*i);
        if (magic != 0x4D414E46)
            throw std::runtime_error("MANIFEST exists but has bad magic: " + manifest_path());
        if (crc32(&buf[8], buf.size() - 8) != crc)
            throw std::runtime_error("MANIFEST exists but CRC mismatch: " + manifest_path());
        size_t q = 8;
        auto pu64 = [&]() { uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t(buf[q++])<<(8*i)); return v; };
        return {pu64(), pu64()};
    }

    // Item 6 fix: scan the existing segment to find the max LSN, so we resume
    // from it rather than resetting to 0. Without this, a restart without
    // checkpoint appends LSN 1,2,3 after LSN 498,499,500, and recovery sees
    // a gap (1 != 501) and reports false corruption.
    uint64_t find_max_lsn_in_segment(uint64_t id) {
        std::string p = seg_path(id);
        auto [status, records] = wal_recover_file(p);
        if (status == WalStatus::CORRUPT || records.empty()) return 0;
        // The last record has the max LSN (records are in file order, LSNs increase)
        // We need to re-parse to get the LSN, since wal_recover_file only returns (cts, ws)
        std::ifstream f(p, std::ios::binary);
        if (!f) return 0;
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        uint64_t max_lsn = 0;
        size_t pos = 0;
        auto parse_lsn = [&](size_t& p, uint64_t& lsn) -> bool {
            if (p + 8 > buf.size()) return false;
            uint32_t len = 0, crc = 0;
            for (int i = 0; i < 4; ++i) len |= static_cast<uint32_t>(buf[p+i]) << (8*i);
            for (int i = 0; i < 4; ++i) crc |= static_cast<uint32_t>(buf[p+4+i]) << (8*i);
            if (len < 20 || len > (1u << 20) || p + 8 + len > buf.size()) return false;
         {
             std::vector<uint8_t> crc_input;
             crc_input.reserve(4 + len);
             for (int i = 0; i < 4; ++i) crc_input.push_back(buf[p+i]);
             crc_input.insert(crc_input.end(), buf.begin() + p + 8, buf.begin() + p + 8 + len);
             if (crc32(crc_input.data(), crc_input.size()) != crc) return false;
         }
         size_t q = p + 8;
         size_t end = p + 8 + len;
         if (q + 8 > end || q + 8 > buf.size()) return false;
            auto pu64 = [&]() { uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t(buf[q++])<<(8*i)); return v; };
            lsn = pu64();
            p += 8 + len;
            return true;
        };
        while (pos < buf.size()) {
            uint64_t lsn;
            if (parse_lsn(pos, lsn)) {
                max_lsn = lsn;
            } else {
                break;  // torn tail or corruption, stop
            }
        }
        return max_lsn;
    }

    bool open_segment(uint64_t id) {
        std::string p = seg_path(id);

        // v24 FIX (Group 1, Fix 3): torn-tail repair before append.
        (void)truncate_torn_tail(p);

        // v25.1 M2 Phase 2: O_WRONLY (not O_APPEND) so pwrite works for the
        // fallback path. lseek(SEEK_END) before writes positions at the end.
        active_fd_ = ::open(p.c_str(), O_CREAT | O_WRONLY, 0644);
        if (active_fd_ < 0) return false;
        active_id_ = id;
        // v25.1 M2: track segment size for size-based rotation.
        // The file may have existing content (reopened after restart).
        active_segment_bytes_ = std::filesystem::file_size(p);
        return true;
    }

    // v25.1 M2 Phase 1: rotate to a new segment if the active segment
    // exceeds SEGMENT_MAX_BYTES. Called by the leader before writing a
    // new batch. The rotation is: fsync+close old segment, create new
    // segment, write MANIFEST. Batches never span segments — the current
    // batch completes on the old segment before rotation.
    void maybe_rotate_segment() {
        if (active_segment_bytes_ < SEGMENT_MAX_BYTES) return;
        // Active segment is full — rotate.
        if (active_fd_ >= 0) {
            checked_fsync(active_fd_);
            checked_close(active_fd_);
            active_fd_ = -1;
        }
        uint64_t new_id = active_id_ + 1;
        if (!open_segment(new_id)) {
            failed_ = true;
            return;
        }
        checked_fsync(active_fd_);
        // Write MANIFEST with the new active_id. ckpt_ts is 0 (no checkpoint
        // triggered this rotation — it's size-based). The checkpoint mechanism
        // (M3) will update ckpt_ts when it runs.
        write_manifest(new_id, 0);
        fsync_dir(dir_);
    }

    // v24 FIX (Group 1, Fix 3): scan a segment file and ftruncate to the
    // last valid frame boundary if a torn tail is found. Returns true if
    // the file was truncated (or was already clean), false on I/O error.
    // Idempotent: a clean file is unchanged.
    bool truncate_torn_tail(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return true;  // file doesn't exist: nothing to repair
        std::vector<uint8_t> buf(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();

        if (buf.empty()) return true;  // empty file: nothing to repair

        // Walk frames using the same parsing logic as wal_recover_buf.
        // We don't reuse wal_recover_buf directly because we need the
        // BYTE OFFSET of the last valid frame boundary, not just the
        // parsed records.
        size_t pos = 0;
        size_t last_valid_end = 0;
        while (pos < buf.size()) {
            // A valid frame is [len:4][crc:4][payload:len]. Minimum len is 20.
            if (pos + 8 > buf.size()) break;  // torn header
            uint32_t len = 0;
            for (int i = 0; i < 4; ++i)
                len |= static_cast<uint32_t>(buf[pos+i]) << (8*i);
            if (len < 20 || len > WAL_MAX_RECORD || pos + 8 + len > buf.size()) break;  // torn or invalid

            // Verify CRC.
            uint32_t crc = 0;
            for (int i = 0; i < 4; ++i)
                crc |= static_cast<uint32_t>(buf[pos+4+i]) << (8*i);
            std::vector<uint8_t> crc_input;
            crc_input.reserve(4 + len);
            for (int i = 0; i < 4; ++i) crc_input.push_back(buf[pos+i]);
            crc_input.insert(crc_input.end(), buf.begin() + pos + 8, buf.begin() + pos + 8 + len);
            if (crc32(crc_input.data(), crc_input.size()) != crc) break;  // corrupt: stop here

            last_valid_end = pos + 8 + len;
            pos = last_valid_end;
        }

        if (last_valid_end < buf.size()) {
            // Torn tail found: truncate the file to last_valid_end.
            // Open for writing (not O_APPEND, we need to ftruncate).
            int fd = ::open(path.c_str(), O_WRONLY, 0644);
            if (fd < 0) return false;
            if (::ftruncate(fd, static_cast<off_t>(last_valid_end)) != 0) {
                ::close(fd);
                return false;
            }
            ::close(fd);
        }
        return true;
    }

public:
    explicit WalSegments(const std::string& dir) : dir_(dir) {
        ::mkdir(dir.c_str(), 0755);
        // v22.1 M4: inter-process guard via advisory flock. Fail loud if this
        // wal_dir is already open in another process (or another live connection
        // in this process). v24: FdGuard releases the fd (and thus the flock)
        // even when a later constructor step throws.
        {
            std::string lock_path = dir + "/.chronokv.lock";
            lock_fd_.fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
            if (lock_fd_.fd < 0) {
                throw std::runtime_error("inter-process lock: cannot open " + lock_path);
            }
            if (::flock(lock_fd_.fd, LOCK_EX | LOCK_NB) != 0) {
                int saved_errno = errno;
                ::close(lock_fd_.fd);
                lock_fd_.fd = -1;  // FdGuard dtor sees -1, skips close
                if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
                    throw std::runtime_error("database at '" + dir + "' is already open in another process");
                }
                throw std::runtime_error("inter-process lock: flock failed on " + lock_path);
            }
        }
        auto [act, ckpt] = read_manifest();
        active_id_ = act;
        // v20.1 BLOCKER #1: never silently recreate a missing active segment
        // for an EXISTING database. If the manifest or any segment is present,
        // the active segment must exist; a missing segment means durable data
        // was lost and recovery must fail loudly (it will: recover_all throws
        // on a missing required segment). Only a brand-new database may create
        // the segment here.
        {
            bool db_has_data = std::filesystem::exists(manifest_path());
            if (!db_has_data) {
                std::error_code sec;
                auto dit = std::filesystem::directory_iterator(dir_, sec);
                for (auto e = std::filesystem::directory_iterator();
                     !sec && dit != e; dit.increment(sec)) {
                    if (!dit->is_regular_file(sec)) continue;
                    std::string fn = dit->path().filename().string();
                    // v24 FIX (Group 2, Fix 8): strict filename match.
                    uint64_t sid = 0;
                    if (parse_seg_filename(fn, &sid)) {
                        db_has_data = true;
                        break;
                    }
                }
            }
            if (!db_has_data) {
                if (!open_segment(active_id_)) failed_ = true;   // new DB: create
            } else if (!std::filesystem::exists(seg_path(active_id_))) {
                failed_ = true;                                  // existing DB: active missing -> fail loud
            } else if (!open_segment(active_id_)) {
                failed_ = true;
            }
        }
        // Item 6 fix: resume lsn_ from the existing segment so we don't reset to 0
        if (!failed_) {
            // v25.1 M2 Phase 2: initialize io_uring for WAL writes.
            iouring_ = std::make_unique<chronokv_iouring::IoUring>();
            // If io_uring is unavailable, the leader path falls back to
            // sync pwrite+fsync (transparent to callers).
            uint64_t max_lsn = [&]() -> uint64_t {
             uint64_t best = 0;
             for (uint64_t scan_id = 1; scan_id <= active_id_; ++scan_id) {
                 uint64_t seg_lsn = find_max_lsn_in_segment(scan_id);
                 if (seg_lsn > best) best = seg_lsn;
             }
             return best;
         }();
            lsn_.store(max_lsn, std::memory_order_relaxed);
        }
    }

    ~WalSegments() {
        // lock_fd_ handled by FdGuard RAII (member destructor)
        if (active_fd_ >= 0) checked_close(active_fd_);
    }

    WalSegments(const WalSegments&) = delete;
    WalSegments& operator=(const WalSegments&) = delete;

    bool is_failed() const { return failed_; }

    // Timestamp is reserved INSIDE batch_mu_ so that cts assignment order
    // matches WAL-batch entry order. This prevents interior gaps in the WAL
    // after a crash: no thread can reserve cts=N+1 and enter the batch before
    // the thread that reserved cts=N has entered.
    // Returns {success, assigned_ts}. If !success and ts > 0, caller must burn ts.
    std::pair<int, uint64_t> group_append(std::atomic<uint64_t>& clock, const WriteSet& ws,
                                   DurabilityMode mode,
                                   const std::function<bool(uint64_t)>& on_reserve) {
    std::unique_lock<std::mutex> lk(batch_mu_);
    if (failed_) return {1, 0};

    uint64_t ts = clock.fetch_add(1, std::memory_order_seq_cst);
    diag::store_max(diag::pub_allocated, ts);
    diag::store_max(i_pub_allocated_max, ts);
    stress_point("after_cts_reserve");

    // v17 race closure:
    //
    // The caller validates range reads and publishes existence transitions
    // inside this timestamp-reservation critical section. Therefore, if a
    // writer receives commit timestamp N and a reader later receives commit
    // timestamp N+1, the reader is guaranteed to observe the writer's
    // existence transition before it validates.
    //
    // If validation fails, we still reserve the timestamp and write an empty
    // no-op record. This preserves WAL timestamp contiguity. The caller burns
    // the timestamp in the publication tracker, so the no-op has no visible
    // MVCC effect.
    bool reserve_conflict = false;
    if (on_reserve && !on_reserve(ts)) reserve_conflict = true;

    WriteSet noop_ws;
    auto rec = reserve_conflict ? wal_ser(ts, noop_ws) : wal_ser(ts, ws);

    // v24 FIX (Group 1, Fix 4): batch separation by durability mode.
    // A batch must contain EITHER async records XOR sync/group records,
    // never both. This ensures:
    //   - On fsync failure, sync/group batches are truncated (no async
    //     records to lose) — caller sees WalFailure, no resurrection.
    //   - On fsync failure, async batches are NOT truncated (async
    //     commits already returned Committed and accepted silent loss;
    //     the documented "stays present after restart" contract holds).
    // cur_batch_ tracks its durability class via the has_async flag.
    // If a new caller's mode differs from cur_batch_'s class, we flush
    // cur_batch_ (set to nullptr so the next leader iteration creates a
    // fresh one) and start a new batch of the caller's class.
    const bool i_am_async = (mode == DurabilityMode::Async);
    if (cur_batch_) {
        const bool batch_is_async = cur_batch_->has_async.load(std::memory_order_relaxed);
        if (batch_is_async != i_am_async) {
            // Mixed-mode: hand off the current batch to a leader by
            // setting it aside. The leader path will pick it up via
            // cur_batch_ on the next iteration. For OUR record, we
            // need a fresh batch of our own class. The simplest way:
            // nullify cur_batch_ so the next line creates a new one.
            // The previous cur_batch_ is still referenced by any
            // earlier callers waiting on it (they hold shared_ptr
            // copies). The leader will pick up the OLD batch when it
            // sees leader_active_==false and cur_batch_==nullptr --
            // wait, no: cur_batch_==nullptr means there's nothing to
            // pick up. The previous batch is still in flight (callers
            // are waiting). We need to NOT lose it.
            //
            // The correct approach: force a leader pass to drain the
            // current batch BEFORE we add our record. Set cur_batch_
            // to nullptr so the leader will pick up the existing
            // shared_ptr (still held by waiting followers), then loop
            // until the batch is done/written/failed, then create a
            // fresh batch for our record.
            //
            // Implementation: just nullify cur_batch_ here. The next
            // line `if (!cur_batch_) cur_batch_ = make_shared<Batch>()`
            // creates a new batch of our class. The OLD batch is still
            // referenced by other callers' my_batch local copies; they
            // will wait on its done/written/failed flags as normal.
            // A leader will emerge from among them (or from us, on a
            // later iteration) and flush the old batch.
            cur_batch_ = nullptr;
        }
    }
    if (!cur_batch_) cur_batch_ = std::make_shared<Batch>();
    auto my_batch = cur_batch_;
    my_batch->records.push_back({ts, rec});
    if (i_am_async)
        my_batch->has_async.store(true, std::memory_order_relaxed);

    const bool need_fsync = !i_am_async;
    while ((need_fsync ? !my_batch->done : !my_batch->written) && !my_batch->failed) {
        if (!leader_active_) {
            leader_active_ = true;
            lk.unlock();
            std::this_thread::yield();
            lk.lock();

            auto batch = cur_batch_;
            cur_batch_ = nullptr;

            // v24 FIX (Group 1, Fix 2): wrap the unlocked leader I/O section
            // (sort, frame construction, write, fsync) in try/catch. If ANY
            // exception escapes (e.g., std::sort or wal_frame throws
            // std::bad_alloc), the leader MUST:
            //   (a) re-acquire batch_mu_,
            //   (b) mark batch->failed and failed_ so all followers see
            //       WalFailure and the WAL enters fail-stop,
            //   (c) reset leader_active_ = false so a future caller doesn't
            //       spin forever waiting for a leader that's gone,
            //   (d) notify_all() to wake every follower parked on batch_cv_.
            // Without this, a single bad_alloc would leave leader_active_=true
            // and cur_batch_=nullptr, deadlocking every subsequent commit.
            //
            // v24 FIX (Group 1, Fix 4): mixed-durability batch truncation.
            // The previous code skipped the ftruncate if the batch contained
            // ANY async record (has_async). That meant Sync/Group records
            // in the same batch survived the failed fsync and would be
            // replayed on recovery -- a durability-contract violation
            // (caller saw WalFailure but data was silently durable). The
            // fix: ALWAYS truncate the batch on fsync failure, AND count
            // any async records as "lost" (since they accepted silent loss
            // at commit time). The has_async flag is no longer used to
            // gate truncation; it is only used to debit the
            // async_committed_then_lost counter for the async subset.
            // Tracking per-record durability mode would be cleaner but
            // requires changing Batch::records; the always-truncate
            // approach is the simpler correct fix the spec requested.
            bool leader_threw = false;
            off_t batch_start = 0;
            bool io_ok = true;
            size_t batch_bytes = 0;
            uint64_t base_lsn = 0;

            try {
                sort(batch->records.begin(), batch->records.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

                // v25.1 M2 Phase 1: rotate segment if full (before writing the batch).
                // This ensures a batch is always fully contained in one segment.
                maybe_rotate_segment();
                if (failed_) { leader_threw = true; throw std::runtime_error("segment rotation failed"); }

                lk.unlock();

                diag::wal_batches.fetch_add(1, std::memory_order_relaxed);
                i_wal_batches.fetch_add(1, std::memory_order_relaxed);

                batch_start = lseek(active_fd_, 0, SEEK_END);

                // v18 M3: batch LSN allocation (one atomic op per batch)
                base_lsn = lsn_.fetch_add(batch->records.size(), std::memory_order_relaxed) + 1;

                // v25.1 M2 Phase 2: concatenate all frames into one buffer,
                // then write via io_uring (or fallback to sync pwrite).
                // The write targets batch_start explicitly (pwrite semantics),
                // NOT append — this is critical for the fallback path.
                std::vector<uint8_t> batch_buf;
                for (size_t ri = 0; ri < batch->records.size(); ++ri) {
                    auto& [cts, b] = batch->records[ri];
                    uint64_t lsn = base_lsn + ri;
                    auto frame = wal_frame(wal_prepend_lsn(lsn, b));
                    batch_buf.insert(batch_buf.end(), frame.begin(), frame.end());
                    batch_bytes += frame.size();
                }

                // v25.1 M2: try io_uring write+fsync, fallback to sync pwrite.
                bool used_iouring = false;
                if (iouring_ && iouring_->available()) {
                    // Submit write at batch_start (positioned, not append).
                    iouring_->submit_write(active_fd_, batch_buf.data(),
                                           batch_buf.size(), batch_start, 1);
                    iouring_->submit_fsync(active_fd_, 2);

                    // CQE-wait hook (for deterministic testing).
                    if (cqe_wait_hook_) cqe_wait_hook_();

                    if (leader_timed_out_.load(std::memory_order_acquire)) {
                        used_iouring = false;
                    } else {
                        int ret = iouring_->enter_with_timeout(2, 2, 500);
                        if (ret > 0) {
                            int res1 = -1, res2 = -1, ud1 = 0, ud2 = 0;
                            iouring_->pop_cqe(&res1, &ud1);
                            iouring_->pop_cqe(&res2, &ud2);
                            if (res1 >= 0 && res2 >= 0) {
                                io_ok = true; used_iouring = true;
                                last_batch_used_iouring_.store(true, std::memory_order_relaxed);
                            } else {
                                used_iouring = false;  // io_uring failed → fallback
                                last_batch_used_iouring_.store(false, std::memory_order_relaxed);
                            }
                        } else {
                            used_iouring = false;  // timeout/error → fallback
                        }
                    }
                }

                if (!used_iouring) {
                    // Fallback: sync pwrite at batch_start + fsync.
                    // v25.1 M2: pwrite (not write) ensures we target the exact
                    // offset, even if a partial io_uring write grew the file.
                    // The post-failure ftruncate at line ~1809 cleans up any
                    // partial data on fsync failure.
                    if (!chronokv_iouring::pwrite_all(active_fd_, batch_buf.data(),
                                                       batch_buf.size(), batch_start)) {
                        io_ok = false;
                        diag::wal_write_fails.fetch_add(1, std::memory_order_relaxed);
                        i_wal_write_fails.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                lk.lock();
                if (io_ok) { batch->written = true; batch_cv_.notify_all(); }
                lk.unlock();

                // v25.1 M2: if io_uring was used (write+fsync as a chain),
                // the fsync is already done. But we still need to check
                // fault injection (FsyncFail) for the durability tests.
                // If fault injection fires, treat as fsync failure (truncation).
                if (io_ok && used_iouring) {
                    // Check fault injection without calling real fsync.
#ifdef CHRONOKV_FAULT_INJECTION
                    if (fault::fire(fault::Kind::FsyncFail)) {
                        io_ok = false;
                        diag::wal_fsync_fails.fetch_add(1, std::memory_order_relaxed);
                        i_wal_fsync_fails.fetch_add(1, std::memory_order_relaxed);
                    }
#endif
                } else if (io_ok && !checked_fsync(active_fd_)) {
                    io_ok = false;
                    diag::wal_fsync_fails.fetch_add(1, std::memory_order_relaxed);
                    i_wal_fsync_fails.fetch_add(1, std::memory_order_relaxed);
                }

                if (io_ok) {
                    diag::wal_records.fetch_add(batch->records.size(), std::memory_order_relaxed);
                    i_wal_records.fetch_add(batch->records.size(), std::memory_order_relaxed);
                    diag::wal_bytes.fetch_add(batch_bytes, std::memory_order_relaxed);
                    i_wal_bytes.fetch_add(batch_bytes, std::memory_order_relaxed);
                    diag::wal_fsyncs.fetch_add(1, std::memory_order_relaxed);
                    i_wal_fsyncs.fetch_add(1, std::memory_order_relaxed);
                    // v25.1 M2: track segment size for size-based rotation.
                    active_segment_bytes_ += batch_bytes;
                }

                // Group 1, Fix 4: mixed-durability batch truncation.
                // The previous code skipped the ftruncate if the batch
                // contained ANY async record (has_async). That meant
                // Sync/Group records in the same batch survived the
                // failed fsync and would be replayed on recovery -- a
                // durability-contract violation (caller saw WalFailure
                // but data was silently durable).
                //
                // The cleaner correct fix the spec requested: do NOT
                // let Async and Sync/Group share a batch. Then Sync/Group
                // batches can be safely truncated on fsync failure (no
                // async records to lose), and Async batches are never
                // truncated (preserving the documented "stays present
                // after restart" contract for async commits that
                // already returned Committed).
                //
                // The batch separation is enforced BELOW in the
                // reservation path (see "Group 1, Fix 4: batch
                // separation" comment). By the time we reach here,
                // a batch contains EITHER async records XOR sync/group
                // records -- never both. So has_async implies "all
                // records in this batch are async" and the original
                // truncation gating is correct again.
                if (!io_ok && batch_start >= 0) {
                    if (batch->has_async.load(std::memory_order_relaxed)) {
                        // All records in this batch are Async. Per the
                        // documented contract, async commits that
                        // returned Committed MUST stay present after
                        // restart (they accepted silent loss at commit
                        // time). Do NOT truncate. Debit the loss
                        // counter for diagnostics.
                        diag::async_committed_then_lost.fetch_add(batch->records.size(),
                                                                  std::memory_order_relaxed);
                        i_async_lost.fetch_add(batch->records.size(), std::memory_order_relaxed);
                    } else {
                        // All records in this batch are Sync/Group.
                        // Truncate to remove the failed batch from the
                        // WAL so it doesn't resurrect on recovery.
                        diag::wal_truncations.fetch_add(1, std::memory_order_relaxed);
                        i_wal_truncations.fetch_add(1, std::memory_order_relaxed);
                        if (ftruncate(active_fd_, batch_start) != 0) {
                            diag::wal_truncate_fails.fetch_add(1, std::memory_order_relaxed);
                            i_wal_truncate_fails.fetch_add(1, std::memory_order_relaxed);
                            std::cerr << "WARNING: ftruncate failed after WAL durability "
                                         "failure (errno=" << errno << ") — non-durable "
                                         "record may persist in the WAL file\n";
                        }
                    }
                }
            } catch (...) {
                // v25.1 M2: log the exception for debugging.
                try { throw; } catch (const std::exception& e) {
                    std::cerr << "WAL leader exception: " << e.what() << "\n";
                } catch (...) {
                    std::cerr << "WAL leader exception: unknown\n";
                }
                leader_threw = true;
                // Ensure lk is in a known state (locked) before we proceed
                // to the failure-cleanup section below. If the throw
                // happened while lk was unlocked, we need to re-acquire.
                // unique_lock::lock() is a no-op if already locked.
                // We do NOT use try/catch here -- if re-locking throws we
                // are in unrecoverable territory and std::terminate is
                // appropriate.
            }

            // Re-acquire batch_mu_ for the final state mutation.
            // (unique_lock::lock() is idempotent if already locked.)
            lk.lock();
            if (leader_threw || !io_ok) {
                failed_ = true;
                batch->failed = true;
            } else {
                batch->done = true;
            }
            leader_active_ = false;
            batch_cv_.notify_all();
            // If the leader threw, propagate the exception AFTER cleanup.
            // Callers (commit_txn) catch it via Fix 1b and return WalFailure.
            if (leader_threw) throw;
        } else {
            batch_cv_.wait(lk);
        }
    }

    bool ok = need_fsync ? (my_batch->done && !my_batch->failed)
                         : my_batch->written;

    if (!ok) return {1, ts};
    return {reserve_conflict ? 2 : 0, ts};
}

bool rotate_after_checkpoint(uint64_t ckpt_ts) {
        std::unique_lock<std::mutex> lk(batch_mu_);
        if (failed_) return false;

     // v17 Work Stream C: wait until no group-commit leader is performing
     // file I/O on active_fd_. The leader releases batch_mu_ during write
     // and fsync; rotating in that window would close the fd out from
     // under it. Commits hold only a SHARED checkpoint_mu_, so checkpoint
     // can acquire the exclusive lock while a leader is still in flight.
     batch_cv_.wait(lk, [this] { return !leader_active_ || failed_; });
     if (failed_) return false;

        if (active_fd_ >= 0) {
            if (!checked_fsync(active_fd_)) { failed_ = true; return false; }
            if (!checked_close(active_fd_)) { failed_ = true; return false; }
            active_fd_ = -1;
        }

        uint64_t old_id = active_id_;
        uint64_t new_id = active_id_ + 1;
        if (!open_segment(new_id)) { failed_ = true; return false; }
        if (!checked_fsync(active_fd_)) { failed_ = true; return false; }

        if (!write_manifest(new_id, ckpt_ts)) { failed_ = true; return false; }

        // Delete old segments fully covered by the checkpoint
        for (uint64_t id = 1; id < old_id; ++id)
            ::unlink(seg_path(id).c_str());

        fsync_dir(dir_);
        return true;
    }

   // Recovery: read all segments in order.
    // v17 Work Stream B: read-only recovery with strict validation.
    //   - Does NOT create directories or files.
    //   - Rejects corrupt manifests.
    //   - Detects orphan segments (id > manifest active_id).
    //   - Detects missing sealed segments.
    //   - Tolerates torn tails only in the final (active) segment.
    static std::pair<WalStatus, std::vector<std::pair<uint64_t, WriteSet>>>
    recover_all(const std::string& dir, uint64_t& ckpt_ts) {
        std::vector<std::pair<uint64_t, WriteSet>> all;

        // 1. Read manifest READ-ONLY (no file creation).
        std::string mp = dir + "/MANIFEST";
        uint64_t active_id = 0;
        ckpt_ts = 0;
        bool manifest_exists = false;

        {
            std::ifstream mf(mp, std::ios::binary);
            if (mf) {
                std::vector<uint8_t> buf(
                    (std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
                mf.close();
                manifest_exists = true;

                if (buf.size() < 24)
                    throw std::runtime_error("Recovery failed: MANIFEST truncated");

                uint32_t magic = 0, crc = 0;
                for (int i = 0; i < 4; ++i) magic |= static_cast<uint32_t>(buf[i]) << (8*i);
                for (int i = 0; i < 4; ++i) crc   |= static_cast<uint32_t>(buf[4+i]) << (8*i);

                if (magic != 0x4D414E46)
                    throw std::runtime_error("Recovery failed: MANIFEST bad magic");
                if (crc32(&buf[8], buf.size() - 8) != crc)
                    throw std::runtime_error("Recovery failed: MANIFEST CRC mismatch");

                size_t q = 8;
                auto pu64 = [&]() { uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t(buf[q++])<<(8*i)); return v; };
                active_id = pu64();
                ckpt_ts = pu64();
            }
        }

        if (!manifest_exists) {
            // No MANIFEST. This is the normal state of a non-checkpointed
            // database: segments exist but MANIFEST is only written by
            // rotate_after_checkpoint(). Infer active_id from the highest
            // segment number found.
            uint64_t max_seg_id = 0;
            if (std::filesystem::exists(dir) && std::filesystem::is_directory(dir)) {
                for (auto& entry : std::filesystem::directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string fname = entry.path().filename().string();
                    // v24 FIX (Group 2, Fix 8): strict filename match.
                    uint64_t seg_id = 0;
                    if (parse_seg_filename(fname, &seg_id)) {
                        if (seg_id > max_seg_id) max_seg_id = seg_id;
                    }
                }
            }
            if (max_seg_id == 0) {
                // Fresh database: no MANIFEST, no segments.
                return {WalStatus::OK, all};
            }
            // Non-checkpointed database: infer active_id from highest segment.
            active_id = max_seg_id;
            ckpt_ts = 0;
            // Fall through to segment reading below.
        }

        // 2. Scan directory for orphan segments (id > active_id).
        for (auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            // v24 FIX (Group 2, Fix 8): strict filename match.
            uint64_t seg_id = 0;
            if (parse_seg_filename(fname, &seg_id)) {
                if (seg_id > active_id) {
                    throw std::runtime_error("Recovery failed: orphan WAL segment " + fname +
                        " (manifest active_id=" + std::to_string(active_id) + ")");
                }
            }
        }

        // 3. Read segments 1..active_id with strict validation.
        WalStatus worst = WalStatus::OK;
        for (uint64_t id = 1; id <= active_id; ++id) {
            char seg_path[128];
            snprintf(seg_path, sizeof(seg_path), "%s/wal_%06llu.log",
                     dir.c_str(), (unsigned long long)id);

            // Check if file exists.
            {
                std::ifstream check(seg_path, std::ios::binary);
                if (!check) {
                    // rotate_after_checkpoint() deletes segments with id < old_id.
                    // Since the manifest stores active_id = old_id + 1, segments
                    // with id < active_id - 1 may have been legitimately deleted.
                    // Segments at or above active_id - 1 must exist.
                    if (ckpt_ts > 0 && active_id >= 2 && id < active_id - 1) {
                        continue;
                    }
                    throw std::runtime_error("Recovery failed: missing WAL segment " + std::to_string(id) +
                        " (manifest active_id=" + std::to_string(active_id) + ")");
                }
            }

            auto [status, records] = wal_recover_file(seg_path);

            if (status == WalStatus::CORRUPT) {
                diag::rec_crc_fails.fetch_add(1, std::memory_order_relaxed);
                throw std::runtime_error("Recovery failed: WAL corruption in segment " + std::to_string(id));
            }

            if (status == WalStatus::TORN_TAIL) {
                // Only tolerate torn tail in the FINAL (active) segment.
                // A torn tail in a sealed segment indicates corruption.
                if (id < active_id) {
                    throw std::runtime_error("Recovery failed: torn tail in sealed segment " + std::to_string(id));
                }
                diag::rec_torn.fetch_add(1, std::memory_order_relaxed);
                worst = WalStatus::TORN_TAIL;
            }

            all.insert(all.end(), records.begin(), records.end());
        }

        return {worst, all};
    }

    // v24 FIX (Group 2, Fix 5): seed clock_ from existing WAL contents.
    // When a Database is opened with recover_on_open=false, the engine
    // skips full recovery but still needs clock_ to be >= max cts in
    // the existing WAL. Otherwise the first write would reserve cts=1,
    // colliding with existing records and producing a duplicate-ts gap
    // on a future recovery. Mirrors how lsn_ is already seeded in the
    // WalSegments constructor (lines ~1281-1290).
    //
    // Approach chosen: seed at construction time, unconditionally, by
    // scanning existing WAL segments. This is the least invasive change
    // -- it mirrors the lsn_ seeding pattern, doesn't change the public
    // API, and is automatic for callers who pass recover_on_open=false.
    //
    // Alternative considered: refuse new writes when recover_on_open=
    // false and the WAL has content until recovery is explicitly run.
    // Rejected because it breaks legitimate "open for append-only"
    // usage and requires a new public API surface for "force recovery."
    //
    // This method is best-effort: it ignores torn tails (which are
    // tolerable at the active segment's end) and corruption (which
    // recovery would catch later). It returns the max cts seen across
    // all parseable records, or 0 if the WAL is empty/new.
    static uint64_t seed_clock_from_wal(const std::string& dir) {
        uint64_t max_cts = 0;
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            return max_cts;
        }
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            std::string fname = entry.path().filename().string();
            // v24 FIX (Group 2, Fix 8): use strict canonical filename match.
            uint64_t seg_id = 0;
            if (!parse_seg_filename(fname, &seg_id)) continue;
            // Parse records from this segment, find max cts.
            // Use wal_recover_file which already handles torn tails
            // and CRC validation. We ignore the status; partial
            // records (torn tail) are tolerated for seeding purposes.
            std::string seg_path = entry.path().string();
            auto [status, records] = wal_recover_file(seg_path);
            (void)status;  // tolerate TORN_TAIL and CORRUPT for seeding
            for (auto& [cts, ws] : records) {
                if (cts > max_cts) max_cts = cts;
            }
        }
        return max_cts;
    }
};

// ======================== Publication tracker ========================


// ================================================================
// [SECTION_4_PUBLICATION_TRACKER]
// ================================================================
class PublicationTracker {
    std::mutex mu_;
    std::set<uint64_t> completed_;
    std::set<uint64_t> burned_;
    std::atomic<uint64_t> published_{0};

    void advance() {
        uint64_t pub = published_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t nxt = pub + 1;
            bool done = completed_.erase(nxt) > 0;
            bool burnt = burned_.erase(nxt) > 0;
            if (!done && !burnt) break;
            ++pub;
        }
        published_.store(pub, std::memory_order_seq_cst);
        diag::store_max(diag::pub_published, pub);
    }

public:
    uint64_t published() const { return published_.load(std::memory_order_seq_cst); }

    void complete(uint64_t ts) {
        std::lock_guard<std::mutex> lk(mu_);
        completed_.insert(ts);
        advance();
    }

    void burn(uint64_t ts) {
        std::lock_guard<std::mutex> lk(mu_);
        burned_.insert(ts);
        advance();
    }

    void recover_to(uint64_t prefix) {
        std::lock_guard<std::mutex> lk(mu_);
        published_.store(prefix, std::memory_order_seq_cst);
        diag::store_max(diag::pub_published, prefix);
    }
};

// ======================== Index epoch tracker ========================


// ================================================================
// [SECTION_5_PHANTOM_TRACKER]
// ================================================================
class PhantomTracker {
    mutable std::mutex mu_;

    // commit_ts -> keys whose logical existence changed at that commit.
    // Only absent->present and present->absent transitions are stored.
    std::map<uint64_t, std::set<std::string>> mods_by_ts_;

    // Active ReadWriteTransaction snapshot timestamps. A modification with
    // commit_ts <= min(active_snapshots) is no longer needed for phantom
    // detection, because all active readers already include that commit.
    std::multiset<uint64_t> active_snapshots_;

    // v20 M1: per-instance epoch diagnostics (dual-written with diag:: globals).
    std::atomic<uint64_t> i_epoch_entries{0}, i_epoch_oldest{0}, i_epoch_newest{0};

public:
    // v20 M1: read-only snapshot of per-instance epoch diagnostics.
    struct EpochStats { uint64_t entries, oldest, newest; };
    EpochStats epoch_stats() const {
        return EpochStats{
            i_epoch_entries.load(std::memory_order_relaxed),
            i_epoch_oldest.load(std::memory_order_relaxed),
            i_epoch_newest.load(std::memory_order_relaxed)};
    }

    void register_reader(uint64_t read_ts) {
        std::lock_guard<std::mutex> lk(mu_);
        active_snapshots_.insert(read_ts);
    }

    void deregister_reader(uint64_t read_ts) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = active_snapshots_.find(read_ts);
        if (it != active_snapshots_.end()) active_snapshots_.erase(it);
    }

    void record_transition(uint64_t commit_ts, const std::string& key,
                           bool old_exists, bool new_exists) {
        if (old_exists == new_exists) return;

        std::lock_guard<std::mutex> lk(mu_);
        mods_by_ts_[commit_ts].insert(key);

        diag::epoch_entries.fetch_add(1, std::memory_order_relaxed);
        diag::store_max(diag::epoch_newest, commit_ts);
        i_epoch_entries.fetch_add(1, std::memory_order_relaxed);
        diag::store_max(i_epoch_newest, commit_ts);
        if (!mods_by_ts_.empty()) {
            diag::epoch_oldest.store(mods_by_ts_.begin()->first,
                                     std::memory_order_relaxed);
            i_epoch_oldest.store(mods_by_ts_.begin()->first,
                                 std::memory_order_relaxed);
        }
    }

    bool has_phantom_in_range(const std::string& lo, const std::string& hi,
                              uint64_t read_ts) const {
        std::lock_guard<std::mutex> lk(mu_);

        // A phantom exists if any existence transition committed after the
        // reader's snapshot timestamp falls inside [lo, hi] (INCLUSIVE, matching
        // range_scan's inclusive upper bound — v20.1 blocker #3).
        auto it = mods_by_ts_.upper_bound(read_ts);
        while (it != mods_by_ts_.end()) {
            auto kit = it->second.lower_bound(lo);
            if (kit != it->second.end() && *kit <= hi) return true;
            ++it;
        }
        return false;
    }

    // v24 FIX (Group 2, Fix 12 — phantom-reader race, Shape C):
    // prune() now accepts an `external_floor` parameter. The caller
    // (ChronoKV::prune_phantom_tracker) computes this as
    // min(reader_slots_[i].snapshot - 1) under reader_mu_, and
    // passes it here while STILL holding reader_mu_. This means a
    // reader that has acquired a slot but not yet registered in
    // active_snapshots_ (the race window the fix closes) still
    // constrains the safe threshold — its slot.snapshot is visible
    // under reader_mu_.
    //
    // Lock order: caller holds reader_mu_ (outer) → prune() takes
    // mu_ (inner). acquire_slot_with_phantom() uses the same order.
    // No other code path nests these two mutexes, so the order is
    // acyclic.
    //
    // Default arg preserves the pre-fix behavior for any future
    // caller that doesn't have an external floor (treats it as
    // "no external constraint," which is the old behavior).
    void prune(uint64_t external_floor = UINT64_MAX) {
        std::lock_guard<std::mutex> lk(mu_);

        // safe = min(external_floor, *active_snapshots_.begin() or UINT64_MAX)
        // Either source can pin the threshold; both must agree to delete.
        uint64_t safe = external_floor;
        if (!active_snapshots_.empty()) {
            safe = std::min(safe, *active_snapshots_.begin());
        }

        auto it = mods_by_ts_.begin();
        while (it != mods_by_ts_.end() && it->first <= safe) {
            diag::epoch_entries.fetch_sub(it->second.size(),
                                          std::memory_order_relaxed);
            i_epoch_entries.fetch_sub(it->second.size(),
                                      std::memory_order_relaxed);
            it = mods_by_ts_.erase(it);
        }

        diag::epoch_oldest.store(
            mods_by_ts_.empty() ? 0 : mods_by_ts_.begin()->first,
            std::memory_order_relaxed);
        i_epoch_oldest.store(
            mods_by_ts_.empty() ? 0 : mods_by_ts_.begin()->first,
            std::memory_order_relaxed);
    }

#ifdef CHRONOKV_TEST_HOOKS
public:
    // Test hook: count transitions with commit_ts strictly greater
    // than `ts`. Used by the Fix 12 regression test to verify that
    // prune() did not erase transitions a live reader needs.
    size_t test_mods_count_above(uint64_t ts) const {
        std::lock_guard<std::mutex> lk(mu_);
        size_t n = 0;
        for (auto it = mods_by_ts_.upper_bound(ts);
             it != mods_by_ts_.end(); ++it) {
            n += it->second.size();
        }
        return n;
    }
private:
#endif
};

// ======================== Range read record ========================

struct RangeRead {
    std::string lo, hi;
    uint64_t snapshot_ts;
    uint64_t index_epoch;
};

// ======================== Dynamic key entry ========================


// ================================================================
// [SECTION_6_MVCC_TYPES]
// ================================================================
struct Version {
    std::atomic<uint64_t> commit_ts;
    std::string value;
    bool deleted;
    std::atomic<Version*> prev;
    Version(uint64_t t, std::string v, bool d, Version* p)
        : commit_ts(t), value(std::move(v)), deleted(d), prev(p) {}
};

struct KeyEntry {
    std::mutex commit_mu;
    std::atomic<Version*> head{nullptr};
    std::atomic<uint64_t> last_write_ts{0};
};

// ======================== Reusable reader slot ========================

// ======================== Retired node (v23 Phase C) ========================
// A Version belongs either to a reachable head chain or to retired_nodes_,
// never both. gc_once() first severs an anchor's prev link with release
// ordering, then transfers ownership of the detached suffix here. The queue
// is protected by retired_mu_, so its epoch needs no separate atomic.
struct RetiredNode {
    std::unique_ptr<Version> node;
    uint64_t retired_epoch;
    RetiredNode(Version* n, uint64_t e) : node(n), retired_epoch(e) {}
};

// snapshot controls MVCC visibility. pinned_epoch controls physical lifetime.
// Both transitions between inactive and pinned are serialized by reader_mu_.
struct ReaderSlot {
    std::atomic<uint64_t> snapshot{0};
    uint64_t pinned_epoch = 0;  // reader_mu_ protected; 0 means unpinned
};

// ======================== MVCC store ========================

// ======================== Memory ordering audit (Phase 0 item 0.8) ========================
// Systematic justification for every atomic's memory order. Corroborated by TSan
// (clean, including -DCHRONOKV_STRESS). Do not change an order without re-running
// TSan and updating this audit.
//
// SEQ_CST (total order required):
//   clock_                   Timestamp allocation; totally ordered with publication and
//                            WAL batch entry (Finding 1). fetch_add(seq_cst).
//   PublicationTracker::     Global publication point; reader snapshots and GC thresholds
//     published_             derive from it. store/load(seq_cst).
//   ReaderSlot::snapshot     acquire_slot double-check (store snapshot, re-read published_)
//                            must pair with published_ in a total order. store/load(seq_cst).
//
// RELEASE / ACQUIRE (publication edges):
//   KeyEntry::head           Writer fully constructs a Version, store(release) publishes the
//                            pointer; reader load(acquire) sees the complete chain.
//   Version::prev            GC severs with store(release); traversers load(acquire).
//   Version::commit_ts       Immutable after construction; visible via head's release edge.
//   dirty_/running_          GC-loop gating (also guarded by gc_mu_/gc_cv_).
//   IndexEpochTracker::      Lock-free "changed?" fast path. The mods_by_epoch_ map is
//     epoch_                 synchronized by mu_, NOT by this atomic.
//
// RELAXED (ordering supplied by a co-held mutex):
//   KeyEntry::last_write_ts  Accessed only while holding the same KeyEntry::commit_mu on
//                            both the validating read and the linking write.
//   stress::*                Test-only seeding.
//
// GC SAFETY / I6 (Phase C epoch-pinned reclamation):
//   SnapshotGuard acquisition and release are linearized by reader_mu_. While a reader
//   is live, its ReaderSlot publishes both its snapshot and the reclaim epoch it pinned.
//   GC severs a version suffix with release stores, records each severed node with the
//   current retired_epoch, advances reclaim_epoch_, then reclaims only nodes satisfying
//   retired_epoch < min_active_pin_epoch(). Thus a reader that could have reached a
//   retired node keeps reclamation blocked; a reader acquired after the GC pass cannot
//   reach a suffix already severed from the published head. Version pointer publication
//   and traversal use release/acquire ordering. Physical destruction occurs only after
//   retired_mu_ is released. This epoch-pin protocol is the reclamation safety mechanism;
//   the old threshold-only argument is no longer sufficient on its own.

// Phase 3 item 14: writer-preferring readers-writer lock. std::shared_mutex is
// reader-preferring, which let a steady stream of commits (shared holders) starve
// the checkpoint (exclusive requester) -- measured as 98.5% of checkpoint time
// spent waiting for the lock. Here, once a writer is WAITING, new readers queue
// behind it, so the checkpoint proceeds as soon as in-flight commits drain.
class FairSharedMutex {
    std::mutex mu_;
    std::condition_variable cv_;
    int readers_ = 0;
    bool writer_active_ = false;
    int writers_waiting_ = 0;
public:
    void lock_shared() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return !writer_active_ && writers_waiting_ == 0; });
        ++readers_;
    }
    void unlock_shared() {
        std::unique_lock<std::mutex> lk(mu_);
        if (--readers_ == 0) cv_.notify_all();
    }
    void lock() {
        std::unique_lock<std::mutex> lk(mu_);
        ++writers_waiting_;
        cv_.wait(lk, [&] { return !writer_active_ && readers_ == 0; });
        --writers_waiting_;
        writer_active_ = true;
    }
    void unlock() {
        std::unique_lock<std::mutex> lk(mu_);
        writer_active_ = false;
        cv_.notify_all();
    }
};


// ================================================================
// [SECTION_7_CHRONOKV_CORE]
// ================================================================
enum class ReplayResult : uint8_t { OK = 0, GapDetected = 1 };

// ======================== RAII SnapshotGuard ========================
// Declared before ChronoKV so ChronoKV's read paths (read, range_scan) can
// hold a slot via RAII. The constructor/destructor require ChronoKV to be
// complete, so they are defined out-of-line after the ChronoKV class
// (v20.1 #11: exception-safe slot management).
class OrderingViolation : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// v25.1 M1.6: forward declarations so ChronoKV can hold B+ tree members.
// PagePool (chronokv_page::) and BTree (chronokv_btree::) are defined later
// in this file; ChronoKV stores them via unique_ptr (non-copyable types).
namespace chronokv_page { class PagePool; }
namespace chronokv_btree { class BTree; }

// v25.1 M1.6 (Phase 2): forward-declared pImpl for range_scan_stream.
// Defined after BTree (holds a BTree::Cursor + SnapshotGuard).
struct ChronoKVRangeScanCursorState;

// v25.1 M2 Phase 2: forward-declare chronokv::Database for friend access.
namespace chronokv { class Database; }

class ChronoKV;
class SnapshotGuard {
    ChronoKV& kv_;
    int slot_;
    uint64_t read_ts_;
public:
    explicit SnapshotGuard(ChronoKV& kv);
    ~SnapshotGuard();
    SnapshotGuard(const SnapshotGuard&) = delete;
    SnapshotGuard& operator=(const SnapshotGuard&) = delete;
    uint64_t read_ts() const { return read_ts_; }
    int slot() const { return slot_; }
};

class ChronoKV {
    friend class SnapshotGuard;
    friend class ReadWriteTransaction;
    friend struct ChronoKVRangeScanCursorState;  // v25.1 M1.6 Phase 2: range_scan_stream pImpl
    friend class ::chronokv::Database;  // v25.1 M2: Database accesses wal_ for test injection

    // v25.1 M1.6: the B+ tree IS the key→KeyEntry index. Replaces
    // name2idx_ + ordered_idx_ + entries_ (the three-structure index from
    // v15). The tree stores KeyEntry* (8 bytes, encoded) as the value;
    // the engine owns KeyEntry lifetime (heap-allocated, deferred
    // reclamation via retire() on erase, same pattern as Version nodes).
    // nm_ now serializes ONLY the ensure_index check-then-insert slow
    // path (new-key creation); reads go through the tree's own per-page
    // latching (M1.4) and need no nm_.
    mutable std::shared_mutex nm_;  // M1.6: ensure_index slow-path only
    std::unique_ptr<chronokv_page::PagePool> pool_;
    std::unique_ptr<chronokv_btree::BTree> tree_;

    std::atomic<uint64_t> clock_{1};
    PublicationTracker pub_;

    // v15: reusable reader slots (no MAX_READERS lifetime limit)
    mutable std::mutex reader_mu_;
    std::atomic<uint64_t> reclaim_epoch_{1};
    std::vector<std::unique_ptr<ReaderSlot>> reader_slots_;
    std::vector<size_t> reader_free_;

    std::mutex gc_mu_;
    std::condition_variable gc_cv_;
    std::atomic<bool> dirty_{false}, running_{false};
    std::thread gc_;
    // Serialises gc_once() with checkpoint(). Exists solely to close the race
    // where GC computes its threshold BEFORE checkpoint registers a reader slot.
    // Without it, GC could reclaim versions that checkpoint is about to serialise.
    // Safe to refine into a finer-grained mechanism in the future, but do NOT
    // remove without replacing the guarantee.
    std::mutex gc_active_mu_;

    // v20 M1: per-instance diagnostics for counters owned by ChronoKV methods
    // (gc_*, rec_records, and the no-WAL pub_allocated path). Dual-written with
    // the diag:: globals so diag::dump() and existing tests keep working.
    std::atomic<uint64_t> i_gc_created{0}, i_gc_retired{0}, i_gc_reclaimed{0};
    // v25.1 M1.6: debug counter for fence re-check retries (delegates to BTree's counter).
    // Kept on ChronoKV for the public accessor; BTree's counter is the source.
    std::atomic<uint64_t> i_fence_retries{0};
    std::atomic<uint64_t> i_gc_passes{0}, i_gc_last_keys{0}, i_gc_last_steps{0};
    std::atomic<uint64_t> i_gc_last_pass_ns{0}, i_gc_max_pass_ns{0};
    std::atomic<uint64_t> i_rec_records{0};
    std::atomic<uint64_t> i_pub_allocated_no_wal{0};
    // v23 Phase C: epoch-pinned reclamation.
    // A reader publishes/clears pinned_epoch while reader_mu_ is held. The
    // reclaimer reads every pin under that same mutex, which linearizes the
    // question "could this reader have reached the retired suffix?" A reader
    // that pins before that scan blocks R < min_epoch; a reader that pins after
    // it cannot dereference a suffix already severed from the head chain.
    std::mutex retired_mu_;
    std::vector<std::unique_ptr<RetiredNode>> retired_nodes_;
    std::atomic<uint64_t> retired_pending_{0};

    void retire(Version* node) {
        const uint64_t epoch = reclaim_epoch_.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(retired_mu_);
        retired_nodes_.push_back(std::make_unique<RetiredNode>(node, epoch));
        retired_pending_.store(retired_nodes_.size(), std::memory_order_release);
        diag::gc_retired.fetch_add(1, std::memory_order_relaxed);
        i_gc_retired.fetch_add(1, std::memory_order_relaxed);
    }

    void advance_reclaim_epoch() {
        uint64_t current = reclaim_epoch_.load(std::memory_order_relaxed);
        for (;;) {
            if (current == UINT64_MAX)
                return;  // Epoch exhausted; halt reclamation safely
            if (reclaim_epoch_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
        }
    }

    uint64_t min_active_pin_epoch() const {
        std::lock_guard<std::mutex> lock(reader_mu_);
        uint64_t min_epoch = UINT64_MAX;
        for (const auto& slot : reader_slots_) {
            if (slot->pinned_epoch != 0)
                min_epoch = std::min(min_epoch, slot->pinned_epoch);
        }
        return min_epoch;
    }

    size_t reclaim_retired() {
        const uint64_t min_epoch = min_active_pin_epoch();
        std::vector<std::unique_ptr<Version>> victims;
        {
            std::lock_guard<std::mutex> lock(retired_mu_);
            auto keep = retired_nodes_.begin();
            for (auto it = retired_nodes_.begin(); it != retired_nodes_.end(); ++it) {
                if ((*it)->retired_epoch < min_epoch) {
                    victims.push_back(std::move((*it)->node));
                } else {
                    if (keep != it) *keep = std::move(*it);
                    ++keep;
                }
            }
            retired_nodes_.erase(keep, retired_nodes_.end());
            retired_pending_.store(retired_nodes_.size(), std::memory_order_release);
        }

        // Version destructors can free strings; run them after releasing every
        // engine mutex. The counter changes only once destruction is complete.
        const size_t reclaimed = victims.size();
        victims.clear();
        if (reclaimed != 0) {
            diag::gc_reclaimed.fetch_add(reclaimed, std::memory_order_relaxed);
            i_gc_reclaimed.fetch_add(reclaimed, std::memory_order_relaxed);
        }
        return reclaimed;
    }
 size_t gc_cursor_ = 0;
 int delta_count_ = 0;  // v18.2: deltas since last full base
 static constexpr int REBASE_AFTER_DELTAS = 100;  // v18: incremental GC cursor
 // v20 M3: byte-size-aware rebase trigger. Thresholds are members (seeded from
 // the constants) so tests can lower them to exercise rebase quickly.
 static constexpr uint64_t REBASE_AFTER_DELTA_BYTES = 64ULL * 1024 * 1024;
 int rebase_count_threshold_ = REBASE_AFTER_DELTAS;
 uint64_t rebase_bytes_threshold_ = REBASE_AFTER_DELTA_BYTES;
 uint64_t delta_bytes_since_base_ = 0;
 static constexpr size_t GC_KEYS_PER_PASS = 256;
 static constexpr size_t GC_STEPS_PER_PASS = 4096;  // v18.2 NOTE: bounds new-key starts per pass; does NOT preempt
 // mid-key chain walks (full-key atomicity is preserved by design).  // v18.1: version-step budget

    std::unique_ptr<WalSegments> wal_;
 std::string wal_dir_;  // v18 M5: stored for replication
    DurabilityMode durability_ = DurabilityMode::Group;  // item 8
    double last_ckpt_lock_wait_ms_ = 0;   // path-2: time blocked acquiring checkpoint_mu_
    double last_ckpt_work_ms_ = 0;        // path-2: time doing work under the lock
    mutable FairSharedMutex checkpoint_mu_;   // item 14: writer-preferring
    PhantomTracker phantom_tracker_;
    // v18: incremental checkpoint dirty-key tracking.
    std::unordered_map<std::string, uint64_t> dirty_since_ckpt_;  // v18.2: key -> max dirty cts
    std::mutex dirty_mu_;
    mutable std::mutex order_mu_;

    // v25.1 M1.6: KeyEntry* ↔ tree-value encode/decode. The tree stores
    // arbitrary byte strings as values; we encode the 8-byte pointer as
    // 8 bytes (native endian — never serialized to disk, only in-memory
    // in the page pool, so endianness is irrelevant).
    static std::string encode_ptr(KeyEntry* p) {
        std::string buf(sizeof(KeyEntry*), '\0');
        std::memcpy(buf.data(), &p, sizeof(KeyEntry*));
        return buf;
    }
    static KeyEntry* decode_ptr(const std::string& buf) {
        if (buf.size() != sizeof(KeyEntry*)) return nullptr;
        KeyEntry* p = nullptr;
        std::memcpy(&p, buf.data(), sizeof(KeyEntry*));
        return p;
    }

    // v25.1 M1.6: ensure_index now returns {KeyEntry*, is_new} directly.
    // The B+ tree IS the index — no integer indirection. Fast path reads
    // the tree (internally latched via M1.4's per-page shared_mutex +
    // hazard pointers); slow path (new-key creation) is serialized by nm_
    // to make the check-then-insert atomic (same pattern as v15, but nm_
    // is now ONLY for this slow path, not for all reads).
    // Defined out-of-line (after BTree is complete) because BTree is
    // forward-declared at this point.
    std::pair<KeyEntry*, bool> ensure_index(const std::string& k);

    // v25.1 M1.6: find_index returns KeyEntry* (nullptr if absent).
    // No nm_ needed — the tree's own latching protects the read.
    KeyEntry* find_index(const std::string& k) const;

    // v25.1 M1.6: thin wrappers around tree_->range_scan / tree_->size.
    // Defined out-of-line (after BTree is complete) because BTree is
    // forward-declared. Used by gc_once, free_all, verify_*,
    // checkpoint, range_scan, recover_with_checkpoint.
    std::vector<std::pair<std::string, std::string>>
    tree_scan(const std::string& lo, const std::string& hi) const;
    size_t tree_size() const;

    void wake_gc() {
        // v24 FIX (Group 2, Fix 7): lost-wakeup race. The previous code
        // set dirty_ and called notify_one() WITHOUT holding gc_mu_.
        // The GC thread waits with a predicate under gc_mu_:
        //   gc_cv_.wait(lk, [this]{ return !running_ || dirty_ || ...; });
        // cv::wait(lk, pred) checks pred() while holding lk, then
        // atomically releases lk and blocks. If wake_gc sets dirty_
        // and notifies BEFORE the GC thread has entered wait_impl
        // (i.e., while it's still holding lk evaluating the predicate),
        // the notification is lost -- the GC thread blocks forever.
        // Fix: take gc_mu_ around the dirty_.store so the predicate
        // check and the store are atomic with respect to each other.
        // notify_one is called AFTER releasing the lock (standard
        // pattern) so the GC thread doesn't wake only to block on lk.
        {
            std::lock_guard<std::mutex> lk(gc_mu_);
            dirty_.store(true, std::memory_order_release);
        }
        gc_cv_.notify_one();
    }

    uint64_t gc_threshold() const {
        uint64_t p = pub_.published();
        uint64_t rm = UINT64_MAX;
        {
            std::lock_guard<std::mutex> lk(reader_mu_);
            for (auto& s : reader_slots_) {
                uint64_t v = s->snapshot.load(std::memory_order_seq_cst);
                if (v != 0) rm = std::min(rm, v - 1);
            }
        }
        return std::min(rm, p);
    }

    bool gc_once() {
     auto gc_t0 = std::chrono::steady_clock::now();
     uint64_t m = gc_threshold();

     size_t keys_done = 0;
     size_t steps_done = 0;

     // v25.1 M1.6 (Phase 3): collect all KeyEntry* via tree range_scan.
     // The tree's range_scan uses Cursor (M1.3/M1.4: shared latches + HP),
     // which is safe under concurrent writers. Phase 3's TSan gate verifies
     // no use-after-free on KeyEntry* fetched mid-cursor-walk.
     // Returns vector<(key, encoded_ptr)>. Decode ptrs to get the entries.
     // NOTE: new keys added during this scan are not visited this pass —
     // same as the old entries_ walk (which checked idx >= entries_.size()).
     std::vector<KeyEntry*> all_entries;
     {
         auto pairs = tree_scan("", std::string(255, '\xFF'));
         all_entries.reserve(pairs.size());
         for (auto& [k, v] : pairs)
             all_entries.push_back(decode_ptr(v));
     }
     size_t total = all_entries.size();

     if (total == 0) {
         // v24 Fix 12: route through prune_phantom_tracker() so the
         // external floor from reader_slots_ is consulted under
         // reader_mu_ — closing the phantom-reader race window.
         prune_phantom_tracker();
         auto gc_t1 = std::chrono::steady_clock::now();
         uint64_t ns = static_cast<uint64_t>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(gc_t1 - gc_t0).count());
         diag::gc_passes.fetch_add(1, std::memory_order_relaxed);
         diag::gc_last_keys.store(0, std::memory_order_relaxed);
         diag::gc_last_steps.store(0, std::memory_order_relaxed);
         diag::gc_last_pass_ns.store(ns, std::memory_order_relaxed);
         diag::store_max(diag::gc_max_pass_ns, ns);
         i_gc_passes.fetch_add(1, std::memory_order_relaxed);
         i_gc_last_keys.store(0, std::memory_order_relaxed);
         i_gc_last_steps.store(0, std::memory_order_relaxed);
         i_gc_last_pass_ns.store(ns, std::memory_order_relaxed);
         diag::store_max(i_gc_max_pass_ns, ns);
         advance_reclaim_epoch();
         reclaim_retired();
         return false;
     }

     if (gc_cursor_ >= total) gc_cursor_ = 0;

     size_t idx = gc_cursor_;
     size_t keys_budget = std::min(GC_KEYS_PER_PASS, total);

     for (size_t k = 0; k < keys_budget; ++k) {
         if (steps_done >= GC_STEPS_PER_PASS) break;  // v18.1: step budget
         if (idx >= all_entries.size()) break;
         KeyEntry* ent = all_entries[idx];
         if (!ent) { ++idx; if (idx >= total) idx = 0; continue; }

         Version* cur = ent->head.load(std::memory_order_acquire);
         bool anchor = false;
         while (cur) {
             ++steps_done;
             Version* nxt = cur->prev.load(std::memory_order_acquire);
             uint64_t c = cur->commit_ts.load(std::memory_order_acquire);
             if (c == 0) { cur = nxt; continue; }
             if (!anchor && c <= m) {
                 anchor = true;
                 stress_point("gc_anchor_sever");
                 cur->prev.store(nullptr, std::memory_order_release);
                 cur = nxt;
                 continue;
             }
             if (anchor) { retire(cur); }
             cur = nxt;
         }

         ++keys_done;
         ++idx;
         if (idx >= total) idx = 0;
     }

     gc_cursor_ = idx;

     // All nodes retired in this pass carry the pre-advance epoch.
     // A node is physically destroyed only after every active pin is
     // strictly newer (or after all pins have left).
     advance_reclaim_epoch();
     reclaim_retired();
     // v24 Fix 12: route through prune_phantom_tracker() so the
     // external floor from reader_slots_ is consulted under
     // reader_mu_ — closing the phantom-reader race window.
     prune_phantom_tracker();

     auto gc_t1 = std::chrono::steady_clock::now();
     uint64_t ns = static_cast<uint64_t>(
         std::chrono::duration_cast<std::chrono::nanoseconds>(gc_t1 - gc_t0).count());

     diag::gc_passes.fetch_add(1, std::memory_order_relaxed);
     diag::gc_last_keys.store(keys_done, std::memory_order_relaxed);
     diag::gc_last_steps.store(steps_done, std::memory_order_relaxed);
     diag::gc_last_pass_ns.store(ns, std::memory_order_relaxed);
     diag::store_max(diag::gc_max_pass_ns, ns);
     i_gc_passes.fetch_add(1, std::memory_order_relaxed);
     i_gc_last_keys.store(keys_done, std::memory_order_relaxed);
     i_gc_last_steps.store(steps_done, std::memory_order_relaxed);
     i_gc_last_pass_ns.store(ns, std::memory_order_relaxed);
     diag::store_max(i_gc_max_pass_ns, ns);

     return keys_done < total;
 }

    void free_all() {
        // v25.1 M1.6: walk the tree to free all version chains + KeyEntry objects.
        auto pairs = tree_scan("", std::string(255, '\xFF'));
        for (auto& [k, v] : pairs) {
            KeyEntry* e = decode_ptr(v);
            if (!e) continue;
            Version* c = e->head.load(std::memory_order_relaxed);
            while (c) { Version* n = c->prev.load(std::memory_order_relaxed); delete c; c = n; }
            e->head.store(nullptr, std::memory_order_relaxed);
            delete e;
        }
    }

    void link_version(KeyEntry* e, uint64_t ts, const std::string& v, bool deleted = false) {
        Version* head = e->head.load(std::memory_order_relaxed);
        // Cheap-tier invariant: commit_ts strictly increases toward the head.
        // O(1) — compare only against the immediately-preceding head.
        if (head) {
            uint64_t head_ts = head->commit_ts.load(std::memory_order_relaxed);
            assert(ts > head_ts && "invariant: new commit_ts must exceed head commit_ts");
        }
        Version* n = new Version(ts, v, deleted, head);
        diag::gc_created.fetch_add(1, std::memory_order_relaxed);
        i_gc_created.fetch_add(1, std::memory_order_relaxed);
        e->head.store(n, std::memory_order_release);
        e->last_write_ts.store(ts, std::memory_order_relaxed);
    }

    // v25.1 M1.6: version_at takes KeyEntry* directly (was int idx).
    std::optional<std::pair<std::string, bool>> version_at(KeyEntry* e, uint64_t ts) const {
        if (!e) return std::nullopt;
        Version* cur = e->head.load(std::memory_order_acquire);
        while (cur) {
            uint64_t c = cur->commit_ts.load(std::memory_order_acquire);
            if (c != 0 && c <= ts) return {{cur->value, cur->deleted}};
            cur = cur->prev.load(std::memory_order_acquire);
        }
        return std::nullopt;
    }

public:
#ifdef CHRONOKV_VERIFY_FULL
    // Expensive-tier sweep: O(total versions) across the whole database.
    // For every key's version chain, checks:
    //   (a) acyclicity       - no pointer visited twice
    //   (b) unique commit_ts - no duplicate commit_ts within one chain
    //   (c) strict ordering  - commit_ts strictly decreases head -> tail
    // NEVER call in the commit hot path. Periodic / slow-CI only.
    void verify_full() {
        // v24 M-cleanup: this function used unqualified shared_lock, set,
        // and memory_order_acquire, relying on the header-wide
        // 'using namespace std;' that was removed in an earlier v24 fix.
        // CHRONOKV_VERIFY_FULL has therefore been a dead, non-compiling
        // build configuration since that removal -- nothing in this
        // project's own sanitizer matrix ever defines it, so this was
        // never caught. Restored explicit std:: qualification.
        SnapshotGuard pin(*this);
        // v25.1 M1.6: walk the tree instead of name2idx_ + entries_.
        auto pairs = tree_scan("", std::string(255, '\xFF'));
        for (auto& [k, v] : pairs) {
            KeyEntry* e = decode_ptr(v);
            if (!e) continue;
            std::set<const Version*> visited;
            std::set<uint64_t> seen_ts;
            Version* cur = e->head.load(std::memory_order_acquire);
            uint64_t prev_ts = UINT64_MAX;
            while (cur) {
                assert(visited.insert(cur).second && "verify_full: cycle in version chain");
                uint64_t c = cur->commit_ts.load(std::memory_order_acquire);
                if (c != 0) {
                    assert(seen_ts.insert(c).second && "verify_full: duplicate commit_ts in chain");
                    assert(c < prev_ts && "verify_full: chain not strictly decreasing");
                    prev_ts = c;
                }
                cur = cur->prev.load(std::memory_order_acquire);
            }
        }
    }
#endif

public:
    explicit ChronoKV(const std::string& wal_dir = "",
                      size_t page_pool_bytes = 256ULL * 1024 * 1024) {
         wal_dir_ = wal_dir;
        // v25.1 M1.6: initialize the B+ tree index. Pool size is configurable
        // (default 256 MiB for production; tests pass a smaller size via
        // Options::page_pool_bytes to avoid VM pressure on the 2-CPU test VM).
        pool_ = std::make_unique<chronokv_page::PagePool>(page_pool_bytes);
        tree_ = std::make_unique<chronokv_btree::BTree>(*pool_);
        if (!wal_dir.empty()) {
            wal_ = std::make_unique<WalSegments>(wal_dir);
            // v24 FIX (Group 2, Fix 5): seed clock_ from existing WAL
            // contents so the first commit doesn't reserve cts=1 and
            // collide with existing records. This is unconditional
            // (independent of recover_on_open) because even an
            // append-only open needs clock_ > max existing cts.
            // recover_with_checkpoint() will overwrite this with the
            // exact value when full recovery is requested.
            uint64_t seed = WalSegments::seed_clock_from_wal(wal_dir);
            if (seed + 1 > clock_.load(std::memory_order_relaxed)) {
                clock_.store(seed + 1, std::memory_order_relaxed);
            }
        }
        // v25.1 M0.6: register this instance LAST, after all throwing
        // operations (WalSegments construction, clock seeding) have
        // completed. If the constructor throws, register_instance()
        // is never called, so the destructor (which wouldn't run
        // anyway on a partially-constructed object) doesn't need to
        // deregister — the registry never got a dangling pointer.
        register_instance();
    }

    // v20 M1: per-instance introspection API. Aggregates from sub-objects
    // (wal_, pub_, phantom_tracker_) and ChronoKV's own per-instance counters.
    // Pure snapshot; read-only; no mutable counter leakage.
    WalInstanceStats wal_stats() const {
        if (!wal_) return WalInstanceStats{};
        auto w = wal_->wal_stats();
        return WalInstanceStats{w.batches, w.records, w.bytes, w.fsyncs,
                                w.write_fails, w.fsync_fails, w.truncations,
                                w.truncate_fails, w.async_lost,
                                w.pub_allocated_max};
    }
    EpochInstanceStats epoch_stats() const {
        auto e = phantom_tracker_.epoch_stats();
        return EpochInstanceStats{e.entries, e.oldest, e.newest};
    }
    GcInstanceStats gc_stats() const {
        return GcInstanceStats{
            i_gc_created.load(std::memory_order_relaxed),
            i_gc_retired.load(std::memory_order_relaxed),
            i_gc_reclaimed.load(std::memory_order_relaxed),
            retired_pending_.load(std::memory_order_acquire),
            reclaim_epoch_.load(std::memory_order_acquire),
            [&]() -> uint64_t {
                const uint64_t min_epoch = min_active_pin_epoch();
                return min_epoch == UINT64_MAX ? 0 : min_epoch;
            }(),
            i_gc_passes.load(std::memory_order_relaxed),
            i_gc_last_keys.load(std::memory_order_relaxed),
            i_gc_last_steps.load(std::memory_order_relaxed),
            i_gc_last_pass_ns.load(std::memory_order_relaxed),
            i_gc_max_pass_ns.load(std::memory_order_relaxed)};
    }
    uint64_t published_watermark() const { return pub_.published(); }
    // v25.1 M1.6: debug counter for fence re-check retries (defined out-of-line after BTree).
    uint64_t fence_recheck_retries() const;
    // v25.1 M1.6: debug counter for ensure_index loser-delete branch.
    uint64_t ensure_index_loser_deletes() const;

    // v25.1 M0.6: per-instance dump method. This is the migration target
    // for diag::dump() — M6 will retire diag::dump() entirely and call
    // this instead. Reads only per-instance atomics (the i_xxx set from
    // v20 M1), no global state. Safe to call on any ChronoKV instance,
    // including under fork (the child's instance has its own atomics).
    void dump_instance_diagnostics(std::ostream& os) const {
        uint64_t created = i_gc_created.load(std::memory_order_relaxed),
                 retired = i_gc_retired.load(std::memory_order_relaxed),
                 reclaimed = i_gc_reclaimed.load(std::memory_order_relaxed);
        uint64_t allocated = pub_allocated(),
                 published = pub_.published();
        // WAL stats come from the WalSegments instance via its public
        // wal_stats() API (the per-instance atomics are private to WalSegments).
        WalSegments::WalStats w = wal_ ? wal_->wal_stats() : WalSegments::WalStats{};
        uint64_t batches = w.batches, records = w.records;
        os << "\n--- instance diagnostics ---\n"
           << "gc:       created=" << created << " retired=" << retired
           << " reclaimed=" << reclaimed
           << " live=" << (created - reclaimed)
           << " passes=" << i_gc_passes.load(std::memory_order_relaxed)
           << " last_keys=" << i_gc_last_keys.load(std::memory_order_relaxed)
           << " last_steps=" << i_gc_last_steps.load(std::memory_order_relaxed)
           << " max_pass_us=" << (i_gc_max_pass_ns.load(std::memory_order_relaxed) / 1000) << "\n"
           << "publish:  allocated=" << allocated << " published=" << published
           << " lag=" << (allocated > published ? allocated - published : 0) << "\n"
           << "wal:      batches=" << batches << " records=" << records
           << " avg_batch=" << (batches ? (double)records / batches : 0.0)
           << " bytes=" << w.bytes << " fsyncs=" << w.fsyncs
           << " write_fails=" << w.write_fails
           << " fsync_fails=" << w.fsync_fails
           << " truncations=" << w.truncations
           << " async_committed_then_lost=" << w.async_lost
           << " truncate_fails=" << w.truncate_fails << "\n"
           << "epoch:    entries=" << phantom_tracker_.epoch_stats().entries
           << " oldest=" << phantom_tracker_.epoch_stats().oldest
           << " newest=" << phantom_tracker_.epoch_stats().newest << "\n";
    }

    // v25.1 M0.6: instance registry for the M6 diag:: retirement.
    // Each ChronoKV instance registers itself on construction and
    // deregisters on destruction. diag::dump() (M6 deliverable) will
    // iterate this registry to produce the process-wide summary from
    // per-instance state, replacing the dual-write globals.
    // For M0, the registry exists but is not yet used by dump() —
    // the dual-write globals remain until M6 verifies the per-instance
    // path is complete and the regression matrix is green.
private:
    struct InstanceNode {
        ChronoKV* instance;
        InstanceNode* next;
    };
    InstanceNode instance_node_{this, nullptr};
    static std::mutex instance_registry_mu_;
    static InstanceNode* instance_registry_head_;
    void register_instance() {
        std::lock_guard<std::mutex> lk(instance_registry_mu_);
        instance_node_.next = instance_registry_head_;
        instance_registry_head_ = &instance_node_;
    }
    void deregister_instance() {
        std::lock_guard<std::mutex> lk(instance_registry_mu_);
        InstanceNode** pp = &instance_registry_head_;
        while (*pp && *pp != &instance_node_) pp = &(*pp)->next;
        if (*pp) *pp = instance_node_.next;
    }
public:

 // v23 Phase B: number of retired nodes awaiting reclamation (Phase C).
 uint64_t retired_pending() const {
     return retired_pending_.load(std::memory_order_relaxed);
 }
    uint64_t pub_allocated() const {
        uint64_t no_wal = i_pub_allocated_no_wal.load(std::memory_order_relaxed);
        uint64_t wal_max = wal_ ? wal_->wal_stats().pub_allocated_max : 0;
        return std::max(no_wal, wal_max);
    }
    uint64_t recovery_records() const {
        return i_rec_records.load(std::memory_order_relaxed);
    }

    // v20 M1: health status derived from per-instance counters.
    HealthInfo health() const {
        HealthInfo h{0, {}};
        if (wal_ && wal_->is_failed()) {
            h.level = 2;
            h.reasons.push_back("wal fail-stop mode active");
            return h;
        }
        auto w = wal_stats();
        if (w.write_fails > 0) {
            h.level = std::max(h.level, 1);
            h.reasons.push_back("wal write failures: " + std::to_string(w.write_fails));
        }
        if (w.fsync_fails > 0) {
            h.level = std::max(h.level, 1);
            h.reasons.push_back("wal fsync failures: " + std::to_string(w.fsync_fails));
        }
        if (w.async_lost > 0) {
            h.level = std::max(h.level, 1);
            h.reasons.push_back("async commits lost: " + std::to_string(w.async_lost));
        }
        if (w.truncate_fails > 0) {
            h.level = std::max(h.level, 1);
            h.reasons.push_back("wal truncate failures: " + std::to_string(w.truncate_fails));
        }
        if (h.level == 0) h.reasons.push_back("all subsystems nominal");
        return h;
    }

    // =================================================================
    // v20 M2: per-subsystem invariant verifiers. Read-only, debug/test.
    // Each returns true if the subsystem's invariants hold.
    // =================================================================

    // Verify a single checkpoint file is structurally valid (magic, CRC,
    // entry parsing). Does NOT apply the entries — read-only.
    static bool verify_ckpt_file(const std::string& path, std::string* reason = nullptr) {
        std::ifstream cf(path, std::ios::binary);
        if (!cf) {
            if (reason) *reason = "cannot open: " + path;
            return false;
        }
        std::vector<uint8_t> buf(
            (std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
        if (buf.size() < 8) {
            if (reason) *reason = "too small: " + path;
            return false;
        }
        uint32_t magic = 0, crc = 0;
        for (int i = 0; i < 4; ++i) magic |= static_cast<uint32_t>(buf[i]) << (8*i);
        for (int i = 0; i < 4; ++i) crc   |= static_cast<uint32_t>(buf[4+i]) << (8*i);
        if (magic != 0x434B5054) {
            if (reason) *reason = "bad magic: " + path;
            return false;
        }
        if (crc32(buf.data()+8, buf.size()-8) != crc) {
            if (reason) *reason = "CRC mismatch: " + path;
            return false;
        }
        // Walk entries to verify structure (read-only, no apply).
        size_t q = 8;
        auto pu64 = [&]() { uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t(buf[q++])<<(8*i)); return v; };
        auto pu32 = [&]() { uint32_t v=0; for(int i=0;i<4;++i) v|=(uint32_t(buf[q++])<<(8*i)); return v; };
        auto pu16 = [&]() { uint16_t v=0; for(int i=0;i<2;++i) v|=(uint16_t(buf[q++])<<(8*i)); return v; };
        if (q + 8 > buf.size()) { if (reason) *reason = "truncated cts: " + path; return false; }
        (void)pu64();  // checkpoint cts
        if (q + 4 > buf.size()) { if (reason) *reason = "truncated nk: " + path; return false; }
        uint32_t nk = pu32();
        for (uint32_t i = 0; i < nk; ++i) {
            if (q + 2 > buf.size()) { if (reason) *reason = "truncated keylen: " + path; return false; }
            uint16_t kl = pu16();
            if (q + kl > buf.size()) { if (reason) *reason = "truncated key: " + path; return false; }
            q += kl;
            if (q + 8 > buf.size()) { if (reason) *reason = "truncated commit_ts: " + path; return false; }
            q += 8;
            if (q + 4 > buf.size()) { if (reason) *reason = "truncated vallen: " + path; return false; }
            uint32_t vl = pu32();
            if (q + vl > buf.size()) { if (reason) *reason = "truncated val: " + path; return false; }
            q += vl;
            if (q + 1 > buf.size()) { if (reason) *reason = "truncated deleted: " + path; return false; }
            q += 1;
        }
        return true;
    }

    // Verify checkpoint chain: base + all deltas parse cleanly.
    // v24 FIX (Group 2, Fix 9): the previous loop stopped at the first
    // missing numbered delta, which means a gap (delta.1, delta.3 with
    // delta.2 missing) would silently skip delta.3 and report success.
    // Now we enumerate all matching files via directory scan, collect
    // the delta numbers, sort them, require contiguity starting at 1,
    // and verify each. This catches gaps AND any non-numeric or
    // out-of-pattern files in the directory.
    static bool verify_checkpoint_chain(const std::string& ckpt_path, std::string* reason = nullptr) {
        if (!std::filesystem::exists(ckpt_path)) {
            if (reason) *reason = "base missing: " + ckpt_path;
            return false;
        }
        if (!verify_ckpt_file(ckpt_path, reason)) return false;

        // Enumerate delta files via directory scan.
        std::vector<uint64_t> delta_nums;
        std::filesystem::path base_p(ckpt_path);
        std::filesystem::path parent_p = base_p.parent_path();
        if (parent_p.empty()) parent_p = ".";
        std::string base_name = base_p.filename().string();
        std::string delta_prefix = base_name + ".delta.";
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(parent_p, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            std::string fn = entry.path().filename().string();
            if (fn.size() <= delta_prefix.size() ||
                fn.compare(0, delta_prefix.size(), delta_prefix) != 0) {
                continue;
            }
            // After the prefix, the remainder must be all decimal digits.
            std::string digits = fn.substr(delta_prefix.size());
            if (digits.empty()) continue;
            bool all_digits = true;
            for (char c : digits) {
                if (c < '0' || c > '9') { all_digits = false; break; }
            }
            if (!all_digits) continue;
            // Parse the delta number (cap at uint64_t).
            uint64_t n = 0;
            for (char c : digits) {
                n = n * 10 + static_cast<uint64_t>(c - '0');
                if (n > 1000000000ULL) { all_digits = false; break; }  // sanity cap
            }
            if (!all_digits) continue;
            delta_nums.push_back(n);
        }
        std::sort(delta_nums.begin(), delta_nums.end());

        // Require contiguity starting at 1: 1, 2, 3, ...
        for (size_t i = 0; i < delta_nums.size(); ++i) {
            if (delta_nums[i] != i + 1) {
                if (reason) *reason = "delta chain gap: expected delta." +
                    std::to_string(i + 1) + " but found delta." +
                    std::to_string(delta_nums[i]);
                return false;
            }
            std::string dp = ckpt_path + ".delta." + std::to_string(delta_nums[i]);
            if (!verify_ckpt_file(dp, reason)) return false;
        }
        return true;
    }

    // Verify WAL integrity: all segments parse cleanly via recover_all.
    static bool verify_wal_dir(const std::string& wal_dir, std::string* reason = nullptr) {
        if (wal_dir.empty()) {
            return true;  // vacuously true
        }
        uint64_t ckpt_ts = 0;
        try {
            auto [status, records] = WalSegments::recover_all(wal_dir, ckpt_ts);
            if (status == WalStatus::CORRUPT) {
                if (reason) *reason = "WAL corruption detected";
                return false;
            }
            return true;  // OK or TORN_TAIL (unclean shutdown, not corruption)
        } catch (const std::exception& e) {
            if (reason) *reason = std::string("WAL exception: ") + e.what();
            return false;
        }
    }

    // Instance-level convenience: verify this instance's WAL.
    bool verify_wal(std::string* reason = nullptr) const {
        return verify_wal_dir(wal_dir_, reason);
    }

    // Verify publication invariant R1: published < clock.
    bool verify_publication(std::string* reason = nullptr) const {
        uint64_t pub = pub_.published();
        uint64_t clk = clock_.load();
        if (pub >= clk) {
            if (reason) *reason = "R1 violated: published(" + std::to_string(pub) +
                                  ") >= clock(" + std::to_string(clk) + ")";
            return false;
        }
        return true;
    }

    // v23 Phase C: a local pin protects every Version dereference while
    // GC may reclaim. v25.1 M1.6: walk the tree instead of entries_.
    bool verify_version_chains(std::string* reason = nullptr) {
        SnapshotGuard pin(*this);
        auto pairs = tree_scan("", std::string(255, '\xFF'));
        for (size_t i = 0; i < pairs.size(); ++i) {
            KeyEntry* e = decode_ptr(pairs[i].second);
            if (!e) continue;
            Version* cur = e->head.load(std::memory_order_acquire);
            uint64_t prev_ts = UINT64_MAX;
            bool saw_committed = false;
            while (cur) {
                uint64_t ts = cur->commit_ts.load(std::memory_order_acquire);
                if (ts >= prev_ts) {
                    if (reason) *reason = "version chain not strictly descending at entry " + std::to_string(i);
                    return false;
                }
                if (saw_committed && ts == 0) {
                    if (reason) *reason = "commit_ts=0 below a committed node at entry " + std::to_string(i);
                    return false;
                }
                if (ts != 0) saw_committed = true;
                prev_ts = ts;
                cur = cur->prev.load(std::memory_order_acquire);
            }
        }
        return true;
    }

    // v20.1 (Hardening 6): verify reader-slot bookkeeping integrity: no
    // duplicate or out-of-bounds free indices, and every free slot is at
    // snapshot 0. (Active slots are intentionally NOT asserted non-zero here
    // because acquire_slot publishes the snapshot just after taking the slot
    // out of the free list, outside reader_mu_.)
    bool verify_reader_slots(std::string* reason = nullptr) const {
        std::lock_guard<std::mutex> lk(reader_mu_);
        std::set<size_t> seen;
        for (size_t idx : reader_free_) {
            if (idx >= reader_slots_.size()) {
                if (reason) *reason = "free slot index out of bounds: " + std::to_string(idx);
                return false;
            }
            if (!seen.insert(idx).second) {
                if (reason) *reason = "duplicate free slot index: " + std::to_string(idx);
                return false;
            }
            if (reader_slots_[idx]->snapshot.load(std::memory_order_seq_cst) != 0 ||
                reader_slots_[idx]->pinned_epoch != 0) {
                if (reason) *reason = "free slot " + std::to_string(idx) +
                                      " is still snapshot-visible or epoch-pinned";
                return false;
            }
        }
        return true;
    }

    ~ChronoKV() {
        { std::lock_guard<std::mutex> lk(gc_mu_); running_.store(false, std::memory_order_release); }
        gc_cv_.notify_all();
        if (gc_.joinable()) gc_.join();
        free_all();
        // v25.1 M0.6: deregister before destruction.
        deregister_instance();
    }

    // Item 8: choose durability mode. Async weakens Visible=Durable to
    // Visible>=Durable (bounded-loss window = until next successful group fsync).
    void set_durability(DurabilityMode m) { durability_ = m; }

    // v20 M3: configure rebase thresholds (tests lower these to exercise rebase).
    void set_rebase_threshold(int count, uint64_t bytes) {
        rebase_count_threshold_ = count;
        rebase_bytes_threshold_ = bytes;
    }
    double last_ckpt_lock_wait_ms() const { return last_ckpt_lock_wait_ms_; }
    double last_ckpt_work_ms() const { return last_ckpt_work_ms_; }

    // v18 M5: Replication readiness spike.
     // Extract all WAL records in LSN order for shipping to a follower.
     std::vector<std::pair<uint64_t, WriteSet>> extract_wal_records() {
         if (wal_dir_.empty()) return {};
         uint64_t ckpt_ts = 0;
         auto [status, records] = WalSegments::recover_all(wal_dir_, ckpt_ts);
         return records;
     }

     // Replay records from a replication log (follower apply).
     // Idempotent: records with CTS <= published are skipped.
     ReplayResult replay_records(const std::vector<std::pair<uint64_t, WriteSet>>& records) {
         uint64_t pub = pub_.published();
         uint64_t expected = pub + 1;
         for (auto& [ts, ws] : records) {
             if (ts <= pub) continue;       // already applied
             if (ts != expected) return ReplayResult::GapDetected;  // gap or duplicate
             std::vector<std::tuple<KeyEntry*, std::string, std::string, bool>> witems;
             for (auto& [k, v, d] : ws) {
                 auto [e, dummy] = ensure_index(k);
                 witems.push_back({e, k, v, d});
             }
             for (auto& [e, k, v, d] : witems) link_version(e, ts, v, d);
             pub_.complete(ts);
             expected = ts + 1;
         }
         clock_.store(expected, std::memory_order_seq_cst);
     return ReplayResult::OK;
     }

     void gc_pass_for_test() {
         std::lock_guard<std::mutex> lk(gc_active_mu_);
         gc_once();
     }

     void start_gc() {
        // v20.1 BLOCKER #12: idempotent — assigning a new std::thread over a
        // joinable one calls std::terminate.
        // v24 FIX (Group 2, Fix 6): the previous check-then-set was a TOCTOU
        // race. Two threads calling start_gc() concurrently could both see
        // running_==false, both set it to true, and both assign gc_ -- the
        // second assignment over a joinable std::thread calls std::terminate.
        // Use compare_exchange_strong to make the check-and-claim atomic:
        // exactly one caller observes running_==false and proceeds; all
        // others observe running_==true (set by the winner) and return.
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;  // already running
        }
        // v25.1 M1.6: if thread creation fails (EAGAIN from pthread_create
        // under memory/thread pressure), reset running_ to false so the
        // state is consistent. Previously, running_ stayed true with no
        // thread created — a latent inconsistency where ~ChronoKV would
        // skip gc_.join() (not joinable) but running_==true suggested a
        // live thread. Now: if creation fails, roll back the claim so a
        // future start_gc() call can retry.
        try {
            gc_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(gc_mu_);
            while (running_.load(std::memory_order_acquire)) {
                gc_cv_.wait(lk, [this] {
                    return !running_.load(std::memory_order_acquire) ||
                           dirty_.load(std::memory_order_acquire) ||
                           retired_pending_.load(std::memory_order_relaxed) != 0;
                });
                if (!running_.load(std::memory_order_acquire)) break;
                dirty_.store(false, std::memory_order_release);
                lk.unlock();
                bool more;
             do {
                 { std::lock_guard<std::mutex> gc_act(gc_active_mu_); more = gc_once(); }
                 if (more) std::this_thread::yield();
             } while (more && running_.load(std::memory_order_acquire));
                // v17: PhantomTracker.prune() is called from gc_once() under
             // gc_active_mu_. It prunes transitions with commit_ts <= the
             // minimum active reader snapshot. This is safe because
             // ReadWriteTransaction registers its snapshot at construction
             // and deregisters at commit/destruction.
                lk.lock();
            }
        });
        } catch (...) {
            // Thread creation failed (EAGAIN). Roll back the running_ claim
            // so the state is consistent and a future start_gc() can retry.
            running_.store(false, std::memory_order_release);
            throw;
        }
    }

    void recover(const std::string& wal_dir) {
        uint64_t ckpt_ts = 0;
        auto [status, records] = WalSegments::recover_all(wal_dir, ckpt_ts);
        if (status == WalStatus::CORRUPT)
            throw std::runtime_error("WAL CORRUPTION DETECTED");

        // Sort by timestamp and verify contiguity (invariant I2)
        sort(records.begin(), records.end(),
             [](const auto& a, const auto& b) { return a.first < b.first; });

        uint64_t expected = 1;
        uint64_t contiguous = 0;
        uint64_t last_replayed = 0;
        for (auto& [ts, ws] : records) {
            // Cheap-tier defence-in-depth: replay order is non-decreasing.
            assert(ts >= last_replayed && "invariant I3: WAL replay must be non-decreasing");
            last_replayed = ts;
            if (ts < expected)
                throw std::runtime_error("Recovery failed: duplicate commit_ts=" + std::to_string(ts)
                    + " (expected " + std::to_string(expected) + ")"
                    + " — WAL corruption or segment overlap");
            if (ts > expected)
                throw std::runtime_error(std::string("Recovery failed: interior gap")
                    + " expected_ts=" + std::to_string(expected)
                    + " found_ts=" + std::to_string(ts)
                    + " — WAL corruption detected");
            // ts == expected: replay
            for (auto& [k, v, d] : ws) {
                auto [e, is_new] = ensure_index(k);
                link_version(e, ts, v, d);
            }
            diag::rec_records.fetch_add(1, std::memory_order_relaxed);
            i_rec_records.fetch_add(1, std::memory_order_relaxed);
            contiguous = ts;
            expected++;
        }

        if (contiguous > 0) {
            clock_.store(contiguous + 1, std::memory_order_relaxed);
            pub_.recover_to(contiguous);
            assert(pub_.published() < clock_.load() && "invariant R1: published_ < clock_ after recovery");
        }
    }

    TxnResult commit(const std::string& key, const std::string& value) {
#ifdef CHRONOKV_RECORD_HISTORY
        HistScope hs("write", key);
#endif
        uint64_t cts = 0;
        TxnResult r = commit_txn(UINT64_MAX, {{key, value, false}}, {}, {}, &cts);
#ifdef CHRONOKV_RECORD_HISTORY
        hs.set_lts(cts);
        hs.set_result(to_string(r));
#endif
        return r;
    }

    TxnResult del(const std::string& key) {
#ifdef CHRONOKV_RECORD_HISTORY
        HistScope hs("delete", key);
#endif
        uint64_t cts = 0;
        TxnResult r = commit_txn(UINT64_MAX, {{key, "", true}}, {}, {}, &cts);
#ifdef CHRONOKV_RECORD_HISTORY
        hs.set_lts(cts);
        hs.set_result(to_string(r));
#endif
        return r;
    }

    TxnResult commit_txn(uint64_t read_ts, const WriteSet& ws,
                         const std::set<std::string>& rs,
                         const std::vector<RangeRead>& range_reads = {},
                         uint64_t* out_cts = nullptr) {
        if (out_cts) *out_cts = 0;
        if (ws.empty()) return TxnResult::Committed;
\
     // v17 Work Stream D: API-level size limits. Reject oversized keys or
     // values before locking, index creation, or WAL serialization. The
     // serialization-layer throw remains as a defense-in-depth backstop.
     for (auto& [k, v, d] : ws) {
         if (k.size() > MAX_KEY_BYTES || v.size() > MAX_VALUE_BYTES)
             return TxnResult::TooLarge;
     }

     // v24 FIX (Group 1, Fix 1c): aggregate-size pre-check.
     // wal_ser() performs a TOTAL-size check (o.size() + 8 > WAL_MAX_RECORD)
     // AFTER serializing all entries. For a multi-key transaction whose
     // individually-compliant values sum past 1 MiB, that check fires AFTER
     // group_append() has already reserved a ts via clock.fetch_add(1) --
     // orphaning the ts in the publication tracker and stalling the engine.
     // (Fix 1b's try/catch surfaces this as WalFailure, but the ts is still
     // burned rather than never reserved -- wasteful and confusing.)
     //
     // Compute the exact serialized payload size here, BEFORE any locking
     // or ts reservation, and reject with TooLarge if it would exceed the
     // WAL framing limit. The payload format is:
     //   [ts:8][n:4] + per-entry { [klen:2][key][vlen:4][value][deleted:1] }
     // wal_ser() checks o.size() + 8 > WAL_MAX_RECORD (the +8 accounts for
     // the LSN that wal_prepend_lsn() adds). So the payload limit is
     // WAL_MAX_RECORD - 8.
     //
     // Only applies when WAL is enabled (in-memory databases have no
     // framing limit). Use size_t arithmetic to avoid overflow.
     if (wal_) {
         size_t payload = 8 + 4;  // ts + n
         for (auto& [k, v, d] : ws) {
             payload += 2 + k.size() + 4 + v.size() + 1;
             // Early exit if we've already exceeded -- avoids potential
             // size_t overflow on a pathological WriteSet with huge values
             // (each individually <= MAX_VALUE_BYTES, but the sum could
             // be large; size_t overflow requires ~2^64 bytes which is
             // impossible in practice, but the early exit is free).
             if (payload + 8 > WAL_MAX_RECORD) {
                 return TxnResult::TooLarge;
             }
         }
     }

        std::shared_lock ckpt_lk(checkpoint_mu_);
        if (wal_ && wal_->is_failed()) return TxnResult::DatabaseFailed;

        std::vector<std::tuple<KeyEntry*, std::string, std::string, bool>> witems;
        std::set<std::string> new_keys;
        // v20.1 BLOCKER #6: reject duplicate keys in the raw write set. Without
        // this, two entries for the same key are both installed at the same cts,
        // violating the strictly-increasing version-chain invariant. Higher-level
        // APIs (ReadWriteTransaction) already dedup via std::map.
        for (auto& [k, v, d] : ws) {
            if (!new_keys.insert(k).second) return TxnResult::InvalidTransaction;
        }
        for (auto& [k, v, d] : ws) {
            auto [e, is_new] = ensure_index(k);
            witems.emplace_back(e, k, v, d);
        }

        // v25.1 M1.6: lock ordering by KeyEntry* address (deterministic, avoids
        // deadlock). Replaces the old int-index ordering (all_idx sorted ints).
        // KeyEntry* is stable (heap-allocated, engine-owned, deferred reclamation).
        std::set<KeyEntry*> all_entries;
        for (auto& [e, k, v, d] : witems) all_entries.insert(e);
        for (auto& k : rs) {
            auto [e, dummy] = ensure_index(k);
            all_entries.insert(e);
        }

        // v25.1 M1.6: the stress_point("after_ensure_index") guard is now MOOT.
        // It existed to catch entries_ vector reallocation hazards (v15 era).
        // With the B+ tree, there is no vector — KeyEntry* is returned directly
        // by ensure_index and is stable (heap address never changes). No
        // equivalent hazard exists. Kept as a no-op marker for test continuity;
        // the Phase 1 test verifies single-key correctness without the old guard.
        stress_point("after_ensure_index");  // M1.6: moot, kept for test continuity

        // v25.1 M1.6: no separate "fetch stable pointers under nm_" step needed.
        // ensure_index returns KeyEntry* which IS the stable pointer (the old
        // emap step existed because entries_[i].get() could become dangling if
        // the vector reallocated; KeyEntry* has no such hazard).

        std::vector<std::unique_lock<std::mutex>> commit_locks;
     commit_locks.reserve(all_entries.size());
     for (auto* e : all_entries) commit_locks.emplace_back(e->commit_mu);

        for (auto* e : all_entries) {
            if (e->last_write_ts.load(std::memory_order_relaxed) > read_ts) {
                // commit_locks release mutexes by RAII
                return TxnResult::Conflict;
            }
        }

        for (auto& rr : range_reads) {
            if (phantom_tracker_.has_phantom_in_range(rr.lo, rr.hi, rr.snapshot_ts)) {
                // commit_locks release mutexes by RAII
                return TxnResult::Conflict;
            }
        }

        // v17 race closure: compute logical existence transitions while holding
     // the relevant commit_mu locks. Phantom validation and transition
     // publication occur inside WAL timestamp reservation so commit-timestamp
     // order and phantom order are identical.
     std::vector<std::tuple<std::string, bool, bool>> transitions;
     for (auto& [e, k, v, d] : witems) {
         Version* h = e->head.load(std::memory_order_acquire);
         bool old_exists = h && !h->deleted;
         bool new_exists = !d;
         if (old_exists != new_exists)
             transitions.emplace_back(k, old_exists, new_exists);
     }

     auto on_reserve = [&](uint64_t reserved_cts) -> bool {
         for (auto& rr : range_reads) {
             if (phantom_tracker_.has_phantom_in_range(rr.lo, rr.hi, rr.snapshot_ts))
                 return false;
         }
         for (auto& [k, old_exists, new_exists] : transitions)
             phantom_tracker_.record_transition(reserved_cts, k, old_exists, new_exists);
         return true;
     };

     uint64_t cts = 0;
     bool wal_ok = false;
     bool phantom_conflict = false;

     if (wal_) {
         // v24 FIX (Group 1, Fix 1b): group_append reserves cts via
         // clock.fetch_add(1) BEFORE doing anything that can throw
         // (wal_ser, push_back, on_reserve). Group 1, Fix 2 (below)
         // makes group_append itself burn-on-throw. HERE, we catch
         // any exception that escapes group_append and surface it as
         // WalFailure so the caller doesn't see a stale cts leak.
         // (We cannot burn cts here because we don't know whether
         // fetch_add was reached; group_append's internal catch
         // handles that.)
         uint64_t ts = 0;
         int status = 0;
         try {
             auto [s, t] = wal_->group_append(clock_, ws, durability_, on_reserve);
             status = s;
             ts = t;
         } catch (...) {
             // group_append's own internal catch (Group 1, Fix 2)
             // guarantees any ts it reserved was already burned.
             return TxnResult::WalFailure;
         }
         cts = ts;
         if (status == 2) phantom_conflict = true;
         wal_ok = (status == 0);
     } else {
         std::lock_guard<std::mutex> order_lk(order_mu_);

         for (auto& rr : range_reads) {
             if (phantom_tracker_.has_phantom_in_range(rr.lo, rr.hi, rr.snapshot_ts)) {
                 phantom_conflict = true;
                 break;
             }
         }

         if (!phantom_conflict) {
             // v24 FIX (Group 1, Fix 1b, no-WAL path): clock_.fetch_add
             // reserves cts before record_transition / link_version can
             // throw. If any of those throw, burn the reserved cts.
             cts = clock_.fetch_add(1, std::memory_order_seq_cst);
             diag::store_max(diag::pub_allocated, cts);
             diag::store_max(i_pub_allocated_no_wal, cts);
             try {
                 for (auto& [k, old_exists, new_exists] : transitions)
                     phantom_tracker_.record_transition(cts, k, old_exists, new_exists);
                 wal_ok = true;
             } catch (...) {
                 pub_.burn(cts);
                 throw;
             }
         }
     }

     if (phantom_conflict) {
         if (cts > 0) pub_.burn(cts);
         // commit_locks release mutexes by RAII
         return TxnResult::Conflict;
     }

     if (!wal_ok) {
         if (cts > 0) pub_.burn(cts);
         // commit_locks release mutexes by RAII
         return TxnResult::WalFailure;
     }

     stress_point("wal_durable_before_install");

        for (auto& [e, k, v, d] : witems) link_version(e, cts, v, d);
     {
         std::lock_guard<std::mutex> dlk(dirty_mu_);
         for (auto& [di, dk, dv, dd] : witems)
             {
                 auto dit = dirty_since_ckpt_.find(dk);
                 if (dit == dirty_since_ckpt_.end() || cts > dit->second)
                     dirty_since_ckpt_[dk] = cts;
             }
     }
        // commit_locks release mutexes by RAII

        stress_point("before_publish");
        pub_.complete(cts);
        {
            // Cheap-tier invariant R1: publication never outruns allocation.
            // Read published_ first, then clock_, so clock_ is at least as fresh.
            uint64_t pub_now = pub_.published();
            uint64_t clk_now = clock_.load();
            assert(pub_now < clk_now && "invariant R1: published_ must be < clock_");
        }
        wake_gc();
        if (out_cts) *out_cts = cts;
        return TxnResult::Committed;
    }

    void checkpoint(const std::string& ckpt_path) {
        struct WorkTimer {
            std::chrono::steady_clock::time_point start;
            double& out;
            WorkTimer(double& o) : start(std::chrono::steady_clock::now()), out(o) {}
            ~WorkTimer() { out = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
        };
        auto ckpt_t_enter = std::chrono::steady_clock::now();
        std::unique_lock ckpt_lk(checkpoint_mu_);
        last_ckpt_lock_wait_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ckpt_t_enter).count();
        WorkTimer ckpt_work(last_ckpt_work_ms_);

        // Wait for any in-progress GC to finish, then register a reader
        // slot so future GC cannot reclaim versions visible at cts.
        std::lock_guard<std::mutex> gc_act(gc_active_mu_);
        uint64_t snap_ts;
        int ckpt_slot = acquire_slot(snap_ts);
     std::shared_ptr<int> slot_guard(new int(ckpt_slot), [this, ckpt_slot](int* p) {
         this->release_slot(ckpt_slot);
         delete p;
     });
        uint64_t cts = snap_ts;
        stress_point("ckpt_slot_registered");

        // v18: incremental checkpoint support.
     bool base_exists = std::filesystem::exists(ckpt_path);
     bool do_rebase = delta_count_ >= rebase_count_threshold_ ||
                      delta_bytes_since_base_ >= rebase_bytes_threshold_;  // v20 M3: count OR bytes
     // v18.2: filter dirty keys by cts <= checkpoint snapshot to close
     // the dirty-clearing race. Only keys whose dirty cts is visible
     // at this snapshot should be captured and cleared.
     std::set<std::string> dirty_keys_to_capture;
     {
         std::lock_guard<std::mutex> dlk(dirty_mu_);
         for (auto& [dk, dcts] : dirty_since_ckpt_) {
             if (dcts <= cts) dirty_keys_to_capture.insert(dk);
         }
     }

     std::vector<std::tuple<std::string, uint64_t, std::string, bool>> snap;
     // v25.1 M1.6: walk the tree instead of name2idx_ + entries_.
     {
         if (base_exists && !do_rebase && !dirty_keys_to_capture.empty()) {
             // v25.1 M1.6 optimization (Option 2): batch the dirty-key lookups
             // via a single ordered tree scan over [min_dirty, max_dirty],
             // filtering for dirty keys. This replaces N individual tree_->get
             // calls (each with per-page latch overhead) with one Cursor pass
             // (shared latches held across multiple keys per leaf page).
             // For sparse dirty keys this visits extra keys in the range, but
             // amortizes latch overhead across all keys in each leaf.
             std::string min_k = *dirty_keys_to_capture.begin();
             std::string max_k = *dirty_keys_to_capture.rbegin();
             auto pairs = tree_scan(min_k, max_k);
             for (auto& [k, v] : pairs) {
                 if (dirty_keys_to_capture.find(k) == dirty_keys_to_capture.end()) continue;
                 KeyEntry* e = decode_ptr(v);
                 if (!e) continue;
                 auto ver = version_at(e, cts);
                 if (ver) {
                     auto& [val, deleted] = *ver;
                     Version* cur = e->head.load(std::memory_order_acquire);
                     uint64_t vts = 0;
                     while (cur) {
                         uint64_t c = cur->commit_ts.load(std::memory_order_acquire);
                         if (c != 0 && c <= cts) { vts = c; break; }
                         cur = cur->prev.load(std::memory_order_acquire);
                     }
                     snap.push_back({k, vts, val, deleted});
                 }
             }
         } else {
             auto pairs = tree_scan("", std::string(255, '\xFF'));
             for (auto& [k, v] : pairs) {
                 KeyEntry* e = decode_ptr(v);
                 if (!e) continue;
                 auto ver = version_at(e, cts);
                 if (ver) {
                     auto& [val, deleted] = *ver;
                     Version* cur = e->head.load(std::memory_order_acquire);
                     uint64_t vts = 0;
                     while (cur) {
                         uint64_t c = cur->commit_ts.load(std::memory_order_acquire);
                         if (c != 0 && c <= cts) { vts = c; break; }
                         cur = cur->prev.load(std::memory_order_acquire);
                     }
                     snap.push_back({k, vts, val, deleted});
                 }
             }
         }
     }
     std::vector<uint8_t> pl;
        auto u64 = [&](uint64_t v) { for(int i=0;i<8;++i) pl.push_back((v>>(8*i))&0xFF); };
        auto u32 = [&](uint32_t v) { for(int i=0;i<4;++i) pl.push_back((v>>(8*i))&0xFF); };
        auto u16 = [&](uint16_t v) { for(int i=0;i<2;++i) pl.push_back((v>>(8*i))&0xFF); };
        u64(cts);
        u32(static_cast<uint32_t>(snap.size()));
        for (auto& [k, hc, v, dl] : snap) {
            if (k.size() > 0xFFFF) throw std::runtime_error("key too large for serialization");
         u16(static_cast<uint16_t>(k.size()));
            for (char x : k) pl.push_back(static_cast<uint8_t>(x));
            u64(hc);
            u32(static_cast<uint32_t>(v.size()));
            for (char x : v) pl.push_back(static_cast<uint8_t>(x));
            pl.push_back(dl ? 1 : 0);
        }

        uint32_t crc = crc32(pl.data(), pl.size());
        std::vector<uint8_t> buf;
        auto p32 = [&](uint32_t v) { for(int i=0;i<4;++i) buf.push_back((v>>(8*i))&0xFF); };
        p32(0x434B5054);
        p32(crc);
        buf.insert(buf.end(), pl.begin(), pl.end());

        // v18: write to delta path if base exists and we have dirty keys.
     std::string write_path = ckpt_path;
     if (base_exists && !do_rebase && !dirty_keys_to_capture.empty()) {
         int delta_n = 1;
         while (std::filesystem::exists(ckpt_path + ".delta." + std::to_string(delta_n)))
             delta_n++;
         write_path = ckpt_path + ".delta." + std::to_string(delta_n);
         delta_count_++;  // v18.2: track delta chain length.
         // NOTE: incremented at filename-selection time, before write/fsync/rename.
         // On a failed write this over-counts by one; the only consequence is a
         // premature rebase (extra I/O, not data loss). Recovery probes the
         // filesystem directly and never trusts this counter.
     }
     std::string tmp = write_path + ".tmp";
        int fd = -1;
#ifdef CHRONOKV_FAULT_INJECTION
        if (!fault::fire(fault::Kind::OpenFail))
#endif
            fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) throw std::runtime_error("checkpoint open failed");
        if (!write_all(fd, buf.data(), buf.size())) { checked_close(fd); throw std::runtime_error("checkpoint write"); }
        if (!checked_fsync(fd)) { checked_close(fd); throw std::runtime_error("checkpoint fsync"); }
        if (!checked_close(fd)) throw std::runtime_error("checkpoint close");
        if (!checked_rename(tmp, write_path)) throw std::runtime_error("checkpoint rename");
        // v20 M3: rebase linearization point (new base installed, old deltas remain).
        if (write_path == ckpt_path && base_exists) stress_point("rebase_base_installed");
        if (!fsync_dir(write_path)) {
            // slot_guard releases ckpt_slot
            throw std::runtime_error("checkpoint dir fsync failed - refusing to rotate WAL");
        }

        // slot_guard releases ckpt_slot

        // v15: rotate WAL segments instead of truncating a single file
     // v18: a new full base checkpoint supersedes any previous delta chain.
     // v20 M3: accumulate delta bytes for the byte-size-aware rebase trigger.
     if (write_path != ckpt_path) delta_bytes_since_base_ += buf.size();
     // Remove old delta files after the new base is durable.
     if (write_path == ckpt_path) {
         delta_count_ = 0;  // v18.2: reset after full base
         delta_bytes_since_base_ = 0;  // v20 M3: reset byte counter on full base
         std::error_code ec;
         std::filesystem::path base_p(ckpt_path);
         std::filesystem::path parent_p = base_p.parent_path();
         if (parent_p.empty()) parent_p = ".";
         std::string prefix = base_p.filename().string() + ".delta.";
         std::filesystem::directory_iterator it(parent_p, ec);
         if (!ec) {
             for (auto& entry : it) {
                 bool is_reg = entry.is_regular_file(ec);
                 if (ec || !is_reg) continue;
                 std::string fn = entry.path().filename().string();
                 if (fn.rfind(prefix, 0) == 0)
                     std::filesystem::remove(entry.path(), ec);
             }
         }
     }

     {
         std::lock_guard<std::mutex> dlk(dirty_mu_);
         // v18.2: only clear dirty entries with cts <= checkpoint cts.
         // Entries with cts > checkpoint cts remain dirty for the next round.
         for (auto it = dirty_since_ckpt_.begin(); it != dirty_since_ckpt_.end(); ) {
             if (it->second <= cts) it = dirty_since_ckpt_.erase(it);
             else ++it;
         }
     }
        if (wal_ && !wal_->rotate_after_checkpoint(cts))
         throw std::runtime_error("WAL rotation failed after checkpoint");
    }

    void recover_with_checkpoint(const std::string& wal_dir, const std::string& ckpt_path) {
        // v20.1 (#7): recovery must run on an EMPTY engine. Recovering into a
        // populated engine would prepend duplicate versions and corrupt the
        // version chains / publication state.
        {
            // v25.1 M1.6: check tree is empty (was entries_.empty()).
            if (tree_size() > 0)
                throw std::runtime_error("recover_with_checkpoint called on a populated engine");
        }
        // v20 M3 R-REBASE: remove orphaned .tmp files left by an interrupted
        // checkpoint/rebase write. A .tmp that never got renamed is not durable
        // and must not be mistaken for a valid checkpoint.
        // v24 FIX (Group 2, Fix 9): the previous loop probed
        // ckpt_path + ".delta.N.tmp" for N=1..10000 and stopped at the
        // first missing file. That misses orphans after a gap (e.g., if
        // delta.1.tmp and delta.3.tmp exist but delta.2.tmp is missing,
        // delta.3.tmp was never deleted). Now we directory-scan for any
        // file matching "<base>.delta.<digits>.tmp" and remove them all.
        {
            std::error_code tec;
            std::filesystem::remove(ckpt_path + ".tmp", tec);
            // Directory scan for orphaned delta .tmp files.
            std::filesystem::path base_p(ckpt_path);
            std::filesystem::path parent_p = base_p.parent_path();
            if (parent_p.empty()) parent_p = ".";
            std::string base_name = base_p.filename().string();
            std::string delta_tmp_prefix = base_name + ".delta.";
            std::string delta_tmp_suffix = ".tmp";
            std::error_code scan_ec;
            for (auto& entry : std::filesystem::directory_iterator(parent_p, scan_ec)) {
                if (scan_ec) break;
                if (!entry.is_regular_file(scan_ec)) continue;
                std::string fn = entry.path().filename().string();
                if (fn.size() <= delta_tmp_prefix.size() + delta_tmp_suffix.size()) continue;
                if (fn.compare(0, delta_tmp_prefix.size(), delta_tmp_prefix) != 0) continue;
                if (fn.compare(fn.size() - delta_tmp_suffix.size(),
                               delta_tmp_suffix.size(), delta_tmp_suffix) != 0) continue;
                // Middle must be all digits.
                std::string mid = fn.substr(delta_tmp_prefix.size(),
                                            fn.size() - delta_tmp_prefix.size() - delta_tmp_suffix.size());
                if (mid.empty()) continue;
                bool all_digits = true;
                for (char c : mid) {
                    if (c < '0' || c > '9') { all_digits = false; break; }
                }
                if (!all_digits) continue;
                std::filesystem::remove(entry.path(), tec);
            }
        }
        uint64_t ckpt_cts = 0;
     // v18: load base checkpoint, then apply delta chain.
     auto parse_and_apply_ckpt = [&](const std::string& path, uint64_t& out_cts,
                                     uint64_t stale_below_cts = 0) -> bool {
         std::ifstream cf(path, std::ios::binary);
         if (!cf) return false;
         std::vector<uint8_t> buf(
             (std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
         if (buf.size() < 8) throw std::runtime_error("Checkpoint corrupt: too small (" + std::to_string(buf.size()) + " bytes): " + path);
         uint32_t magic = 0, crc = 0;
         for (int i = 0; i < 4; ++i) magic |= static_cast<uint32_t>(buf[i]) << (8*i);
         for (int i = 0; i < 4; ++i) crc   |= static_cast<uint32_t>(buf[4+i]) << (8*i);
         // v20.1 (#14): the file exists, so bad magic / bad CRC is CORRUPTION,
         // not absence. Fail loud per D1 instead of being treated as a missing
         // checkpoint (which would silently fall through to WAL-only recovery).
         if (magic != 0x434B5054) throw std::runtime_error("Checkpoint corrupt: bad magic in " + path);
         if (crc32(buf.data()+8, buf.size()-8) != crc) throw std::runtime_error("Checkpoint corrupt: CRC mismatch in " + path);
         size_t q = 8;
         auto pu64 = [&]() { uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t(buf[q++])<<(8*i)); return v; };
         auto pu32 = [&]() { uint32_t v=0; for(int i=0;i<4;++i) v|=(uint32_t(buf[q++])<<(8*i)); return v; };
         auto pu16 = [&]() { uint16_t v=0; for(int i=0;i<2;++i) v|=(uint16_t(buf[q++])<<(8*i)); return v; };
         // v20.1 BLOCKER #2: bound header reads. A valid magic+CRC file can
         // still be truncated; reading cts/nk past the buffer is a heap overflow.
         if (q + 8 > buf.size()) throw std::runtime_error("Checkpoint corrupt: truncated checkpoint cts");
         out_cts = pu64();
         // v20 M3 R-REBASE: a checkpoint whose snapshot cts is not newer than
         // the base is a stale leftover from an interrupted rebase. The base
         // already supersedes it, so skip without applying.
         if (stale_below_cts > 0 && out_cts <= stale_below_cts) return true;
         if (q + 4 > buf.size()) throw std::runtime_error("Checkpoint corrupt: truncated entry count");
         uint32_t nk = pu32();
         // v24 FIX (Group 2, Fix 10): plausibility check on nk before
         // calling reserve(nk). A corrupt or maliciously-crafted
         // checkpoint could declare nk = 4 billion, causing reserve()
         // to attempt a multi-GB allocation (and std::bad_alloc throw)
         // before any per-entry truncation check fires. Each entry
         // requires at minimum 2 (klen) + 8 (cts) + 4 (vlen) + 1 (deleted)
         // = 15 bytes. If the remaining buffer cannot possibly hold
         // nk * 15 bytes, the entry count is implausible -- fail loud
         // rather than OOM-throw.
         {
             size_t remaining = buf.size() - q;
             constexpr uint64_t MIN_BYTES_PER_ENTRY = 15;
             constexpr uint64_t MAX_PLAUSIBLE_NK = 1000000000ULL;
             uint64_t needed = static_cast<uint64_t>(nk) * MIN_BYTES_PER_ENTRY;
             if (nk > MAX_PLAUSIBLE_NK || needed > remaining) {
                 throw std::runtime_error(
                     "Checkpoint corrupt: implausible entry count nk=" +
                     std::to_string(nk) + " (remaining bytes=" +
                     std::to_string(remaining) + ", need >=" +
                     std::to_string(needed) + ")");
             }
         }
         std::vector<std::tuple<std::string, uint64_t, std::string, bool>> ckpt_entries;
         ckpt_entries.reserve(nk);
         for (uint32_t i = 0; i < nk; ++i) {
             if (q + 2 > buf.size()) throw std::runtime_error("Checkpoint corrupt: entry " + std::to_string(i) + " truncated key length");
             uint16_t kl = pu16();
             if (q + kl > buf.size()) throw std::runtime_error("Checkpoint corrupt: entry " + std::to_string(i) + " truncated key data");
             std::string k(reinterpret_cast<char*>(&buf[q]), kl); q += kl;
             if (q + 8 > buf.size()) throw std::runtime_error("Checkpoint corrupt: entry " + std::to_string(i) + " truncated commit_ts");
             uint64_t hc = pu64();
             if (q + 4 > buf.size()) throw std::runtime_error("Checkpoint corrupt: entry " + std::to_string(i) + " truncated value length");
             uint32_t vl = pu32();
             if (q + vl > buf.size()) throw std::runtime_error("Checkpoint corrupt: entry " + std::to_string(i) + " truncated value data");
             std::string v(reinterpret_cast<char*>(&buf[q]), vl); q += vl;
             if (q + 1 > buf.size()) throw std::runtime_error("Checkpoint corrupt: entry " + std::to_string(i) + " truncated deleted flag");
             bool dl = buf[q++] != 0;
             ckpt_entries.emplace_back(k, hc, v, dl);
         }
         for (auto& [k, hc, v, dl] : ckpt_entries) {
             // v25.1 M1.6: ensure_index returns KeyEntry* directly.
             auto [e, dummy] = ensure_index(k);
             Version* h = e->head.load(std::memory_order_acquire);
             uint64_t head_ts = h ? h->commit_ts.load(std::memory_order_acquire) : 0;
             if (hc <= head_ts)
                 throw std::runtime_error("Checkpoint chain corrupt: non-increasing commit_ts for key " + k);
             link_version(e, hc, v, dl);
         }
         return true;
     };
     // Load base checkpoint.
     uint64_t base_cts_for_staleness = 0;  // v20 M3 R-REBASE
     {
         uint64_t base_cts = 0;
         if (parse_and_apply_ckpt(ckpt_path, base_cts)) {
             ckpt_cts = base_cts;
             base_cts_for_staleness = base_cts;
         }
     }
     // Apply delta chain in order.
     {
         int delta_n = 1;
         while (true) {
             std::string delta_path = ckpt_path + ".delta." + std::to_string(delta_n);
             uint64_t delta_cts = 0;
             // v20 M3 R-REBASE: pass base cts so stale deltas left by an
             // interrupted rebase are skipped instead of tripping the
             // non-increasing-commit_ts guard.
             if (!parse_and_apply_ckpt(delta_path, delta_cts, base_cts_for_staleness)) break;
             if (base_cts_for_staleness > 0 && delta_cts <= base_cts_for_staleness) {
                 // v20 M3 R-REBASE: this delta was skipped as stale. Delete it now
                 // that it has been parsed and confirmed stale, so its filename
                 // slot is reclaimed and it does not accumulate forever.
                 std::error_code dec;
                 std::filesystem::remove(delta_path, dec);
             } else if (delta_cts > ckpt_cts) {
                 ckpt_cts = delta_cts;
             }
             delta_n++;
         }
     }
     uint64_t wal_ckpt_ts = 0;
        auto [status, records] = WalSegments::recover_all(wal_dir, wal_ckpt_ts);
        if (status == WalStatus::CORRUPT)
            throw std::runtime_error("WAL CORRUPTION DETECTED");

        // Sort and verify contiguity starting from ckpt_cts + 1
        sort(records.begin(), records.end(),
             [](const auto& a, const auto& b) { return a.first < b.first; });

        uint64_t expected = ckpt_cts + 1;
        uint64_t contiguous = ckpt_cts;
        uint64_t last_replayed = ckpt_cts;
        for (auto& [ts, ws] : records) {
            if (ts <= ckpt_cts) continue;
            assert(ts >= last_replayed && "invariant I3: WAL replay must be non-decreasing");
            last_replayed = ts;      // already in checkpoint
            if (ts < expected)
                throw std::runtime_error("Recovery failed: duplicate commit_ts=" + std::to_string(ts)
                    + " (expected " + std::to_string(expected) + ")"
                    + " — WAL corruption or segment overlap");
            if (ts > expected)
                throw std::runtime_error(std::string("Recovery failed: interior gap")
                    + " expected_ts=" + std::to_string(expected)
                    + " found_ts=" + std::to_string(ts)
                    + " — WAL corruption detected");
            for (auto& [k, v, d] : ws) {
                auto [e, dummy] = ensure_index(k);
                link_version(e, ts, v, d);
            }
            diag::rec_records.fetch_add(1, std::memory_order_relaxed);
            i_rec_records.fetch_add(1, std::memory_order_relaxed);
            contiguous = ts;
            expected++;
        }

        if (contiguous > 0) {
            clock_.store(contiguous + 1, std::memory_order_relaxed);
            pub_.recover_to(contiguous);
            assert(pub_.published() < clock_.load() && "invariant R1: published_ < clock_ after recovery");
        }
    }

    uint64_t index_epoch() const { return 0; }

    // v17 phantom redesign: snapshot-bound existence-transition tracking.
    //
    // v24 Fix 12: register_phantom_reader() is now DEPRECATED on the
    // ChronoKV public surface. The only correct way to enroll a
    // phantom-tracked reader is acquire_slot_with_phantom(), which
    // performs slot acquisition and phantom registration atomically
    // under reader_mu_ (closing the race where prune() could erase
    // transitions in the gap between the two steps). Calling
    // register_phantom_reader() standalone reintroduces that race.
    //
    // deregister_phantom_reader() is NOT deprecated: teardown still
    // needs it (release_slot + deregister_phantom_reader at txn
    // commit/destruction). It does not have the same race because
    // it only removes a reader, never publishes one.
    [[deprecated("v24 Fix 12: use acquire_slot_with_phantom() instead — "
                "calling register_phantom_reader() standalone reintroduces "
                "the phantom-reader registration race")]]
    void register_phantom_reader(uint64_t read_ts) {
        phantom_tracker_.register_reader(read_ts);
    }
    void deregister_phantom_reader(uint64_t read_ts) {
        phantom_tracker_.deregister_reader(read_ts);
    }

    // Retained temporarily for API compatibility; deprecated in v17.
    uint64_t capture_and_register_range() { return 0; }
    void deregister_range_epoch(uint64_t) {}

    std::vector<std::pair<std::string, std::string>> range_scan(uint64_t read_ts,
                                             const std::string& lo,
                                             const std::string& hi) {
#ifdef CHRONOKV_RECORD_HISTORY
        HistScope hs("range_scan", lo + ".." + hi);
        hs.set_lts(read_ts);
#endif
        // v19 M2 FIX: register a reader snapshot to protect versions from
        // GC during the scan. Without this, gc_once() can reclaim a Version
        // node that range_scan is still reading (use-after-free).
        // v19 M2 FIX: register a reader slot for GC protection, but read at
        // the original read_ts (clamped to published). The slot's snapshot is
        // set to pub_.published()+1 by acquire_slot, which protects reads at
        // the current published timestamp. For historical reads (read_ts <
        // published), the slot still prevents GC from running concurrently.
        uint64_t effective_ts = std::min(read_ts, pub_.published());
        SnapshotGuard sg(*this);  // v20.1 (#11): RAII slot, exception-safe
        // The SnapshotGuard publishes a physical-lifetime pin. Phase C
        // reclamation therefore needs no long-held GC/scan lock.

        // v25.1 M1.6 (Phase 2): range_scan uses the tree's range_scan (which
        // wraps Cursor — per-page snapshot via shared latch + HP, M1.3/M1.4).
        // The tree returns (key, encoded_ptr) pairs; we decode the ptr and
        // read the version chain at effective_ts. This replaces the old
        // ordered_idx_.lower_bound + iterate pattern.
        std::vector<std::pair<std::string, std::string>> out;
        auto pairs = tree_scan(lo, hi);
        for (auto& [k, v] : pairs) {
            KeyEntry* e = decode_ptr(v);
            if (!e) continue;
            auto val = read_at_idx(effective_ts, e);
            if (val) out.push_back({k, *val});
        }
#ifdef CHRONOKV_RECORD_HISTORY
        hs.set_result(std::to_string(out.size()));
#endif
        return out;
    }

    // v25.1 M1.6 (Phase 2): range_scan_stream — incremental iteration via Cursor.
    // Returns a pImpl state (defined after BTree) that holds a BTree::Cursor
    // + SnapshotGuard. The caller (Database::RangeScanStream) uses
    // stream_has_next / stream_next / stream_cur_key / stream_cur_val to
    // iterate. Consistency guarantee: per-page snapshot via Cursor's shared
    // latch (M1.4). No cross-page consistency guarantee.
    std::unique_ptr<ChronoKVRangeScanCursorState>
    open_range_scan_stream(const std::string& lo, const std::string& hi);
    static bool stream_has_next(ChronoKVRangeScanCursorState& s);
    static std::pair<std::string, std::string>
    stream_next(ChronoKVRangeScanCursorState& s);

private:
    // v23 Phase C: reader enrollment is linearized under reader_mu_. A reader
    // cannot dereference Version until both its snapshot and its pin are live;
    // reclaim_retired() examines the same pin state under the same mutex.
    //
    // v24 FIX (Group 2, Fix 12 — phantom-reader race, Shape C):
    // The slot-acquisition body is extracted into acquire_slot_locked()
    // so acquire_slot() (read-only SnapshotGuard path) and
    // acquire_slot_with_phantom() (ReadWriteTransaction path) can share
    // the implementation. The phantom variant holds reader_mu_ across
    // BOTH the slot setup AND the phantom_tracker_.register_reader()
    // call, closing the race where prune() ran in the gap between
    // them and erased transitions the about-to-register reader needs.
    int acquire_slot_locked(uint64_t& read_ts) {
        size_t idx;
        if (!reader_free_.empty()) {
            idx = reader_free_.back();
            reader_free_.pop_back();
        } else {
            idx = reader_slots_.size();
            reader_slots_.push_back(std::make_unique<ReaderSlot>());
        }
        ReaderSlot& slot = *reader_slots_[idx];

        uint64_t snapshot;
        do {
            snapshot = pub_.published();
            slot.snapshot.store(snapshot + 1, std::memory_order_seq_cst);
        } while (pub_.published() != snapshot);

        const uint64_t epoch = reclaim_epoch_.load(std::memory_order_acquire);
        if (epoch == 0) {
            slot.snapshot.store(0, std::memory_order_seq_cst);
            reader_free_.push_back(idx);
            throw OrderingViolation("reclaim_epoch_ is zero during reader-slot acquisition");
        }
        slot.pinned_epoch = epoch;
        read_ts = snapshot;
        return static_cast<int>(idx);
    }

public:
    // Read-only path: SnapshotGuard uses this. No phantom registration
    // — a read-only snapshot cannot conflict via SSI phantom detection,
    // so it doesn't need to pin phantom_tracker_.mods_by_ts_.
    int acquire_slot(uint64_t& read_ts) {
        std::lock_guard<std::mutex> lock(reader_mu_);
        return acquire_slot_locked(read_ts);
    }

    // v24 FIX (Group 2, Fix 12 — phantom-reader race, Shape C):
    // Atomic slot acquisition + phantom-reader registration. Used by
    // ReadWriteTransaction's constructor. Holds reader_mu_ across
    // both steps so prune_phantom_tracker() — which also holds
    // reader_mu_ while calling phantom_tracker_.prune() — cannot
    // observe the intermediate state (slot visible, active_snapshots_
    // entry not yet visible) that caused the missed-conflict race.
    //
    // Lock order: reader_mu_ (outer) → phantom_tracker_.mu_ (inner).
    // prune_phantom_tracker() uses the same order; no other call
    // path nests these mutexes, so the order is acyclic.
    //
    // The explicit `*_with_phantom` name (rather than a bool parameter
    // on acquire_slot) makes the read-only vs. phantom-tracked
    // distinction visible at every call site, so a future caller
    // can't accidentally use the untracked variant for a transaction.
    int acquire_slot_with_phantom(uint64_t& read_ts) {
        std::lock_guard<std::mutex> lock(reader_mu_);
        int idx = acquire_slot_locked(read_ts);
        // Register the phantom reader while still holding reader_mu_.
        // register_reader() takes phantom_tracker_.mu_ internally.
        phantom_tracker_.register_reader(read_ts);
        return idx;
    }

private:
    // v24 FIX (Group 2, Fix 12 — phantom-reader race, Shape C):
    // GC's prune entry point. Computes the minimum active reader
    // snapshot from reader_slots_ (under reader_mu_), then calls
    // phantom_tracker_.prune() with that as the external floor —
    // WHILE STILL HOLDING reader_mu_. This is the key invariant:
    // reader_mu_ is held from before the floor computation until
    // after prune()'s erase completes, so acquire_slot_with_phantom()
    // (which also needs reader_mu_) cannot complete in between,
    // publishing a slot without a corresponding active_snapshots_
    // entry.
    //
    // v24 audit (post-fix): the PhantomTracker was the ONLY prune-shaped
    // operation in the engine that consulted a separate "active reader"
    // set with a non-atomic registration step. The other prune-shaped
    // operations are NOT affected:
    //   - gc_threshold() already walks reader_slots_ under reader_mu_.
    //   - min_active_pin_epoch() / reclaim_retired() rely on the
    //     epoch-pin protocol (v23 E1-E16): a reader's pinned_epoch is
    //     set INSIDE acquire_slot_locked() under reader_mu_, with no
    //     separate registration step, so there is no gap to close.
    // No sibling bug exists. This audit comment exists to spare the
    // next reviewer from re-walking the same paths.
    void prune_phantom_tracker() {
        std::lock_guard<std::mutex> lk(reader_mu_);
        uint64_t floor = UINT64_MAX;
        for (auto& s : reader_slots_) {
            uint64_t v = s->snapshot.load(std::memory_order_seq_cst);
            if (v != 0) {
                uint64_t ts = v - 1;  // slot stores read_ts + 1
                if (ts < floor) floor = ts;
            }
        }
        // Lock order: reader_mu_ (held above) → phantom_tracker_.mu_
        // (taken inside prune). Consistent with acquire_slot_with_phantom().
        phantom_tracker_.prune(floor);
    }

public:

    void release_slot(int slot_index) {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(reader_mu_);
            ReaderSlot& slot = *reader_slots_[slot_index];
            // Callers perform no Version dereference after release_slot().
            slot.pinned_epoch = 0;
            slot.snapshot.store(0, std::memory_order_seq_cst);
            reader_free_.push_back(static_cast<size_t>(slot_index));
            wake = retired_pending_.load(std::memory_order_relaxed) != 0;
        }
        // Without this notification, a blocked retired suffix could remain
        // queued forever when no subsequent write marks the GC dirty.
        if (wake) wake_gc();
    }

#ifdef CHRONOKV_TEST_HOOKS
public:
    void test_set_reclaim_epoch(uint64_t e) {
        reclaim_epoch_.store(e, std::memory_order_seq_cst);
    }
    void test_acquire_and_release_slot() {
        uint64_t ignored = 0;
        const int slot = acquire_slot(ignored);
        release_slot(slot);
    }
    std::optional<std::string> test_read_at(uint64_t read_ts, const std::string& key) const {
        return read_at(read_ts, key);
    }

    // v24 Fix 12 regression test hooks.
    //
    // test_acquire_slot_no_phantom(): acquires a reader slot WITHOUT
    // registering a phantom reader. Simulates the pre-fix constructor's
    // intermediate state — slot visible in reader_slots_, but no entry
    // yet in active_snapshots_. With the fix, prune_phantom_tracker()
    // consults reader_slots_ directly, so this state still protects
    // phantom transitions.
    //
    // Returns the slot index (caller must release_slot() it later).
    // Out-param read_ts is the snapshot the slot was acquired at.
    int test_acquire_slot_no_phantom(uint64_t& read_ts) {
        return acquire_slot(read_ts);  // slot only, no phantom registration
    }

    // Direct prune entry point — same path gc_once() uses.
    void test_force_prune_phantom() {
        prune_phantom_tracker();
    }

    // Count phantom-tracker transitions with commit_ts strictly
    // greater than `ts`. Used to assert that transitions a live
    // reader (at snapshot `ts`) needs have survived prune().
    size_t test_phantom_mods_count_above(uint64_t ts) {
        return phantom_tracker_.test_mods_count_above(ts);
    }

    // Direct phantom-range query — used to assert that a reader at
    // snapshot `read_ts` would detect a phantom in [lo, hi].
    bool test_has_phantom_in_range(const std::string& lo,
                                    const std::string& hi,
                                    uint64_t read_ts) {
        return phantom_tracker_.has_phantom_in_range(lo, hi, read_ts);
    }

/*
 * =================================================================
 * CHRONOKV_PHASE_D_TESTS
 *
 * Phase-D adversarial reclamation tests.
 *
 * These helpers are intended for the ChronoKV test-hook build.
 *
 * They exercise:
 *
 *   E9-E14:
 *       a reader holding a live Version* while GC retirement and
 *       reclamation activity occurs underneath it.
 *
 *   E16:
 *       a severed Version must no longer be reachable from the
 *       currently published head.
 * =================================================================
 */

bool test_phase_d_stalled_reader() {

    const std::string key =
        "__phase_d_stalled_reader__";

    /*
     * Build a real version chain.
     */
    for (int i = 0; i < 8; ++i) {

        if (commit(
                key,
                "phaseD-" + std::to_string(i)
            ) != TxnResult::Committed) {

            return false;
        }
    }

    std::mutex mu;
    std::condition_variable cv;

    bool reader_ready = false;
    bool allow_reader = false;
    bool reader_finished = false;

    bool reader_ok = false;

    std::string observed;

    std::exception_ptr reader_error;

    std::thread reader([&] {

        int slot = -1;

        try {

            uint64_t snapshot_ts = 0;

            /*
             * Acquire the same production snapshot slot/pin
             * mechanism used by readers.
             */
            slot = acquire_slot(snapshot_ts);

            const KeyEntry* entry =
                find_index(key);

            if (entry == nullptr) {

                throw std::runtime_error(
                    "Phase D: key not found"
                );
            }

            // v25.1 M1.6: find_index returns KeyEntry* directly (was int idx).
            // No nm_ lock needed — KeyEntry* is stable.

            Version* head =
                entry->head.load(
                    std::memory_order_acquire
                );

            if (head == nullptr) {

                throw std::runtime_error(
                    "Phase D: published head is null"
                );
            }

            /*
             * Capture a historical node while the epoch pin is held.
             */
            Version* retained =
                head->prev.load(
                    std::memory_order_acquire
                );

            if (retained == nullptr) {

                throw std::runtime_error(
                    "Phase D: historical version is null"
                );
            }

            /*
             * The reader is now intentionally stalled while still
             * holding the production epoch pin.
             */
            {
                std::lock_guard<std::mutex> lk(mu);

                reader_ready = true;
            }

            cv.notify_all();

            {
                std::unique_lock<std::mutex> lk(mu);

                cv.wait(
                    lk,
                    [&] {
                        return allow_reader;
                    }
                );
            }

            /*
             * This is the critical lifetime probe.
             *
             * GC may have retired/severed the node, but it must not
             * have destroyed it while this reader's pin is active.
             */
            const uint64_t ts =
                retained->commit_ts.load(
                    std::memory_order_acquire
                );

            const std::string value =
                retained->value;

            observed = value;

            reader_ok =ts != 0 &&
                value.rfind(
                    "phaseD-",
                    0
                ) == 0;

        }
        catch (...) {

            reader_error =std::current_exception();

            {
                std::lock_guard<std::mutex> lk(mu);

                reader_ready = true;
            }

            cv.notify_all();
        }

        if (slot >= 0) {

            release_slot(slot);
        }

        {
            std::lock_guard<std::mutex> lk(mu);

            reader_finished = true;
        }

        cv.notify_all();
    });

    /*
     * Wait until the reader has captured the live pointer.
     */
    {
        std::unique_lock<std::mutex> lk(mu);

        cv.wait(
            lk,
            [&] {
                return reader_ready;
            }
        );
    }

    /*
     * Run the real GC path while the reader remains pinned.
     */
    gc_pass_for_test();

    const auto stats_while_pinned =
        gc_stats();

    /*
     * Resume the reader.
     */
    {
        std::lock_guard<std::mutex> lk(mu);

        allow_reader = true;
    }

    cv.notify_all();

    {
        std::unique_lock<std::mutex> lk(mu);

        cv.wait(
            lk,
            [&] {
                return reader_finished;
            }
        );
    }

    reader.join();

    /*
     * After the reader releases its pin, normal reclamation should
     * eventually be able to drain the retired list.
     */
    for (int i = 0; i < 16; ++i) {

        gc_pass_for_test();

        if (
            retired_pending_.load(
                std::memory_order_acquire
            ) == 0
        ) {
            break;
        }
    }

    const bool drained =
        retired_pending_.load(
            std::memory_order_acquire
        ) == 0;

    const bool reclaim_was_blocked =
        stats_while_pinned.retired_pending != 0;

    return
        reader_error == nullptr &&
        reclaim_was_blocked &&
        reader_ok &&
        !observed.empty() &&
        drained;
}


bool test_phase_d_e16_reachability() {

    const std::string key =
        "__phase_d_e16__";

    /*
     * Build a chain containing historical versions.
     */
    for (int i = 0; i < 8; ++i) {

        if (commit(
                key,
                "phaseD-e16-" + std::to_string(i)
            ) != TxnResult::Committed) {

            return false;
        }
    }

    /*
     * Keep reclamation from destroying the target while we inspect
     * the reachability invariant.
     */
    SnapshotGuard pin(*this);

    // v25.1 M1.6: find_index returns KeyEntry* directly (was int idx).
    KeyEntry* entry = find_index(key);

    if (entry == nullptr) {
        return false;
    }

    Version* published_head =
        entry->head.load(
            std::memory_order_acquire
        );

    if (published_head == nullptr) {
        return false;
    }

    Version* target =
        published_head->prev.load(
            std::memory_order_acquire
        );

    if (target == nullptr) {
        return false;
    }

    /*
     * Run the real retirement/severing path.
     */
    gc_pass_for_test();

    /*
     * Verify that the target was actually retired.
     */
    bool was_retired = false;

    {
        std::lock_guard<std::mutex> lk(retired_mu_);

        for (const auto& item : retired_nodes_) {

            if (!item) {
                continue;
            }

            if (item->node.get() == target) {

                was_retired = true;
                break;
            }
        }
    }

    if (!was_retired) {
        return false;
    }

    /*
     * E16:
     *
     * Starting from the currently published head, the exact
     * severed node must no longer be reachable.
     */
    bool reachable = false;

    Version* current =
        entry->head.load(
            std::memory_order_acquire
        );

    size_t guard = 0;

    while (
        current != nullptr &&
        guard++ < 100000
    ) {

        if (current == target) {

            reachable = true;
            break;
        }

        current =current->prev.load(
                std::memory_order_acquire
            );
    }

    if (reachable) {
        return false;
    }

    /*
     * The SnapshotGuard releases its pin on return.
     */
    return true;
}


private:
#endif

    // v25.1 M1.6: read_at_idx takes KeyEntry* directly (was int idx).
    // No nm_ lock — the KeyEntry* is stable (heap-allocated, engine-owned,
    // deferred reclamation via retire() under epoch-pinned GC, same as
    // Version nodes). The version chain walk is protected by the caller's
    // SnapshotGuard pin (same as before).
    std::optional<std::string> read_at_idx(uint64_t read_ts, KeyEntry* ent) const {
        if (!ent) return std::nullopt;
        Version* cur = ent->head.load(std::memory_order_acquire);
        while (cur) {
            uint64_t c = cur->commit_ts.load(std::memory_order_acquire);
            if (c != 0 && c <= read_ts) {
                if (cur->deleted) return std::nullopt;
                return cur->value;
            }
            cur = cur->prev.load(std::memory_order_acquire);
        }
        return std::nullopt;
    }

    std::optional<std::string> read_at(uint64_t read_ts, const std::string& key) const {
        return read_at_idx(read_ts, find_index(key));
    }

public:
    std::optional<std::string> read(const std::string& key) {
#ifdef CHRONOKV_RECORD_HISTORY
        HistScope hs("read", key);
#endif
        SnapshotGuard sg(*this);
        uint64_t r = sg.read_ts();
#ifdef CHRONOKV_RECORD_HISTORY
        hs.set_lts(r);
#endif
        auto res = read_at(r, key);
#ifdef CHRONOKV_RECORD_HISTORY
        hs.set_result(res ? *res : "<absent>");
#endif
        return res;
    }
};

// v25.1 M0.6: static member definitions for the instance registry.
// These live here (after the ChronoKV class definition) so the type is complete.
std::mutex ChronoKV::instance_registry_mu_;
ChronoKV::InstanceNode* ChronoKV::instance_registry_head_ = nullptr;

// ======================== RAII SnapshotGuard (out-of-line definitions) =======
// The class itself is declared before ChronoKV (so ChronoKV's read paths can
// use it); these definitions require ChronoKV to be complete because they call
// acquire_slot/release_slot (v20.1 #11).

// v24 M-cleanup: missing 'inline' meant any consumer including this
// header in more than one translation unit got duplicate-symbol link
// errors -- a real multi-TU ODR violation, undetected by this project's
// own single-TU test binary. Added.
inline SnapshotGuard::SnapshotGuard(ChronoKV& kv) : kv_(kv) {
    slot_ = kv_.acquire_slot(read_ts_);
}
inline SnapshotGuard::~SnapshotGuard() { kv_.release_slot(slot_); }

// ======================== ReadWriteTransaction ========================

class ReadWriteTransaction {
    ChronoKV& kv_;
    uint64_t read_ts_;
    int slot_;
    std::map<std::string, std::pair<std::string, bool>> ws_;
    std::set<std::string> rs_;
    std::vector<RangeRead> range_reads_;
    TxnState state_ = TxnState::Active;
    bool slot_released_ = false;
    bool phantom_registered_ = true;
    // v24 fix: when created via chronokv::Transaction (public API),
    // db_alive_ is a weak_ptr into Database::alive_ and guarded_ is
    // true.  The destructor skips kv_ cleanup if the Database was
    // closed or destroyed.  When created directly (internal engine
    // tests), guarded_ is false and cleanup always runs.
    std::weak_ptr<bool> db_alive_;
    bool guarded_ = false;

    // Returns true if the owning Database is still open and the engine
    // referenced by kv_ is still alive.  Only meaningful when guarded_
    // is true (public API path).  When guarded_ is false, always returns
    // true — the caller has guaranteed engine lifetime.
    bool engine_live() const {
        if (!guarded_) return true;
        if (auto sp = db_alive_.lock()) return *sp;
        return false;
    }

public:
    // Used by chronokv::Transaction (public API) — liveness-guarded.
    // db_alive is a weak_ptr into Database::alive_.  The destructor
    // skips kv_ cleanup if the Database was closed or destroyed.
    //
    // v24 FIX (Group 2, Fix 12 — phantom-reader race, Shape C):
    // Constructor now makes a single atomic call to
    // acquire_slot_with_phantom(), which holds reader_mu_ across both
    // the slot acquisition AND the phantom-reader registration. This
    // closes the race where GC's prune() ran between the two steps
    // (slot visible in reader_slots_, but no entry yet in
    // active_snapshots_) and erased phantom-tracker transitions this
    // reader needs. See acquire_slot_with_phantom() for the lock
    // ordering argument.
    ReadWriteTransaction(ChronoKV& kv, std::weak_ptr<bool> db_alive)
        : kv_(kv), db_alive_(std::move(db_alive)), guarded_(true) {
        slot_ = kv_.acquire_slot_with_phantom(read_ts_);
    }

    // Test-only / internal constructor that does NOT guard against
    // Database lifetime.  The caller must guarantee that the supplied
    // ChronoKV& outlives this transaction.  Gated behind
    // CHRONOKV_TEST_HOOKS because this constructor intentionally
    // bypasses the v24 liveness protection.
#ifdef CHRONOKV_TEST_HOOKS
    explicit ReadWriteTransaction(ChronoKV& kv) : kv_(kv) {
        slot_ = kv_.acquire_slot_with_phantom(read_ts_);
    }
#endif

    ~ReadWriteTransaction() {
        if (engine_live()) {
            if (!slot_released_) kv_.release_slot(slot_);
            if (phantom_registered_) kv_.deregister_phantom_reader(read_ts_);
        }
    }

    ReadWriteTransaction(const ReadWriteTransaction&) = delete;
    ReadWriteTransaction& operator=(const ReadWriteTransaction&) = delete;

    TxnState state() const { return state_; }

    std::optional<std::string> read(const std::string& k) {
        if (state_ != TxnState::Active) return std::nullopt;
        auto it = ws_.find(k);
        if (it != ws_.end())
            return it->second.second ? std::nullopt : std::optional<std::string>(it->second.first);
        rs_.insert(k);
        return kv_.read_at(read_ts_, k);
    }

    void write(const std::string& k, const std::string& v) {
        if (state_ != TxnState::Active) return;
        ws_[k] = {v, false};
    }

    void del(const std::string& k) {
        if (state_ != TxnState::Active) return;
        ws_[k] = {"", true};
    }

    std::vector<std::pair<std::string, std::string>> range_scan(const std::string& lo, const std::string& hi) {
        if (state_ != TxnState::Active) return {};
        auto result = kv_.range_scan(read_ts_, lo, hi);
        range_reads_.push_back({lo, hi, read_ts_, 0});
        // v22 M2 (invariant T1, read-your-writes): overlay this transaction's
        // buffered write-set onto the snapshot scan. Buffered deletes suppress
        // keys; buffered writes insert/override. Result stays key-sorted. The
        // overlay is read-side only; phantom detection (range_reads_ registered
        // above) is unchanged, so SSI validation is unaffected.
        std::map<std::string, std::string> merged;
        for (auto& [mk, mv] : result) merged[mk] = mv;
        for (auto wit = ws_.lower_bound(lo);
             wit != ws_.end() && wit->first <= hi; ++wit) {
            if (wit->second.second) merged.erase(wit->first);
            else merged[wit->first] = wit->second.first;
        }
        result.clear();
        result.reserve(merged.size());
        for (auto& [mk, mv] : merged) result.emplace_back(mk, mv);
        return result;
    }

    TxnResult commit() {
        if (state_ != TxnState::Active) return TxnResult::InvalidState;
#ifdef CHRONOKV_RECORD_HISTORY
        std::string ks;
        for (auto& [k, vd] : ws_) { if (!ks.empty()) ks += ","; ks += k; }
        HistScope hs("txn_commit", ks);
        hs.set_lts(read_ts_);
#endif
        WriteSet ws;
        for (auto& [k, vd] : ws_) ws.push_back({k, vd.first, vd.second});
        uint64_t cts = 0;
        TxnResult r = kv_.commit_txn(read_ts_, ws, rs_, range_reads_, &cts);
        state_ = (r == TxnResult::Committed) ? TxnState::Committed : TxnState::Aborted;
        kv_.release_slot(slot_);
        slot_released_ = true;
        if (phantom_registered_) {
            kv_.deregister_phantom_reader(read_ts_);
            phantom_registered_ = false;
        }
#ifdef CHRONOKV_RECORD_HISTORY
        hs.set_result(to_string(r));
#endif
        return r;
    }
};

// ======================== Serial oracle ========================

class SerialOracle {
    std::map<std::string, std::string> state_;
public:
    void apply(const WriteSet& ws) {
        for (auto& [k, v, d] : ws) {
            if (d) state_.erase(k);
            else state_[k] = v;
        }
    }

    std::optional<std::string> read(const std::string& k) const {
        auto it = state_.find(k);
        if (it == state_.end()) return std::nullopt;
        return it->second;
    }

    std::vector<std::pair<std::string, std::string>> range_scan(const std::string& lo, const std::string& hi) const {
        std::vector<std::pair<std::string, std::string>> out;
        auto it = state_.lower_bound(lo);
        while (it != state_.end() && it->first <= hi) {
            out.push_back(*it);
            ++it;
        }
        return out;
    }

    size_t size() const { return state_.size(); }
};

// ======================== Tests ========================


// ================================================================
// [SECTION_8_TEST_SUITE]
// ================================================================

// ======================== M6 benchmark harness (CHRONOKV_BENCH) ========================
#ifdef CHRONOKV_BENCH
static void run_bench() {
    const int NWARMUP = 5000;
    const int NWRITE = 200000;
    const int NREAD = 2000000;
    auto now = [] { return std::chrono::steady_clock::now(); };
    std::vector<std::string> keys;
    keys.reserve(NWRITE);
    for (int i = 0; i < NWRITE; ++i) keys.push_back("k" + std::to_string(i));
    ChronoKV kv;  // no WAL: isolates in-memory MVCC + per-instance counter overhead
    for (int i = 0; i < NWARMUP; ++i) kv.commit("w" + std::to_string(i), "x");
    auto t0 = now();
    for (int i = 0; i < NWRITE; ++i) kv.commit(keys[i], "value_" + std::to_string(i));
    auto t1 = now();
    double sec = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e9;
    std::cout << "BENCH write (no-WAL): " << (long long)(NWRITE / sec) << " commits/sec\n";
    volatile long sink = 0;
    t0 = now();
    for (int i = 0; i < NREAD; ++i) {
        auto v = kv.read(keys[i % NWRITE]);
        if (v) sink++;
    }
    t1 = now();
    sec = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e9;
    std::cout << "BENCH read: " << (long long)(NREAD / sec) << " reads/sec\n";
}
#endif


// =====================================================================
// v21 M1: Public C++ API
// =====================================================================

namespace chronokv {

// ---- Version ------------------------------------------------------------
// v21 M4: public API version constant. Bump on any breaking API change.
// NOTE (v24 M-cleanup): this constant was found stale during v24 review --
// it still said 0.22.2 despite v23 (epoch-pinned reclamation, Phases A-D)
// being fully shipped and verified under all four sanitizer configs. Bumped
// to reflect the actual shipped major version. Keep this constant in sync
// going forward -- it is the single source of truth other comments in this
// file should be checked against, not the other way around.
//
// v24 (closure/verification release) bumps to 0.24.0: the twelve v24 fixes
// (WAL durability, GC/TOCTOU, transaction move semantics, phantom-reader
// race closure, orphaned-.tmp directory-scan cleanup, etc.) are all
// applied and clean under Release / ASan+UBSan / TSan / Stress; the
// hooks-off public_api_smoke.cpp target (28 checks) passes against the
// same header.
static constexpr const char* CHRONOKV_VERSION = "0.25.2";
static constexpr int CHRONOKV_VERSION_MAJOR = 0;
static constexpr int CHRONOKV_VERSION_MINOR = 25;
static constexpr int CHRONOKV_VERSION_PATCH = 2;

// ---- Error hierarchy --------------------------------------------------
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CorruptionError : public Error {
public:
    using Error::Error;
};

class LifecycleError : public Error {
public:
    using Error::Error;
};

// Thrown by language bindings (e.g. the Python module) that choose to
// surface Status::Conflict as an exception rather than a return value.
// Not thrown by the C++ API itself — Database::put/erase and
// Transaction::commit() return Status::Conflict, they do not throw.
class ConflictError : public Error {
public:
    using Error::Error;
};

// v25.1 M1.5: thrown by API stubs that are declared but not yet
// implemented (e.g., range_scan_stream, which requires M1.6 B+ tree
// integration for real incremental iteration). This is a deliberate,
// tracked stub — not a permanent limitation.
class NotYetImplementedError : public Error {
public:
    using Error::Error;
};

// ---- Transactional outcomes (not exceptions) --------------------------
enum class Status : uint8_t {
    OK,
    Conflict,
    TooLarge,
    InvalidTransaction,
    InvalidState,
    WalFailure,
    Failed
};

// ---- Result<T, Status> (M1.5) ------------------------------------------
//
// The type returned by async operations. Replaces throwing for
// recoverable error conditions (Conflict, TooLarge, WalFailure,
// Failed). Exceptions remain for unrecoverable conditions
// (CorruptionError, LifecycleError, NotYetImplementedError).
//
// Usage:
//   auto r = co_await db.put_async("k", "v");
//   if (!r.ok()) { /* handle r.status */ }
//   // or: auto val = *r;  (undefined if !r.ok())
template<typename T>
struct Result {
    Status status = Status::OK;
    std::optional<T> value;

    bool ok() const { return status == Status::OK; }
    T& operator*() { return *value; }
    const T& operator*() const { return *value; }
    T* operator->() { return &*value; }
    const T* operator->() const { return &*value; }

    static Result success(T v) {
        return {Status::OK, std::move(v)};
    }
    static Result failure(Status s) {
        return {s, std::nullopt};
    }
};

// Re-export the engine's DurabilityMode so public API callers don't need
// to reference the engine's internal namespace.
using DurabilityMode = ::DurabilityMode;

// ---- Configuration ----------------------------------------------------
// durability modes:
//   Group (default): batched fsync, durable against power loss, amortizes
//     fsync cost across concurrent writers.
//   Sync: conservative per-commit fsync, same durability guarantee.
//   Async: durable against process crash only (not power loss).
struct Options {
    std::string wal_dir;          // "" = in-memory only (no durability)
    std::string checkpoint_path;  // "" = no checkpointing
    bool auto_start_gc = true;
    bool recover_on_open = true;
    DurabilityMode durability = DurabilityMode::Group;
    // v25.1 M1.6: page pool capacity for the B+ tree index. Default 256 MiB
    // (production-sized). Tests can set this lower (e.g., 16 MiB) to avoid
    // virtual memory pressure on memory-constrained VMs — the 2-CPU test
    // VM (4 GB RAM, no swap) hits EAGAIN from pthread_create when many
    // 256 MiB pools are allocated in sequence (76 Database::open calls in
    // the full test suite), even though each is freed before the next.
    size_t page_pool_bytes = 256ULL * 1024 * 1024;
};

// ---- Forward declarations ---------------------------------------------
class Transaction;

// ---- Diagnostics value types (v22 M3) ----------------------------------
// Read-only snapshots mirroring the engine's per-instance counters.
struct WalStats {
    uint64_t batches = 0, records = 0, bytes = 0, fsyncs = 0,
             write_fails = 0, fsync_fails = 0, truncations = 0,
             truncate_fails = 0, async_lost = 0, pub_allocated_max = 0;
};
struct GcStats {
    uint64_t created = 0, retired = 0, reclaimed = 0, retired_pending = 0,
             reclaim_epoch = 0, oldest_active_pin_epoch = 0, passes = 0,
             last_keys = 0, last_steps = 0, last_pass_ns = 0,
             max_pass_ns = 0;
};
struct EpochStats {
    uint64_t entries = 0, oldest = 0, newest = 0;
};
struct Health {
    int level = 0;                       // 0 healthy, 1 degraded, 2 failing
    std::vector<std::string> reasons;
};
// ---- Database ---------------------------------------------------------
class Database {
    std::unique_ptr<ChronoKV> engine_;
    std::string checkpoint_path_;
    bool closed_ = false;
    // v24 fix (was Kimi review 1.4 partial mitigation, now complete):
    // a Transaction holds a raw ChronoKV& internally. This shared flag
    // lets Transaction::check_active() detect "my Database is closed or
    // destroyed" and throw LifecycleError instead of touching a dangling
    // reference. The fix is complete: both explicit API methods AND the
    // destructor cleanup path (release_slot / deregister_phantom_reader)
    // check this flag via a weak_ptr held by ReadWriteTransaction.
    //
    // Concurrency note: this is a plain bool, not std::atomic<bool>.
    // Concurrent Database::close() (which writes *alive_ = false) with
    // Transaction method calls on another thread is NOT a supported
    // concurrency pattern.  Making the bool atomic would prevent a
    // theoretical data race on the flag itself, but would not make
    // close() thread-safe: there would still be a TOCTOU window
    // between Transaction::check_active() passing and the subsequent
    // engine_->... call seeing a destructed engine.  Callers that
    // need concurrent shutdown must provide their own external
    // synchronisation (e.g. join all transaction-holding threads
    // before calling close()).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // Private constructor (used by open factory)
    Database(std::unique_ptr<ChronoKV> eng, std::string cp)
        : engine_(std::move(eng)), checkpoint_path_(std::move(cp)) {}

    void check_open() const {
        if (closed_) throw LifecycleError("database is closed");
    }

public:
    // Factory: constructs engine, recovers (if configured), starts GC
    static Database open(const Options& opts) {
        std::unique_ptr<ChronoKV> engine;
        try {
            engine = std::make_unique<ChronoKV>(opts.wal_dir, opts.page_pool_bytes);
        } catch (const std::exception& e) {
            throw LifecycleError(std::string("cannot open database: ") + e.what());
        }
        
        if (opts.recover_on_open && (!opts.wal_dir.empty() || !opts.checkpoint_path.empty())) {
            try {
                engine->recover_with_checkpoint(opts.wal_dir, opts.checkpoint_path);
            } catch (const std::exception& e) {
                throw CorruptionError(std::string("recovery failed: ") + e.what());
            }
        }
        
        if (opts.auto_start_gc) {
            engine->start_gc();
        }
        
        engine->set_durability(opts.durability);
        
        return Database(std::move(engine), opts.checkpoint_path);
    }

    ~Database() {
        if (!closed_) close();
    }

    // Move-only.  Explicit move operations transfer closed_ from
    // the source so that move-from-closed or move-from-moved-from
    // correctly leaves the destination closed (engine_==nullptr,
    // alive_==nullptr).  Without this, the destination would get
    // closed_==false with null members, making is_open() lie and
    // causing null dereferences in close()/get()/etc.
    Database(Database&& src) noexcept
        : engine_(std::move(src.engine_)),
          checkpoint_path_(std::move(src.checkpoint_path_)),
          closed_(src.closed_),
          alive_(std::move(src.alive_)) {
        src.closed_ = true;  // prevent src.~Database() from re-closing
    }
    Database& operator=(Database&& src) noexcept {
        if (this != &src) {
            if (!closed_) close();           // clean up current
            engine_ = std::move(src.engine_);
            checkpoint_path_ = std::move(src.checkpoint_path_);
            alive_ = std::move(src.alive_);
            closed_ = src.closed_;           // transfer closed state
            src.closed_ = true;              // prevent src.~Database() from re-closing
        }
        return *this;
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Simple single-key operations (blind writes).
    // v25.1 M1.5 design decision (B): sync methods call the engine DIRECTLY,
    // not via std::async. The original plan called for sync to be one-line
    // wrappers over the async path (.get()), but that was wrong once
    // thread-spawn cost is accounted for: std::async(std::launch::async)
    // spawns a thread per call (~10-12us pthread_create), which exceeds the
    // ~8us cost of an in-memory commit_txn. Sync-via-async would regress
    // sync latency vs v24 for no benefit. The async methods (put_async,
    // get_async, erase_async) are therefore SEPARATE entry points for callers
    // who want concurrency, not the implementation basis for sync. This is a
    // corrected design decision documented in the M1.5 milestone report.
    std::optional<std::string> get(std::string_view key) {
        check_open();
        try {
            return engine_->read(std::string(key));
        } catch (const std::exception& e) {
            throw Error(std::string("get failed: ") + e.what());
        }
    }

    Status put(std::string_view key, std::string_view value) {
        check_open();
        try {
            WriteSet ws = {{std::string(key), std::string(value), false}};
            uint64_t cts = 0;
            auto r = engine_->commit_txn(UINT64_MAX, ws, {}, {}, &cts);
            auto status = map_txn_result(r);
            if (status == Status::OK) notify_observers(ws);
            return status;
        } catch (const std::exception& e) {
            throw Error(std::string("put failed: ") + e.what());
        }
    }

    Status erase(std::string_view key) {
        check_open();
        try {
            WriteSet ws = {{std::string(key), "", true}};
            uint64_t cts = 0;
            auto r = engine_->commit_txn(UINT64_MAX, ws, {}, {}, &cts);
            auto status = map_txn_result(r);
            if (status == Status::OK) notify_observers(ws);
            return status;
        } catch (const std::exception& e) {
            throw Error(std::string("erase failed: ") + e.what());
        }
    }

    // Range scan (inclusive [lo, hi]) at the latest committed state. This
    // provides a snapshot-consistent scan within one call (pinned while it
    // runs) but NO isolation across separate calls — use Transaction for
    // multi-read atomicity.
    std::vector<std::pair<std::string, std::string>>
    range_scan(std::string_view lo, std::string_view hi) {
        check_open();
        try {
            return engine_->range_scan(UINT64_MAX, std::string(lo), std::string(hi));
        } catch (const std::exception& e) {
            throw Error(std::string("range_scan failed: ") + e.what());
        }
    }

    // Start an SSI transaction
    Transaction begin();

    void checkpoint() {
        check_open();
        if (checkpoint_path_.empty())
            throw LifecycleError("checkpoint() called but no checkpoint_path was configured");
        try {
            engine_->checkpoint(checkpoint_path_);
        } catch (const std::exception& e) {
            throw Error(std::string("checkpoint failed: ") + e.what());
        }
    }

    void close() {
        if (!closed_) {
            closed_ = true;
            *alive_ = false;  // signal any outstanding Transaction handles
            engine_.reset();  // destruct the engine
        }
    }

    bool is_open() const { return !closed_; }

    // v22 M3: diagnostics - read-only snapshots forwarded from the engine.
    WalStats wal_stats() const {
        check_open();
        auto w = engine_->wal_stats();
        return WalStats{w.batches, w.records, w.bytes, w.fsyncs,
                        w.write_fails, w.fsync_fails, w.truncations,
                        w.truncate_fails, w.async_lost, w.pub_allocated_max};
    }
    GcStats gc_stats() const {
        check_open();
        auto g = engine_->gc_stats();
        return GcStats{g.created, g.retired, g.reclaimed, g.retired_pending,
                       g.reclaim_epoch, g.oldest_active_pin_epoch, g.passes,
                       g.last_keys, g.last_steps, g.last_pass_ns,
                       g.max_pass_ns};
    }
    EpochStats epoch_stats() const {
        check_open();
        auto e = engine_->epoch_stats();
        return EpochStats{e.entries, e.oldest, e.newest};
    }
    uint64_t published_watermark() const {
        check_open();
        return engine_->published_watermark();
    }
    // v25.1 M1.6: debug counter for fence re-check retries (test use).
    // Defined out-of-line (after BTree) — see below.
    uint64_t fence_recheck_retries() const;
    // v25.1 M1.6: debug counter for ensure_index loser-delete branch (test use).
    uint64_t ensure_index_loser_deletes() const;
    // v25.1 M2 Phase 2: test-only io_uring mock injection.
    void inject_iouring_for_test(std::unique_ptr<chronokv_iouring::IoUring> mock) {
        check_open();
        engine_->wal_->inject_iouring_for_test(std::move(mock));
    }
    bool last_batch_used_iouring() const {
        check_open();
        return engine_->wal_->last_batch_used_iouring_.load();
    }
    Health health() const {
        check_open();
        auto hh = engine_->health();
        return Health{hh.level, hh.reasons};
    }

    // ======================== M1.5: Async API ========================

    // Async put: returns a future that resolves to the commit Status.
    std::future<Status> put_async(std::string key, std::string value) {
        check_open();
        return std::async(std::launch::async, [this, key = std::move(key), value = std::move(value)]() {
            WriteSet ws = {{key, value, false}};
            uint64_t cts = 0;
            auto r = engine_->commit_txn(UINT64_MAX, ws, {}, {}, &cts);
            auto status = map_txn_result(r);
            if (status == Status::OK) {
                notify_observers(ws);
            }
            return status;
        });
    }

    // Async get: returns a future that resolves to Result<std::optional<std::string>>.
    std::future<Result<std::optional<std::string>>> get_async(std::string key) {
        check_open();
        return std::async(std::launch::async, [this, key = std::move(key)]() -> Result<std::optional<std::string>> {
            try {
                return Result<std::optional<std::string>>::success(engine_->read(key));
            } catch (...) {
                return Result<std::optional<std::string>>::failure(Status::Failed);
            }
        });
    }

    // Async erase: returns a future that resolves to the commit Status.
    std::future<Status> erase_async(std::string key) {
        check_open();
        return std::async(std::launch::async, [this, key = std::move(key)]() {
            WriteSet ws = {{key, "", true}};
            uint64_t cts = 0;
            auto r = engine_->commit_txn(UINT64_MAX, ws, {}, {}, &cts);
            auto status = map_txn_result(r);
            if (status == Status::OK) {
                notify_observers(ws);
            }
            return status;
        });
    }

    // ======================== M1.5: Batch ========================
    //
    // First-class object for atomic write-set commits without isolation.
    // Cheaper than Transaction (no reader slot, no phantom registration).
    // All keys in the batch are committed atomically in a single
    // commit_txn call.

    class Batch {
    public:
        Batch(Database& db) : db_(db) {}

        void put(std::string key, std::string value) {
            entries_.push_back({std::move(key), std::move(value), false});
        }

        void erase(std::string key) {
            entries_.push_back({std::move(key), "", true});
        }

        Status commit() {
            db_.check_open();
            if (entries_.empty()) return Status::OK;
            WriteSet ws;
            ws.reserve(entries_.size());
            for (auto& e : entries_) {
                ws.push_back({e.key, e.value, e.deleted});
            }
            uint64_t cts = 0;
            auto r = db_.engine_->commit_txn(UINT64_MAX, ws, {}, {}, &cts);
            entries_.clear();
            auto status = db_.map_txn_result(r);
            // v25.1 M1.5 fix D: Batch::commit must notify observers on
            // success, exactly as put()/erase()/put_async()/erase_async()
            // do (see Database::put at line ~4954). Previously Batch
            // silently skipped observer notification — observers never
            // saw batch writes. This was a correctness gap caught during
            // M1.5 state verification.
            if (status == Status::OK) db_.notify_observers(ws);
            return status;
        }

        size_t size() const { return entries_.size(); }
        void clear() { entries_.clear(); }

    private:
        struct Entry { std::string key, value; bool deleted; };
        Database& db_;
        std::vector<Entry> entries_;
    };

    Batch create_batch() { return Batch(*this); }

    // ======================== M1.5: Observers ========================
    //
    // RAII-registered per key prefix. on_change(key, old_value, new_value)
    // fires when a matching key is written via put/erase/put_async/erase_async/
    // Batch::commit (see fix D — batches notify observers on success).
    //
    // v25.1 M1.5 implementation model (design decision C):
    //   Observers are stored in a mutex-protected vector (observer_mu_).
    //   notify_observers() walks the write-set under a lock on observer_mu_
    //   and calls each matching callback INLINE — there is no dedicated
    //   notification thread and no queue. The callback runs on the committing
    //   thread, under observer_mu_.
    //
    //   Known limitation (deliberately deferred to M6):
    //     Because notify_observers() holds observer_mu_ while invoking the
    //     callback, a callback that re-enters the observer machinery (e.g.
    //     calls observe() or unregisters a handle) or calls back into the
    //     Database in a way that re-enters notify_observers() will DEADLOCK
    //     or throw std::system_error (non-recursive std::mutex). This is
    //     verified by run_observer_test's reentrancy block — the deadlock
    //     is the documented expected behavior, NOT a bug to fix by making
    //     the mutex recursive. The real fix is the dedicated notification
    //     thread + queue (decouples callback execution from the commit
    //     path and from observer_mu_), scheduled for M6 (production
    //     observability). The <5% hot-path overhead target is unmeasured
    //     in M1.5 (run_observer_test is single-threaded correctness only);
    //     measuring it is M6 work alongside the thread.
    //
    //   Why not fix it in M1.5: the inline model is correct for the
    //   non-reentrant case (the common case) and the thread adds real
    //   complexity (queue lifetime, shutdown ordering, backpressure).
    //   M1.5's scope was the async-first public API surface, not the
    //   observer delivery mechanism. The reentrancy hazard is documented
    //   + regression-tested rather than silently left.

    using ObserverCallback = std::function<void(const std::string& key,
                                                const std::optional<std::string>& old_val,
                                                const std::optional<std::string>& new_val)>;

    class ObserverHandle {
    public:
        ObserverHandle() = default;
        ObserverHandle(ObserverHandle&&) = default;
        ObserverHandle& operator=(ObserverHandle&&) = default;
        ~ObserverHandle() {
            if (unregister_) unregister_();
        }
        bool valid() const { return static_cast<bool>(unregister_); }
    private:
        std::function<void()> unregister_;
        friend class Database;
        ObserverHandle(std::function<void()> unreg) : unregister_(std::move(unreg)) {}
    };

    ObserverHandle observe(std::string prefix, ObserverCallback callback) {
        check_open();
        std::lock_guard<std::mutex> lk(observer_mu_);
        size_t idx = observers_.size();
        observers_.push_back({std::move(prefix), std::move(callback)});
        return ObserverHandle([this, idx]() {
            std::lock_guard<std::mutex> lk(observer_mu_);
            if (idx < observers_.size()) {
                observers_[idx].callback = nullptr;  // mark as removed
            }
        });
    }

    // ======================== M1.6 Phase 2: range_scan_stream ========================
    //
    // v25.1 M1.6 (Phase 2): IMPLEMENTED (was M1.5 stub). Wraps the engine's
    // B+ tree Cursor (M1.3/M1.4) for real incremental iteration — no O(n²)
    // re-scan per chunk. The stream holds a pImpl (ChronoKVRangeScanCursorState)
    // that contains the Cursor + SnapshotGuard, defined after BTree.
    //
    // Consistency guarantee: per-page snapshot via Cursor's shared latch
    // (M1.4). A cursor observes the state of each leaf at the moment it
    // enters it; concurrent mutations to that page are invisible until the
    // cursor moves on. Cross-page consistency is NOT guaranteed.

    class RangeScanStream {
    public:
        RangeScanStream(Database& db, std::string lo, std::string hi);
        ~RangeScanStream();
        RangeScanStream(const RangeScanStream&) = delete;
        RangeScanStream& operator=(const RangeScanStream&) = delete;
        RangeScanStream(RangeScanStream&&) noexcept;
        // No move-assign: db_ is a reference (can't be reassigned).
        RangeScanStream& operator=(RangeScanStream&&) = delete;
        bool has_next();
        std::pair<std::string, std::string> next();
    private:
        Database& db_;
        std::unique_ptr<ChronoKVRangeScanCursorState> state_;
    };

private:
    // v25.1 M1.5: observer infrastructure.
    struct Observer {
        std::string prefix;
        ObserverCallback callback;
    };
    std::mutex observer_mu_;
    std::vector<Observer> observers_;

    // v25.1 M1.5: called from commit path to notify observers.
    // Walks the write-set, checks prefixes, enqueues notifications.
    void notify_observers(const WriteSet& ws) {
        if (observers_.empty()) return;
        std::lock_guard<std::mutex> lk(observer_mu_);
        for (auto& [key, value, deleted] : ws) {
            for (auto& obs : observers_) {
                if (obs.callback && key.compare(0, obs.prefix.size(), obs.prefix) == 0) {
                    std::optional<std::string> old_val;  // not available in blind-write path
                    std::optional<std::string> new_val = deleted
                        ? std::nullopt
                        : std::optional<std::string>(value);
                    obs.callback(key, old_val, new_val);
                }
            }
        }
    }
private:
    static Status map_txn_result(::TxnResult r) {
        switch (r) {
            case ::TxnResult::Committed:          return Status::OK;
            case ::TxnResult::Conflict:           return Status::Conflict;
            case ::TxnResult::InvalidState:       return Status::InvalidState;
            case ::TxnResult::TooLarge:           return Status::TooLarge;
            case ::TxnResult::WalFailure:         return Status::WalFailure;
            case ::TxnResult::DatabaseFailed:     return Status::Failed;
            case ::TxnResult::InvalidTransaction: return Status::InvalidTransaction;
        }
        return Status::Failed;
    }

    friend class Transaction;
};

// ---- Transaction ------------------------------------------------------
class Transaction {
    std::unique_ptr<ReadWriteTransaction> txn_;
    bool active_ = true;
    // v24 fix: weak_ptr into the owning Database's liveness flag.
    // Locks the weak_ptr and inspects the boolean value — this catches
    // both Database destruction (weak_ptr expires) AND Database::close()
    // (boolean set to false before engine is reset).
    std::weak_ptr<bool> db_alive_;

    // Private constructor (used by Database::begin)
    Transaction(ChronoKV& engine, std::weak_ptr<bool> db_alive)
        : txn_(std::make_unique<ReadWriteTransaction>(engine, db_alive)),
          db_alive_(std::move(db_alive)) {}

    void check_active() const {
        if (!active_) throw LifecycleError("transaction is not active");
        // v24 fix: lock the weak_ptr and check the boolean value.
        // expired() => Database was destroyed.  !*sp => Database::close()
        // was called.  Either way the engine is gone.
        if (auto sp = db_alive_.lock(); !sp || !*sp)
            throw LifecycleError(
                "transaction's Database was closed or destroyed while the "
                "transaction was still active");
    }

public:
    ~Transaction() {
        // v24 fix: if the engine is still alive and the transaction was
        // never explicitly committed or aborted, this is a programming
        // error — abort.  But if the Database was closed or destroyed
        // under this transaction, the ReadWriteTransaction destructor
        // (guarded by its own weak_ptr check) will safely skip cleanup,
        // so we just destroy txn_ and return.
        if (active_) {
            if (auto sp = db_alive_.lock(); sp && *sp) {
                // Engine is still live — this is a real leak, abort.
                std::abort();
            }
            // Engine gone — let ~ReadWriteTransaction clean up safely.
        }
    }

    // Move-only — explicit because active_ must be cleared on the source.
    Transaction(Transaction&& o) noexcept
        : txn_(std::move(o.txn_))
          , active_(o.active_)
          , db_alive_(std::move(o.db_alive_)) {
        o.active_ = false;
    }
    Transaction& operator=(Transaction&& o) noexcept {
        if (this != &o) {
            // v24 FIX (Group 2, Fix 11) -- Option C (revised):
            //
            // If *this is still active AND the engine is still live, the
            // target has a pending transaction that was never committed or
            // explicitly aborted. The previous code silently overwrote it,
            // leaking the reader slot and phantom registration. The spec's
            // first draft asked for std::abort() (process-level), which
            // would fire for the legitimate move-assign-over-active pattern
            // used by retry loops. The correct fix is to call the target's
            // OWN abort() method -- the existing logical rollback path
            // (release reader slot, deregister phantom reader) -- before
            // completing the move.
            //
            // std::abort() (process-level) remains reserved for the
            // DESTRUCTOR's contract: a Transaction that goes out of scope
            // while still active+live is a real leak (the caller forgot to
            // commit/abort and isn't transferring ownership anywhere).
            // Move-assignment is different: ownership is being transferred,
            // so the old transaction should be rolled back, not crashed.
            //
            // abort() is technically not noexcept (it acquires mutexes via
            // ~ReadWriteTransaction -> release_slot/deregister_phantom_reader),
            // but the throw paths are exotic (std::system_error on mutex
            // acquisition failure). Wrap in try/catch to preserve the
            // noexcept guarantee; on catch, proceed with the move (the
            // target's txn_ may be in an inconsistent state, but
            // std::move(o.txn_) will overwrite it anyway).
            if (active_) {
                if (auto sp = db_alive_.lock(); sp && *sp) {
                    try {
                        abort();
                    } catch (...) {
                        // Swallow to preserve noexcept. The target's
                        // cleanup may be incomplete, but we're about to
                        // overwrite txn_ and active_ anyway.
                    }
                }
            }
            txn_ = std::move(o.txn_);
            active_ = o.active_;
            db_alive_ = std::move(o.db_alive_);
            o.active_ = false;
        }
        return *this;
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    std::optional<std::string> get(std::string_view key) {
        check_active();
        return txn_->read(std::string(key));
    }

    void put(std::string_view key, std::string_view value) {
        check_active();
        txn_->write(std::string(key), std::string(value));
    }

    void erase(std::string_view key) {
        check_active();
        txn_->del(std::string(key));
    }

    std::vector<std::pair<std::string, std::string>>
    range_scan(std::string_view lo, std::string_view hi) {
        check_active();
        return txn_->range_scan(std::string(lo), std::string(hi));
    }

    Status commit() {
        check_active();
        ::TxnResult result = txn_->commit();
        active_ = false;
        return Database::map_txn_result(result);
    }

    void abort() {
        if (active_) {
            // ReadWriteTransaction has no explicit abort(); resetting the
            // unique_ptr invokes its destructor, which releases the reader
            // slot and deregisters the phantom reader.
            txn_.reset();
            active_ = false;
        }
    }

    bool is_active() const { return active_; }

    friend class Database;
};

// Database::begin implementation (must be after Transaction is defined)
inline Transaction Database::begin() {
    check_open();
    return Transaction(*engine_, alive_);
}

} // namespace chronokv

// =====================================================================
// v25.1 M1: Page-Structured Storage components
//
// The following sections (PagePool, BTree, Latency measurement) are
// the Pillar 1 + M0 measurement-core work. They are in separate namespaces
// (chronokv_page, chronokv_btree, chronokv_latency) and are not yet
// integrated into the ChronoKV engine — that integration is M1.4-M1.6.
//
// They live in this single header so the project stays at two files
// (chronokv.hpp + main.cpp) rather than sprawling into a header forest.
// =====================================================================

namespace chronokv_page {

// Standard page size. 4 KiB matches OS page and typical O_DIRECT
// alignment. Cache lines are 64 B; the 4 KiB size is NOT chosen for
// cache properties (v25.1 §3 corrects the original draft's wrong
// "cache-line burst" rationale).
constexpr size_t PAGE_SIZE = 4096;

// Page ID. 0 is reserved as "null" (no page).
using PageId = uint64_t;

// Opaque page handle. The pool returns this; callers cast to their
// own page layout (B+ tree interior, B+ tree leaf, etc.).
struct Page {
    uint8_t bytes[PAGE_SIZE];
};

class PagePool {
public:
    // Construct a pool with `capacity` bytes (rounded up to PAGE_SIZE).
    // Default 1 GiB. Throws std::bad_alloc if the reservation fails.
    explicit PagePool(size_t capacity = 1ULL * 1024 * 1024 * 1024)
        : capacity_(round_up(capacity, PAGE_SIZE)),
          base_(allocate_raw(capacity_)),
          bump_(0),
          free_list_() {}

    ~PagePool() {
        release_raw(base_, capacity_);
    }

    PagePool(const PagePool&) = delete;
    PagePool& operator=(const PagePool&) = delete;
    PagePool(PagePool&&) = delete;
    PagePool& operator=(PagePool&&) = delete;

    // Allocate a page. Returns the page ID (1-based; 0 is null).
    // The page contents are zeroed on allocation.
    PageId alloc() {
        std::lock_guard<std::mutex> lk(mu_);
        PageId id;
        if (!free_list_.empty()) {
            id = free_list_.back();
            free_list_.pop_back();
        } else {
            if (bump_ + PAGE_SIZE > capacity_) {
                throw std::bad_alloc();
            }
            id = (bump_ / PAGE_SIZE) + 1;  // 1-based; 0 is null
            bump_ += PAGE_SIZE;
        }
        // Zero the page on allocation (defensive — callers shouldn't
        // assume stale contents).
        std::memset(get(id)->bytes, 0, PAGE_SIZE);
        return id;
    }

    // Free a page. The page ID may be reused by a subsequent alloc();
    // the ABA interaction with hazard pointers is specified in v25.1
    // §6 (HP4 — the free-list reuse is tagged or Version allocations
    // are never reused). For M1.1 (single-threaded page pool), this
    // is straightforward.
    void free(PageId id) {
        if (id == 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        free_list_.push_back(id);
    }

    // Get a pointer to a page by ID. The pointer is valid until the
    // pool is destroyed or the page is freed (and re-allocated).
    // Callers must hold a hazard pointer (Pillar 4) if they need
    // the pointer to remain valid across concurrent free() calls.
    Page* get(PageId id) {
        if (id == 0) return nullptr;
        size_t offset = (id - 1) * PAGE_SIZE;
        // No bounds check on the hot path — callers should not pass
        // invalid IDs. Debug builds can add an assert.
        return reinterpret_cast<Page*>(static_cast<uint8_t*>(base_) + offset);
    }

    const Page* get(PageId id) const {
        if (id == 0) return nullptr;
        size_t offset = (id - 1) * PAGE_SIZE;
        return reinterpret_cast<const Page*>(static_cast<const uint8_t*>(base_) + offset);
    }

    // Statistics for diagnostics.
    size_t capacity() const { return capacity_; }
    size_t allocated() const {
        std::lock_guard<std::mutex> lk(mu_);
        return bump_;
    }
    size_t free_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return free_list_.size();
    }
    size_t live_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return (bump_ / PAGE_SIZE) - free_list_.size();
    }

    // Fragmentation ratio: live pages / total allocated pages.
    // 1.0 = no fragmentation (every allocated page is in use).
    // Lower = more fragmentation (more pages on the free-list).
    double fragmentation_ratio() const {
        std::lock_guard<std::mutex> lk(mu_);
        size_t total = bump_ / PAGE_SIZE;
        if (total == 0) return 1.0;
        size_t live = total - free_list_.size();
        return static_cast<double>(live) / total;
    }

private:
    static size_t round_up(size_t v, size_t multiple) {
        return (v + multiple - 1) & ~(multiple - 1);
    }

    static void* allocate_raw(size_t bytes) {
        // Use aligned_alloc for OS-page alignment (PAGE_SIZE).
        // Fallback: if aligned_alloc fails, try malloc (slower but
        // works everywhere).
        void* p = nullptr;
        #ifdef _WIN32
            p = _aligned_malloc(bytes, PAGE_SIZE);
        #else
            p = std::aligned_alloc(PAGE_SIZE, bytes);
        #endif
        if (!p) throw std::bad_alloc();
        return p;
    }

    static void release_raw(void* p, size_t /*bytes*/) {
        #ifdef _WIN32
            _aligned_free(p);
        #else
            std::free(p);
        #endif
    }

    size_t capacity_;
    void* base_;
    size_t bump_;
    std::vector<PageId> free_list_;
    mutable std::mutex mu_;
};

} // namespace chronokv_page


namespace chronokv_btree {

using chronokv_page::PageId;
using chronokv_page::Page;
using chronokv_page::PagePool;
using chronokv_page::PAGE_SIZE;

// ---- B+ tree page layout (single-threaded, M1.2) ----
//
// PageHeader (40 B) at offset 0.
// Slot array grows forward from offset 40.
// Slab grows backward from offset PAGE_SIZE.
// Offsets stored in slots are RELATIVE TO PAGE START.
//
// v25.1 M1.2 (leaf-chain fix): leaf pages and interior pages have
// DISTINCT fields for their distinct purposes. The previous design
// overloaded a single `next_leaf_id` field for both "next leaf in
// scan order" (leaf pages) and "rightmost child page ID" (interior
// pages). That overloading allowed an interior-node operation to
// corrupt a leaf page's chain pointer — the exact bug the fuzz
// harness caught.
//
// The fix: two separate fields, `next_leaf_id` (only meaningful on
// a leaf page) and `rightmost_child_id` (only meaningful on an
// interior page). Type-checked accessors enforce the distinction
// at every call site — a future code path (including M1.4's
// concurrent work) cannot accidentally write the wrong one.

#pragma pack(push, 1)
struct PageHeader {
    uint8_t  is_leaf;        // 1 = leaf, 0 = interior
    uint8_t  reserved[3];
    uint16_t key_count;      // number of slots in use
    uint16_t reserved2;
    uint32_t next_leaf_id;       // LEAF ONLY: PageId of next leaf (for range scans)
    uint32_t rightmost_child_id; // INTERIOR ONLY: PageId of rightmost child
    uint32_t min_key_off;        // offset of the page's minimum key (for fence pruning)
    uint16_t min_key_len;
    uint16_t reserved3;
    uint32_t max_key_off;        // offset of the page's maximum key (for fence pruning)
    uint16_t max_key_len;
    uint16_t reserved4;
};
static_assert(sizeof(PageHeader) == 32, "PageHeader must be 32 bytes");

// Leaf slot: columnar (key_off, key_len, value_off, value_len) — 12 bytes.
// (The v25.1 §3 spec includes commit_ts in the slot — that's deferred to
// M1.4 integration, where it'll be a separate column to keep the slot
// compact. For M1.2, the B+ tree stores keys+values only; commit_ts comes
// from the engine during integration.)
struct LeafSlot {
    uint16_t key_off;     // offset from page start
    uint16_t key_len;
    uint16_t value_off;   // offset from page start; 0 = deleted (not used in M1.2)
    uint16_t value_len;
};

// Interior slot: (key_off, key_len, child_page_id) — 12 bytes.
struct InteriorSlot {
    uint16_t key_off;
    uint16_t key_len;
    uint64_t child_page_id;  // left child of this key
};
#pragma pack(pop)

// The free-space gap is between (header + slots) and (slab start).
//   free_lo = sizeof(PageHeader) + key_count * sizeof(slot)
//   free_hi = slab_start = the lowest offset currently used by the slab
//
// We need to track free_hi explicitly because the slab isn't contiguous
// — deletes can leave holes. After compaction, free_hi moves down to
// just above the highest live slab byte.

// ======================== Latch table (M1.4) ========================
//
// Per-page shared_mutex stored in a separate hash map, keyed by PageId.
// Page bytes stay at fixed 4 KiB; the latch lives in a separate
// allocation. Readers acquire shared latch; writers (put/erase/split/
// compact) acquire exclusive latch.
//
// The latch table is mutex-protected for the map itself, but the
// individual shared_mutexes are accessed without the table mutex once
// looked up. Latch creation is lazy (on first access).
class LatchTable {
public:
    // Acquire a shared (read) latch on page `id`. Blocks if a writer
    // holds the exclusive latch.
    std::shared_lock<std::shared_mutex> lock_shared(PageId id) const {
        std::shared_mutex* m = const_cast<LatchTable*>(this)->get_or_create(id);
        return std::shared_lock<std::shared_mutex>(*m);
    }

    // Acquire an exclusive (write) latch on page `id`. Blocks if any
    // reader or writer holds the latch.
    std::unique_lock<std::shared_mutex> lock_exclusive(PageId id) {
        std::shared_mutex* m = get_or_create(id);
        return std::unique_lock<std::shared_mutex>(*m);
    }

    // Remove a latch entry (called when a page is freed). Safe to call
    // even if no latch exists for this id.
    void erase(PageId id) {
        std::lock_guard<std::mutex> lk(mu_);
        latches_.erase(id);
    }

    // v25.1 M1.4: public access to get_or_create (Cursor needs it for
    // manual latch management, avoiding the optional<shared_lock> pattern
    // that triggers GCC -Wmaybe-uninitialized).
    std::shared_mutex* get_or_create(PageId id) const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        auto it = latches_.find(id);
        if (it == latches_.end()) {
            it = const_cast<std::unordered_map<PageId, std::unique_ptr<std::shared_mutex>>&>(latches_).emplace(id, std::make_unique<std::shared_mutex>()).first;
        }
        return it->second.get();
    }

private:

    std::mutex mu_;
    std::unordered_map<PageId, std::unique_ptr<std::shared_mutex>> latches_;
};

// ======================== Hazard pointer (M1.4) ========================
//
// A single hazard pointer per reader slot. Published before dereferencing
// a page; cleared after. A page cannot be freed while any HP points to it.
//
// M1.4 implementation: the HP is a raw PageId (not a Page*). The
// reclamation check scans all published HPs before freeing a page.
// This is simpler than a full HP array and sufficient for M1.4's
// single-reader concurrent-writer model.
struct HazardPointer {
    std::atomic<PageId> hp{0};  // 0 = not published

    void publish(PageId id) { hp.store(id, std::memory_order_seq_cst); }
    void clear() { hp.store(0, std::memory_order_seq_cst); }
    PageId load() const { return hp.load(std::memory_order_seq_cst); }
};

// Global HP registry — all reader slots register here.
// A page is safe to free only if no HP in the registry points to it.
class HPRegistry {
public:
    static HPRegistry& instance() {
        static HPRegistry reg;
        return reg;
    }

    // Register a new HP slot. Returns a reference to the HP.
    // The reference is stable for the lifetime of the registry.
    HazardPointer& register_hp() {
        std::lock_guard<std::mutex> lk(mu_);
        hps_.push_back(std::make_unique<HazardPointer>());
        return *hps_.back();
    }

    // Check if any published HP points to `id`.
    bool is_hazardous(PageId id) const {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& hp : hps_) {
            if (hp->load() == id) return true;
        }
        return false;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<HazardPointer>> hps_;
};

class BTree {
    friend class ::ChronoKV;  // v25.1 M1.6: for fence_recheck_retries_ access
public:
    explicit BTree(PagePool& pool) : pool_(pool), root_id_(0) {
        // Create an empty leaf as the root.
        root_id_.store(pool_.alloc(), std::memory_order_release);
        Page* p = pool_.get(root_id_.load(std::memory_order_acquire));
        PageHeader* h = header(p);
        h->is_leaf = 1;
        h->key_count = 0;
        set_next_leaf(p, 0);
        h->min_key_off = h->max_key_off = 0;
        h->min_key_len = h->max_key_len = 0;
    }

    // Insert or update a key. Returns true on insert, false on update.
    bool put(const std::string& key, const std::string& value) {
        generation_.fetch_add(1, std::memory_order_relaxed);  // M1.4: atomic (concurrent-safe)
        InsertResult r = put_recursive(root_id_.load(std::memory_order_acquire), key, value);
        if (r.split) {
            // Root split: create a new root with the split key + two children.
            PageId new_root = pool_.alloc();
            // v25.1 M2 FIX: acquire exclusive latch on new_root BEFORE writing
            // to it. Without this, a concurrent reader (find_leaf_crabbing)
            // that loads root_id_ = new_root after the relaxed store below
            // can read new_root's bytes without synchronization — a data race
            // caught by TSan on native hardware (higher concurrency than the
            // 2-CPU container). The latch is released after root_id_ is
            // stored (with release ordering) so readers that see the new
            // root_id_ also see the fully-written page.
            auto new_root_latch = latches_.lock_exclusive(new_root);
            Page* rp = pool_.get(new_root);
            std::memset(rp->bytes, 0, PAGE_SIZE);
            PageHeader* rh = header(rp);
            rh->is_leaf = 0;
            rh->key_count = 1;
            set_rightmost_child(rp, r.new_child);  // rightmost child
            // Write the split key into the slab.
            InteriorSlot* slots = interior_slots(rp);
            uint16_t key_off = PAGE_SIZE - r.split_key.size();
            std::memcpy(reinterpret_cast<uint8_t*>(rp) + key_off,
                         r.split_key.data(), r.split_key.size());
            slots[0].key_off = key_off;
            slots[0].key_len = r.split_key.size();
            slots[0].child_page_id = root_id_.load(std::memory_order_acquire);  // left child
            update_fences_interior(rp);
            // v25.1 M2 FIX: store with release ordering so readers that see
            // the new root_id_ also see the fully-written page contents.
            root_id_.store(new_root, std::memory_order_release);
            new_root_latch.unlock();
        }
        return r.inserted;
    }

    // v25.1 M1.6: put that returns the old value on update (for ensure_index's
    // double-allocation race handling). If the key existed, *old_value is set
    // and the function returns true (existed). If the key is new, returns false.
    bool put_with_old(const std::string& key, const std::string& value,
                      std::string* old_value) {
        generation_.fetch_add(1, std::memory_order_relaxed);
        // Check if key exists first (shared crabbing).
        std::string existing;
        bool existed = get(key, &existing);
        if (existed) {
            // Key exists — we're about to overwrite. Save the old value.
            *old_value = existing;
        }
        InsertResult r = put_recursive(root_id_.load(std::memory_order_acquire), key, value);
        if (r.split) {
            PageId new_root = pool_.alloc();
            // v25.1 M2 FIX: latch new_root before writing (same as put()).
            auto new_root_latch = latches_.lock_exclusive(new_root);
            Page* rp = pool_.get(new_root);
            std::memset(rp->bytes, 0, PAGE_SIZE);
            PageHeader* rh = header(rp);
            rh->is_leaf = 0;
            rh->key_count = 1;
            set_rightmost_child(rp, r.new_child);
            InteriorSlot* slots = interior_slots(rp);
            uint16_t key_off = PAGE_SIZE - r.split_key.size();
            std::memcpy(reinterpret_cast<uint8_t*>(rp) + key_off,
                         r.split_key.data(), r.split_key.size());
            slots[0].key_off = key_off;
            slots[0].key_len = r.split_key.size();
            slots[0].child_page_id = root_id_.load(std::memory_order_acquire);
            update_fences_interior(rp);
            root_id_.store(new_root, std::memory_order_release);
            new_root_latch.unlock();
        }
        return existed;
    }
    bool get(const std::string& key, std::string* out_value) const {
        return get_recursive(root_id_.load(std::memory_order_acquire), key, out_value);
    }

    // Erase a key. Returns true if the key existed.
    bool erase(const std::string& key) {
        generation_.fetch_add(1, std::memory_order_relaxed);  // M1.4: atomic (concurrent-safe)
        return erase_recursive(root_id_.load(std::memory_order_acquire), key);
    }

    // Range scan: returns all (key, value) pairs with lo <= key <= hi.
    // Inclusive on both ends (matching the engine's range_scan contract).
    //
    // v25.1 M1.3: refactored to use Cursor internally, so the fence
    // pruning, fence-consistency assertion, and generation check are
    // exercised through the same code path that external cursor users
    // use. This means a bug in range_scan is a bug in Cursor and vice
    // versa — no divergence between the two paths.
    std::vector<std::pair<std::string, std::string>>
    range_scan(const std::string& lo, const std::string& hi) const {
        std::vector<std::pair<std::string, std::string>> result;
        Cursor c = open_cursor(lo, hi);
        while (c.valid()) {
            result.emplace_back(c.key(), c.value());
            c.next();
        }
        return result;
    }

    // For diagnostics: count total keys in the tree.
    size_t size() const {
        return size_recursive(root_id_.load(std::memory_order_acquire));
    }

    // For diagnostics: tree depth (root = depth 1).
    size_t depth() const {
        size_t d = 0;
        PageId id = root_id_.load(std::memory_order_acquire);
        while (id != 0) {
            const Page* p = pool_.get(id);
            const PageHeader* h = header(p);
            ++d;
            if (h->is_leaf) break;
            const InteriorSlot* slots = interior_slots(p);
            if (h->key_count == 0) break;
            id = slots[0].child_page_id;
        }
        return d;
    }

    // v25.1 M1.3/M1.4: generation counter accessor (for Cursor).
    uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }

    // v25.1 M1.3/M1.4: Cursor — iterator-style range scan.
    //
    // Yields one (key, value) pair at a time without materializing the
    // entire result set. Uses fence-based pruning to skip pages whose
    // [min_key, max_key] doesn't intersect [lo, hi].
    //
    // M1.4: per-page snapshot via shared latch + hazard pointer.
    // The cursor holds a shared latch on the current leaf, preventing
    // concurrent writers from modifying it. When moving to the next
    // leaf, the cursor reads next_leaf_id under the current latch,
    // then releases the latch and acquires the next leaf's latch.
    //
    // The generation counter remains as a debug-mode tripwire: if a
    // cursor detects generation_ changed, it means a concurrent write
    // happened to a page the cursor was NOT latched on (which is fine
    // — the latch only protects the current page). The assertion is
    // relaxed in M1.4 to only fire if the cursor's current page was
    // modified, not if any write happened tree-wide.
    class Cursor {
    public:
        Cursor(const BTree& tree, const std::string& lo, const std::string& hi)
            : tree_(tree), lo_(lo), hi_(hi), gen_(tree.generation()),
              leaf_id_(0), slot_idx_(0), exhausted_(false),
              key_count_snapshot_(0) {
            if (lo_ > hi_) {
                exhausted_ = true;
                return;
            }
            // v25.1 M1.6: crabbing descent — find_leaf_for_scan_crabbing
            // returns the leaf ID AND holds the leaf's shared latch.
            leaf_id_ = tree_.find_leaf_for_scan_crabbing(lo_, initial_latch_);
            slot_idx_ = 0;
            enter_leaf_from_crabbing();
            advance_to_valid();
        }

        ~Cursor() {
            leave_leaf();
        }

        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;
        Cursor(Cursor&&) = default;
        Cursor& operator=(Cursor&&) = default;

        bool valid() const {
            return !exhausted_;
        }

        bool next() {
            if (exhausted_) return false;
            ++slot_idx_;
            return advance_to_valid();
        }

        const std::string& key() const {
            assert(!exhausted_ && "key() called on exhausted cursor");
            return cur_key_;
        }

        const std::string& value() const {
            assert(!exhausted_ && "value() called on exhausted cursor");
            return cur_value_;
        }

    private:
        const BTree& tree_;
        std::string lo_, hi_;
        uint64_t gen_;
        PageId leaf_id_;
        uint16_t slot_idx_;
        std::string cur_key_, cur_value_;
        bool exhausted_;
        uint16_t key_count_snapshot_;  // captured at leaf entry

        // Per-page latch and HP. The latch is held while reading the
        // current leaf; released when moving to the next leaf.
        //
        // v25.1 M1.4: manual latch management instead of optional<shared_lock>.
        // std::optional<std::shared_lock<...>> triggers a GCC -Wmaybe-uninitialized
        // false positive through the optional payload's move/destruction path.
        // The manual pattern (shared_mutex* + bool owns) avoids the GCC analysis
        // gap entirely and is equally safe — enter_leaf() sets both before use,
        // leave_leaf() releases and clears both.
        std::shared_mutex* leaf_latch_mu_ = nullptr;
        bool owns_latch_ = false;
        HazardPointer* hp_ = nullptr;  // published while on a leaf
        // v25.1 M1.6: shared latch from the initial crabbing descent.
        // Held until the Cursor moves to the next leaf or is destroyed.
        std::shared_lock<std::shared_mutex> initial_latch_;

        // v25.1 M1.6: enter_leaf variant for the initial crabbing descent.
        // The latch is already held (in initial_latch_); just set up HP +
        // snapshot key_count.
        void enter_leaf_from_crabbing() {
            if (leaf_id_ == 0) return;
            // The latch is held via initial_latch_. Set up the manual members
            // for consistency with leave_leaf.
            leaf_latch_mu_ = tree_.latches_.get_or_create(leaf_id_);
            owns_latch_ = false;  // initial_latch_ owns it, not the manual path
            if (!hp_) hp_ = &HPRegistry::instance().register_hp();
            hp_->publish(leaf_id_);
            const Page* p = tree_.pool_.get(leaf_id_);
            key_count_snapshot_ = tree_.header(p)->key_count;
        }

        void enter_leaf() {
            if (leaf_id_ == 0) return;
            // Acquire shared latch on the current leaf.
            leaf_latch_mu_ = tree_.latches_.get_or_create(leaf_id_);
            leaf_latch_mu_->lock_shared();
            owns_latch_ = true;
            // Publish HP (prevents the page from being freed while we
            // read it — important for future merge/rebalance).
            if (!hp_) hp_ = &HPRegistry::instance().register_hp();
            hp_->publish(leaf_id_);
            // Capture snapshot of key_count (per-page snapshot semantic).
            const Page* p = tree_.pool_.get(leaf_id_);
            key_count_snapshot_ = tree_.header(p)->key_count;

            // Debug tripwire: if generation changed since open, a write
            // happened tree-wide. This is ALLOWED in M1.4 (concurrent
            // writes to OTHER pages are fine). We only assert that the
            // write didn't happen to THIS page (which the latch prevents).
            // The latch guarantees this, so no assertion needed here —
            // the latch IS the enforcement.
        }

        void leave_leaf() {
            if (hp_) hp_->clear();
            if (owns_latch_) {
                leaf_latch_mu_->unlock_shared();
                owns_latch_ = false;
                leaf_latch_mu_ = nullptr;
            }
            // v25.1 M1.6: release the initial crabbing latch if held.
            // Only release once — use owns_latch_ == false as the signal
            // that initial_latch_ is the active one (set in enter_leaf_from_crabbing).
            // After the first leave_leaf, initial_latch_ is released and we
            // switch to the manual latch path for subsequent leaves.
            if (initial_latch_.owns_lock()) {
                initial_latch_.unlock();
            }
        }

        bool advance_to_valid() {
            while (leaf_id_ != 0) {
                const Page* p = tree_.pool_.get(leaf_id_);
                const PageHeader* h = tree_.header(p);

                // v25.1 M1.2 fence-consistency assertion.
                assert(tree_.verify_fence_consistency(p) && "stale fence in cursor");

                // Fence pruning: if this page's min_key > hi, done.
                if (h->min_key_len > 0) {
                    std::string page_min(
                        reinterpret_cast<const char*>(p) + h->min_key_off,
                        h->min_key_len);
                    if (page_min > hi_) {
                        leave_leaf();
                        exhausted_ = true;
                        return false;
                    }
                }

                const LeafSlot* slots = tree_.leaf_slots(p);
                // Use the snapshot key_count, not the live one —
                // a concurrent writer that's blocked on our latch can't
                // have changed key_count, but using the snapshot is the
                // correct per-page snapshot semantic.
                while (slot_idx_ < key_count_snapshot_) {
                    std::string k(
                        reinterpret_cast<const char*>(p) + slots[slot_idx_].key_off,
                        slots[slot_idx_].key_len);
                    if (k < lo_) {
                        ++slot_idx_;
                        continue;
                    }
                    if (k > hi_) {
                        leave_leaf();
                        exhausted_ = true;
                        return false;
                    }
                    cur_key_ = std::move(k);
                    cur_value_.assign(
                        reinterpret_cast<const char*>(p) + slots[slot_idx_].value_off,
                        slots[slot_idx_].value_len);
                    return true;
                }
                // Exhausted this leaf — move to the next.
                // Read next_leaf_id UNDER the current latch (before releasing).
                PageId next = tree_.get_next_leaf(p);
                leave_leaf();
                leaf_id_ = next;
                slot_idx_ = 0;
                if (leaf_id_ != 0) {
                    enter_leaf();
                }
            }
            exhausted_ = true;
            return false;
        }
    };

    Cursor open_cursor(const std::string& lo, const std::string& hi) const {
        return Cursor(*this, lo, hi);
    }

    // v25.1 M1.6: find_leaf_for_scan now crabs (holds parent shared latch
    // until child's is acquired). Returns the leaf ID AND the held shared
    // latch (via out-param). The caller (Cursor) uses this latch directly
    // instead of acquiring it in enter_leaf. This closes the race window
    // between find_leaf_for_scan returning and enter_leaf acquiring the latch.
    PageId find_leaf_for_scan_crabbing(const std::string& key,
                                       std::shared_lock<std::shared_mutex>& held_latch) const {
        return find_leaf_crabbing(root_id_.load(std::memory_order_acquire), key, held_latch);
    }

    // v25.1 M1.3: find the leaf that would contain `key`. Public wrapper
    // around find_leaf for Cursor's use. (Non-crabbing — kept for the
    // standalone-tree tests that don't need concurrent safety.)
    PageId find_leaf_for_scan(const std::string& key) const {
        return find_leaf(root_id_.load(std::memory_order_acquire), key);
    }

    // v25.1 M1.2 diagnostic: verify the leaf chain is intact.
    //
    // Walks the leaf chain from the leftmost leaf to the rightmost,
    // verifying:
    //   1. Every next_leaf_id points to a page that is actually a leaf.
    //   2. The keys in each leaf are <= the keys in the next leaf
    //      (i.e., the chain is in sorted order).
    //   3. No cycles (a next_leaf_id doesn't point back to a leaf
    //      already visited).
    //
    // Returns a description of the first corruption found, or an empty
    // string if the chain is intact. Records which page ID was corrupted
    // and what the bad pointer was, so the caller can correlate with the
    // operation that caused it.
    struct ChainCorruption {
        bool found = false;
        PageId corrupted_leaf = 0;     // the leaf whose next_leaf_id is wrong
        PageId bad_next = 0;           // the bad value of next_leaf_id
        std::string reason;            // human-readable description
        // For distinguishing field-aliasing vs split relink-order:
        // If bad_next points to an interior page, it's field-aliasing
        // corruption. If bad_next is 0 or points to the wrong leaf,
        // it's a split relink-order bug.
        bool bad_next_is_interior = false;
    };

    ChainCorruption verify_leaf_chain() const {
        ChainCorruption result;
        // Find the leftmost leaf by descending from the root.
        PageId id = root_id_.load(std::memory_order_acquire);
        while (true) {
            const Page* p = pool_.get(id);
            const PageHeader* h = header(p);
            if (h->is_leaf) break;
            const InteriorSlot* slots = interior_slots(p);
            if (h->key_count == 0) break;
            id = slots[0].child_page_id;
        }
        // Walk the chain.
        PageId current = id;
        std::vector<PageId> visited;
        std::string prev_max_key;
        while (current != 0) {
            const Page* p = pool_.get(current);
            const PageHeader* h = header(p);
            if (!h->is_leaf) {
                result.found = true;
                result.corrupted_leaf = visited.empty() ? 0 : visited.back();
                result.bad_next = current;
                result.bad_next_is_interior = true;
                result.reason = "next_leaf_id points to an interior page (field-aliasing corruption)";
                return result;
            }
            // Check for cycles.
            for (PageId v : visited) {
                if (v == current) {
                    result.found = true;
                    result.corrupted_leaf = current;
                    result.bad_next = current;
                    result.reason = "cycle detected: leaf " + std::to_string(current) + " visited twice";
                    return result;
                }
            }
            visited.push_back(current);
            // Check sorted order: this leaf's min key should be >= prev leaf's max key.
            if (h->key_count > 0 && !prev_max_key.empty()) {
                const LeafSlot* slots = leaf_slots(p);
                std::string cur_min(reinterpret_cast<const char*>(p) + slots[0].key_off,
                                      slots[0].key_len);
                if (cur_min < prev_max_key) {
                    result.found = true;
                    result.corrupted_leaf = current;
                    result.bad_next = get_next_leaf(p);
                    result.reason = "leaf chain out of order: " + cur_min + " < " + prev_max_key;
                    return result;
                }
            }
            // Update prev_max_key.
            if (h->key_count > 0) {
                const LeafSlot* slots = leaf_slots(p);
                prev_max_key.assign(
                    reinterpret_cast<const char*>(p) + slots[h->key_count - 1].key_off,
                    slots[h->key_count - 1].key_len);
            }
            current = get_next_leaf(p);
        }
        return result;
    }

    // v25.1 M1.2 structural invariant check: verify that every interior
    // node's separator keys correctly partition its children's key ranges.
    //
    // Convention: slots[i] = (key_i, child_i) where child_i is the LEFT
    // child of key_i. So:
    //   child_0 holds keys < key_0
    //   child_i (i > 0) holds keys in [key_{i-1}, key_i)
    //   rightmost_child holds keys >= key_{n-1}
    //
    // This function walks every interior node, collects the min and max
    // key from each child subtree, and verifies the bounds hold. Catches
    // separator-key violations at the moment they're introduced, not when
    // a later scan exposes them.
    //
    // Returns a description of the first violation found, or empty if OK.
    struct SeparatorViolation {
        bool found = false;
        PageId interior_page = 0;
        size_t slot_index = 0;
        std::string expected_lower;  // the bound that should hold
        std::string expected_upper;
        std::string actual_min;      // what the child subtree actually contains
        std::string actual_max;
        std::string reason;
    };

    SeparatorViolation verify_separator_invariants() const {
        // First check that slots within each page are sorted.
        SeparatorViolation v = verify_slot_order_recursive(root_id_.load(std::memory_order_acquire));
        if (v.found) return v;
        // Then check separator bounds.
        return verify_separators_recursive(root_id_.load(std::memory_order_acquire), "", "");
    }

    SeparatorViolation verify_slot_order() const {
        return verify_slot_order_recursive(root_id_.load(std::memory_order_acquire));
    }

    // v25.1 M1.2: verify that every leaf reachable via the tree structure
    // (through find_leaf / interior pointers) is also reachable via the
    // leaf chain from the leftmost leaf. Also checks for duplicate
    // reachability — the same leaf ID appearing at two different positions
    // in the chain, or reachable via two different top-down paths (aliasing).
    //
    // This catches:
    //   - Missing leaves: a leaf in the tree but not in the chain
    //   - Duplicate leaves: the same PageId appearing twice in the chain
    //   - Aliased leaves: two different interior pointers routing to the
    //     same leaf (which would mean two different key ranges share one
    //     leaf, violating the B+ tree invariant)
    struct ChainCompleteness {
        bool found = false;
        std::string reason;
        PageId bad_leaf = 0;
        // For missing-leaf: the leaf that's tree-reachable but not chain-reachable
        // For duplicate-leaf: the leaf that appears twice in the chain
        // For aliased-leaf: the leaf reachable via two paths
    };

    ChainCompleteness verify_chain_completeness() const {
        // Step 1: collect all leaf IDs reachable via the tree structure.
        std::set<PageId> tree_leaves;
        std::set<PageId> aliased;
        collect_tree_leaves(root_id_.load(std::memory_order_acquire), tree_leaves, aliased);

        if (!aliased.empty()) {
            ChainCompleteness c;
            c.found = true;
            c.bad_leaf = *aliased.begin();
            c.reason = "leaf " + std::to_string(c.bad_leaf) +
                       " reachable via two different top-down paths (aliasing)";
            return c;
        }

        // Step 2: walk the leaf chain from the leftmost leaf.
        std::set<PageId> chain_leaves;
        PageId id = root_id_.load(std::memory_order_acquire);
        // Descend to leftmost leaf.
        while (true) {
            const Page* p = pool_.get(id);
            const PageHeader* h = header(p);
            if (h->is_leaf) break;
            const InteriorSlot* slots = interior_slots(p);
            if (h->key_count == 0) break;
            id = slots[0].child_page_id;
        }
        // Walk the chain.
        while (id != 0) {
            if (chain_leaves.count(id)) {
                ChainCompleteness c;
                c.found = true;
                c.bad_leaf = id;
                c.reason = "leaf " + std::to_string(id) +
                           " appears at two positions in the chain (duplicate)";
                return c;
            }
            chain_leaves.insert(id);
            const Page* p = pool_.get(id);
            id = get_next_leaf(p);
        }

        // Step 3: check that every tree-reachable leaf is in the chain.
        for (PageId leaf : tree_leaves) {
            if (chain_leaves.find(leaf) == chain_leaves.end()) {
                ChainCompleteness c;
                c.found = true;
                c.bad_leaf = leaf;
                c.reason = "leaf " + std::to_string(leaf) +
                           " is tree-reachable but NOT in the leaf chain (missing)";
                return c;
            }
        }

        return {};
    }

private:
    SeparatorViolation verify_slot_order_recursive(PageId page_id) const {
        const Page* p = pool_.get(page_id);
        const PageHeader* h = header(p);

        if (h->is_leaf) {
            const LeafSlot* slots = leaf_slots(p);
            std::string prev;
            for (uint16_t i = 0; i < h->key_count; ++i) {
                std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                               slots[i].key_len);
                if (i > 0 && k < prev) {
                    SeparatorViolation v;
                    v.found = true;
                    v.interior_page = page_id;
                    v.actual_min = prev;
                    v.actual_max = k;
                    v.reason = "leaf slots out of order: " + prev + " > " + k;
                    return v;
                }
                prev = k;
            }
            return {};
        }

        // Interior node: check slot order, then recurse.
        const InteriorSlot* slots = interior_slots(p);
        std::string prev;
        for (uint16_t i = 0; i < h->key_count; ++i) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            if (i > 0 && k < prev) {
                SeparatorViolation v;
                v.found = true;
                v.interior_page = page_id;
                v.actual_min = prev;
                v.actual_max = k;
                v.reason = "interior slots out of order: " + prev + " > " + k;
                return v;
            }
            prev = k;
            // Recurse into child.
            SeparatorViolation cv = verify_slot_order_recursive(slots[i].child_page_id);
            if (cv.found) return cv;
        }
        // Recurse into rightmost child.
        return verify_slot_order_recursive(get_rightmost_child(p));
    }

    // Helper for verify_chain_completeness: collect all leaf IDs
    // reachable via the tree structure. Detects aliasing (two different
    // interior pointers routing to the same leaf).
    void collect_tree_leaves(PageId page_id, std::set<PageId>& leaves,
                              std::set<PageId>& aliased) const {
        const Page* p = pool_.get(page_id);
        const PageHeader* h = header(p);
        if (h->is_leaf) {
            if (!leaves.insert(page_id).second) {
                aliased.insert(page_id);
            }
            return;
        }
        const InteriorSlot* slots = interior_slots(p);
        for (uint16_t i = 0; i < h->key_count; ++i) {
            collect_tree_leaves(slots[i].child_page_id, leaves, aliased);
        }
        collect_tree_leaves(get_rightmost_child(p), leaves, aliased);
    }

    // Returns the (min, max) key in a subtree, and checks separator invariants.
    // `lo_bound` and `hi_bound` are the bounds the parent expects this subtree
    // to satisfy: all keys should be in [lo_bound, hi_bound). Empty string =
    // no bound (the root has no parent bounds).
    SeparatorViolation verify_separators_recursive(PageId page_id,
                                                     const std::string& lo_bound,
                                                     const std::string& hi_bound) const {
        const Page* p = pool_.get(page_id);
        const PageHeader* h = header(p);

        if (h->is_leaf) {
            // Leaf: collect min and max key, check against bounds.
            if (h->key_count == 0) return {};
            const LeafSlot* slots = leaf_slots(p);
            std::string min_k(reinterpret_cast<const char*>(p) + slots[0].key_off,
                                slots[0].key_len);
            std::string max_k(reinterpret_cast<const char*>(p) + slots[h->key_count - 1].key_off,
                                slots[h->key_count - 1].key_len);
            // Check bounds.
            if (!lo_bound.empty() && min_k < lo_bound) {
                SeparatorViolation v;
                v.found = true;
                v.interior_page = page_id;
                v.expected_lower = lo_bound;
                v.actual_min = min_k;
                v.reason = "leaf min key " + min_k + " < parent's lower bound " + lo_bound;
                return v;
            }
            if (!hi_bound.empty() && max_k >= hi_bound) {
                SeparatorViolation v;
                v.found = true;
                v.interior_page = page_id;
                v.expected_upper = hi_bound;
                v.actual_max = max_k;
                v.reason = "leaf max key " + max_k + " >= parent's upper bound " + hi_bound;
                return v;
            }
            return {};
        }

        // Interior node: check each child's bounds.
        const InteriorSlot* slots = interior_slots(p);
        std::string prev_key;  // the key to the LEFT of the current child

        for (uint16_t i = 0; i <= h->key_count; ++i) {
            PageId child_id;
            std::string child_lo, child_hi;

            if (i == 0) {
                // Leftmost child: keys < slots[0].key
                child_id = slots[0].child_page_id;
                child_lo = lo_bound;  // inherit from parent
                child_hi = std::string(
                    reinterpret_cast<const char*>(p) + slots[0].key_off,
                    slots[0].key_len);
            } else if (i == h->key_count) {
                // Rightmost child: keys >= slots[n-1].key
                child_id = get_rightmost_child(p);
                child_lo = prev_key;
                child_hi = hi_bound;  // inherit from parent
            } else {
                // Middle child: keys in [slots[i-1].key, slots[i].key)
                child_id = slots[i].child_page_id;
                child_lo = prev_key;
                child_hi = std::string(
                    reinterpret_cast<const char*>(p) + slots[i].key_off,
                    slots[i].key_len);
            }

            // Recurse into the child.
            SeparatorViolation v = verify_separators_recursive(child_id, child_lo, child_hi);
            if (v.found) return v;

            // Update prev_key for the next iteration.
            if (i < h->key_count) {
                prev_key.assign(
                    reinterpret_cast<const char*>(p) + slots[i].key_off,
                    slots[i].key_len);
            }
        }

        return {};
    }

public:
    // (public methods above)

private:
    PagePool& pool_;
    // v25.1 M1.6: root_id_ is atomic because it's read by concurrent
    // get/find_leaf/cursor traversals while put may reassign it during a
    // root split. Relaxed loads on read paths (the latch on the root page
    // provides the happens-before for the page contents; the root_id_ itself
    // just needs to be a consistent snapshot). Relaxed store on root split
    // (the root split path is serialized by nm_ in ensure_index's slow path,
    // so only one writer can split the root at a time).
    std::atomic<PageId> root_id_;

    // v25.1 M1.4: per-page latch table for concurrent reader/writer safety.
    LatchTable latches_;

    // v25.1 M1.3/M1.4: generation counter. Incremented on every mutating
    // operation (put/erase). M1.4 makes it atomic — concurrent writers
    // increment while cursors read. The cursor captures the generation
    // at open time as a debug reference point, but does NOT assert it's
    // unchanged (M1.4 allows concurrent writes to other pages). The
    // per-page latch is the actual consistency mechanism.
    std::atomic<uint64_t> generation_{0};
    // v25.1 M1.6: debug counter for fence re-check retries (concurrent-split detection).
    // Public so ChronoKV/Database can read it for test verification.
    mutable std::atomic<uint64_t> fence_recheck_retries_{0};
    // v25.1 M1.6: counter for ensure_index loser-delete branch (double-allocation race).
    mutable std::atomic<uint64_t> ensure_index_loser_deletes_{0};

    // ---- Helpers for page access ----

    static PageHeader* header(Page* p) {
        return reinterpret_cast<PageHeader*>(p->bytes);
    }
    static const PageHeader* header(const Page* p) {
        return reinterpret_cast<const PageHeader*>(p->bytes);
    }

    static LeafSlot* leaf_slots(Page* p) {
        return reinterpret_cast<LeafSlot*>(p->bytes + sizeof(PageHeader));
    }
    static const LeafSlot* leaf_slots(const Page* p) {
        return reinterpret_cast<const LeafSlot*>(p->bytes + sizeof(PageHeader));
    }

    static InteriorSlot* interior_slots(Page* p) {
        return reinterpret_cast<InteriorSlot*>(p->bytes + sizeof(PageHeader));
    }
    static const InteriorSlot* interior_slots(const Page* p) {
        return reinterpret_cast<const InteriorSlot*>(p->bytes + sizeof(PageHeader));
    }

    // ---- Type-checked field accessors ----
    //
    // These enforce the leaf-vs-interior distinction at every call site.
    // Writing next_leaf_id on an interior page, or rightmost_child_id on
    // a leaf page, is a programming error that these accessors catch via
    // assert(). In release builds the assert is stripped but the field
    // remains distinct — no silent corruption.
    //
    // (v23 E7 precedent: this project has already learned that overloading
    //  one slot for two different values is a recurring bug source. The
    //  fix is always: separate the fields and add type-checked access.)

    static PageId get_next_leaf(const Page* p) {
        const PageHeader* h = header(p);
        assert(h->is_leaf && "get_next_leaf called on interior page");
        return h->next_leaf_id;
    }
    static void set_next_leaf(Page* p, PageId id) {
        PageHeader* h = header(p);
        assert(h->is_leaf && "set_next_leaf called on interior page");
        h->next_leaf_id = id;
    }
    static PageId get_rightmost_child(const Page* p) {
        const PageHeader* h = header(p);
        assert(!h->is_leaf && "get_rightmost_child called on leaf page");
        return h->rightmost_child_id;
    }
    static void set_rightmost_child(Page* p, PageId id) {
        PageHeader* h = header(p);
        assert(!h->is_leaf && "set_rightmost_child called on leaf page");
        h->rightmost_child_id = id;
    }

    // v25.1 M1.2: verify that the header's fence offsets point to the
    // actual first and last slot keys. Returns false if the fence is
    // stale (the offsets don't match the slot data). Called from
    // range_scan's assertion to catch fence-update omissions at the
    // moment a stale fence is read, not when a scan happens to expose
    // a wrong result.
    static bool verify_fence_consistency(const Page* p) {
        const PageHeader* h = header(p);
        if (!h->is_leaf) return true;  // interior pages don't use these fences
        if (h->key_count == 0) {
            return h->min_key_len == 0 && h->max_key_len == 0;
        }
        const LeafSlot* slots = leaf_slots(p);
        // min_key fence should match slots[0]
        if (h->min_key_off != slots[0].key_off) return false;
        if (h->min_key_len != slots[0].key_len) return false;
        // max_key fence should match slots[key_count-1]
        if (h->max_key_off != slots[h->key_count - 1].key_off) return false;
        if (h->max_key_len != slots[h->key_count - 1].key_len) return false;
        return true;
    }

    // Free-space boundaries.
    // free_lo = end of slot array (forward-growing)
    // free_hi = top of slab (backward-growing) = lowest live slab byte
    // Free space = [free_lo, free_hi)
    static uint16_t free_lo(const Page* p) {
        const PageHeader* h = header(p);
        return sizeof(PageHeader) + h->key_count * sizeof(LeafSlot);  // or InteriorSlot
    }
    static uint16_t free_hi(const Page* p) {
        const PageHeader* h = header(p);
        if (h->is_leaf) {
            // For leaves: free_hi = lowest used slab byte.
            // We need to compute this by scanning slots.
            uint16_t hi = PAGE_SIZE;
            const LeafSlot* slots = leaf_slots(p);
            for (uint16_t i = 0; i < h->key_count; ++i) {
                if (slots[i].key_off < hi) hi = slots[i].key_off;
                if (slots[i].value_off < hi) hi = slots[i].value_off;
            }
            return hi;
        } else {
            uint16_t hi = PAGE_SIZE;
            const InteriorSlot* slots = interior_slots(p);
            for (uint16_t i = 0; i < h->key_count; ++i) {
                if (slots[i].key_off < hi) hi = slots[i].key_off;
            }
            return hi;
        }
    }

    // Update the min/max key fences in the header (for fence-based pruning).
    void update_fences_leaf(Page* p) {
        PageHeader* h = header(p);
        LeafSlot* slots = leaf_slots(p);
        if (h->key_count == 0) {
            h->min_key_off = h->max_key_off = 0;
            h->min_key_len = h->max_key_len = 0;
            return;
        }
        h->min_key_off = slots[0].key_off;
        h->min_key_len = slots[0].key_len;
        h->max_key_off = slots[h->key_count - 1].key_off;
        h->max_key_len = slots[h->key_count - 1].key_len;
    }

    void update_fences_interior(Page* p) {
        PageHeader* h = header(p);
        InteriorSlot* slots = interior_slots(p);
        if (h->key_count == 0) {
            h->min_key_off = h->max_key_off = 0;
            h->min_key_len = h->max_key_len = 0;
            return;
        }
        h->min_key_off = slots[0].key_off;
        h->min_key_len = slots[0].key_len;
        h->max_key_off = slots[h->key_count - 1].key_off;
        h->max_key_len = slots[h->key_count - 1].key_len;
    }

    // Compact a leaf: slide all live slab bytes down to remove holes.
    // After compaction, the slab occupies a contiguous range [free_hi, PAGE_SIZE).
    void compact_leaf(Page* p) {
        PageHeader* h = header(p);
        LeafSlot* slots = leaf_slots(p);

        // Collect (src_off, len, dst_off) tuples for keys and values.
        // Sort by src_off to determine the new contiguous layout.
        struct Region { uint16_t src_off, len, slot_idx; bool is_value; };
        std::vector<Region> regions;
        for (uint16_t i = 0; i < h->key_count; ++i) {
            regions.push_back({slots[i].key_off, slots[i].key_len, i, false});
            regions.push_back({slots[i].value_off, slots[i].value_len, i, true});
        }
        std::sort(regions.begin(), regions.end(),
                  [](const Region& a, const Region& b) { return a.src_off < b.src_off; });

        // New layout: pack from the top of the page downward.
        uint16_t new_top = PAGE_SIZE;
        // Use a temporary buffer to avoid overwriting during the move.
        std::vector<uint8_t> temp(PAGE_SIZE);
        for (auto& r : regions) {
            new_top -= r.len;
            std::memcpy(temp.data() + new_top, p->bytes + r.src_off, r.len);
            if (r.is_value) {
                slots[r.slot_idx].value_off = new_top;
            } else {
                slots[r.slot_idx].key_off = new_top;
            }
        }
        // Copy the compacted slab back into the page.
        if (new_top < PAGE_SIZE) {
            std::memcpy(p->bytes + new_top, temp.data() + new_top, PAGE_SIZE - new_top);
        }
        // v25.1 M1.2 FIX: update the fence offsets after compaction.
        // compaction moves key/value bytes to new slab positions and
        // updates slots[i].key_off/value_off, but the header's
        // min_key_off/max_key_off still point to the OLD positions.
        // Without this call, range_scan's fence pruning reads stale
        // data and makes incorrect break decisions — the exact bug
        // that caused the dual-chain-divergence symptom.
        update_fences_leaf(p);
    }

    // Result of a put operation: whether the child split, and if so,
    // the split key and new child page to insert into the parent.
    struct InsertResult {
        bool inserted;       // true = new key inserted, false = existing key updated
        bool split;          // true = child split, parent must insert split_key/new_child
        std::string split_key;
        PageId new_child;
    };

    // v25.1 M1.6: Optimistic latch crabbing for the write path.
    //
    // Descend with SHARED latches only (crabbing: hold parent shared until
    // child shared acquired, then release parent). This avoids the deadlock
    // that exclusive descent caused with the GC's Cursor (both crab down
    // with shared latches → no writer holds exclusive during descent → no
    // cycle).
    //
    // At the leaf: release shared, acquire EXCLUSIVE, re-check fence (the
    // leaf may have been split by a concurrent writer between shared-release
    // and exclusive-acquire, moving our key to a sibling). If the fence
    // check fails, re-descend and retry.
    //
    // Split propagation: after splitting a leaf, release its exclusive latch,
    // re-acquire the PARENT's exclusive with its own fence re-check, and
    // insert the split key + new child. If the parent's fence check fails
    // (the parent itself was split), re-descend to find the correct parent.
    // This propagates up the chain; root split is handled by put().
    InsertResult put_recursive(PageId page_id, const std::string& key,
                                const std::string& value) {
        // v25.1 M1.6: Optimistic crabbing with fence re-check.
        //
        // Descend with shared latches (crabbing). Capture the leaf's fence
        // (min_key, max_key, key_count) at descent time UNDER the shared latch.
        // Release shared, acquire exclusive. Re-check: if the fence changed,
        // a concurrent writer split/modified the leaf between shared-release
        // and exclusive-acquire → RELEASE the exclusive, re-descend, retry.
        //
        // The release-before-re-descend ordering is what avoids the deadlock:
        // we never hold an exclusive latch while re-descending with shared
        // latches (no hold-and-wait cycle). The retry is a fresh shared-descent
        // from the root, same as the working read path.
        while (true) {
            std::shared_lock<std::shared_mutex> leaf_shared;
            LeafFence desc_fence;
            PageId leaf = find_leaf_crabbing_with_fence(page_id, key, leaf_shared, desc_fence);
            leaf_shared.unlock();

            auto leaf_excl = latches_.lock_exclusive(leaf);
            // Re-check: did the leaf change between shared-release and exclusive-acquire?
            Page* p = pool_.get(leaf);
            PageHeader* h = header(p);
            if (!fence_unchanged(p, h, desc_fence)) {
                // Leaf was modified by a concurrent writer (split or insert).
                // RELEASE the exclusive before re-descending (no hold-and-wait).
                leaf_excl.unlock();
                fence_recheck_retries_.fetch_add(1, std::memory_order_relaxed);
                continue;  // re-descend from root
            }
            // Fence unchanged — no concurrent modification. Proceed with the put.
            InsertResult r = put_leaf_nolatch(leaf, key, value);
            if (r.split) {
                leaf_excl.unlock();
                InsertResult pr = insert_into_parent_optimistic(
                    page_id, r.split_key, r.new_child, leaf);
                if (pr.split) return pr;  // root split — handled by put()
                return {r.inserted, false, "", 0};
            }
            return r;
        }
    }

    // v25.1 M1.6: Check if key falls within the leaf's [min_key, max_key] fence.
    // Empty fence (empty leaf) means the key belongs here.
    bool key_in_leaf_fence(const Page* p, const PageHeader* h, const std::string& key) const {
        if (h->min_key_len == 0) return true;  // empty leaf
        std::string min_k(reinterpret_cast<const char*>(p) + h->min_key_off, h->min_key_len);
        std::string max_k(reinterpret_cast<const char*>(p) + h->max_key_off, h->max_key_len);
        return key >= min_k && key <= max_k;
    }

    // v25.1 M1.6: Insert (split_key, new_child) into the parent of `child`.
    // Re-descend to find the parent, acquire exclusive with fence re-check.
    // Returns {split: true} if the root split (caller handles).
    InsertResult insert_into_parent_optimistic(PageId root,
                                                const std::string& split_key,
                                                PageId new_child,
                                                PageId child) {
        // Re-descend to find the parent of `child`. We descend with shared
        // crabbing, tracking the parent. At the parent, release shared,
        // acquire exclusive, re-check that `child` is still a child of this
        // parent, then insert.
        while (true) {
            // Descend to find the leaf for split_key (the split key defines
            // which leaf the new child pointer goes into). The parent is the
            // interior page that points to `child`.
            std::shared_lock<std::shared_mutex> parent_shared;
            PageId parent = find_parent_crabbing(root, split_key, child, parent_shared);
            if (parent == 0) {
                // `child` is the root — root split needed.
                return {false, true, split_key, new_child};
            }
            parent_shared.unlock();
            auto parent_excl = latches_.lock_exclusive(parent);
            // Re-check: is `child` still a child of `parent`?
            Page* p = pool_.get(parent);
            PageHeader* h = header(p);
            if (!child_is_child_of(p, h, child)) {
                // Parent changed (was split). Re-descend.
                parent_excl.unlock();
                continue;
            }
            // Insert (split_key, new_child) into this parent.
            InsertResult r = insert_into_interior_nolatch(parent, split_key, new_child);
            if (r.split) {
                // Parent itself split — propagate up.
                parent_excl.unlock();
                return insert_into_parent_optimistic(root, r.split_key, r.new_child, parent);
            }
            return {false, false, "", 0};
        }
    }

    // v25.1 M1.6: Find the parent of `child` by descending with shared
    // crabbing. Returns the parent's PageId, or 0 if `child` is the root.
    PageId find_parent_crabbing(PageId root, const std::string& key,
                                 PageId child,
                                 std::shared_lock<std::shared_mutex>& held_latch) const {
        PageId id = root;
        auto cur_latch = latches_.lock_shared(id);
        while (true) {
            const Page* p = pool_.get(id);
            const PageHeader* h = header(p);
            if (h->is_leaf) {
                // `child` is the root (a leaf). No parent.
                return 0;
            }
            // Check if `child` is a direct child of this interior page.
            const InteriorSlot* slots = interior_slots(p);
            bool found = false;
            for (uint16_t i = 0; i < h->key_count; ++i) {
                if (slots[i].child_page_id == child) { found = true; break; }
            }
            if (get_rightmost_child(p) == child) found = true;
            if (found) {
                held_latch = std::move(cur_latch);
                return id;
            }
            // Descend further.
            size_t i = 0;
            while (i < h->key_count) {
                std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                               slots[i].key_len);
                if (key < k) break;
                ++i;
            }
            PageId next_id;
            if (i == h->key_count) {
                next_id = get_rightmost_child(p);
            } else {
                next_id = slots[i].child_page_id;
            }
            auto child_latch = latches_.lock_shared(next_id);
            cur_latch = std::move(child_latch);
            id = next_id;
        }
    }

    // v25.1 M1.6: Check if `child` is a direct child of the interior page.
    bool child_is_child_of(const Page* p, const PageHeader* h, PageId child) const {
        const InteriorSlot* slots = interior_slots(p);
        for (uint16_t i = 0; i < h->key_count; ++i) {
            if (slots[i].child_page_id == child) return true;
        }
        if (get_rightmost_child(p) == child) return true;
        return false;
    }

    // v25.1 M1.6: put_leaf_nolatch — the put_leaf logic WITHOUT latch
    // acquisition. The caller (put_recursive_held) holds the exclusive latch.
    InsertResult put_leaf_nolatch(PageId page_id, const std::string& key, const std::string& value) {
        Page* p = pool_.get(page_id);
        PageHeader* h = header(p);
        LeafSlot* slots = leaf_slots(p);

        // Find existing key (update) or insertion point (insert).
        size_t i = 0;
        while (i < h->key_count) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            if (k == key) {
                // Update.
                // If the new value fits in the existing slot, overwrite in place.
                if (slots[i].value_len >= value.size()) {
                    std::memcpy(p->bytes + slots[i].value_off, value.data(), value.size());
                    slots[i].value_len = value.size();
                    return {false, false, "", 0};
                }
                // Otherwise: write the new value at the top of the slab,
                // leaving the old value's space as a hole (compaction
                // will reclaim it later).
                uint16_t new_off = free_hi(p) - value.size();
                if (new_off >= free_lo(p) + sizeof(LeafSlot) + value.size()) {
                    // Wait — need to recheck: we need space for the new value
                    // AND the existing slot's key. The slot is already there.
                    // Just need space for the new value.
                    if (new_off >= free_lo(p)) {
                        std::memcpy(p->bytes + new_off, value.data(), value.size());
                        slots[i].value_off = new_off;
                        slots[i].value_len = value.size();
                        return {false, false, "", 0};
                    }
                }
                // Not enough space even for just the new value — try compact then split.
                compact_leaf(p);
                new_off = free_hi(p) - value.size();
                if (new_off >= free_lo(p)) {
                    std::memcpy(p->bytes + new_off, value.data(), value.size());
                    slots[i].value_off = new_off;
                    slots[i].value_len = value.size();
                    return {false, false, "", 0};
                }
                // Still not enough — split.
                return split_leaf(page_id, key, value, /*update=*/true, /*update_idx=*/i);
            }
            if (k > key) break;
            ++i;
        }

        // Insert at position i.
        // Need space for: 1 new slot (sizeof(LeafSlot)) + key.size() + value.size().
        size_t needed = sizeof(LeafSlot) + key.size() + value.size();
        size_t lo = free_lo(p);
        size_t hi = free_hi(p);
        if (lo + needed > hi) {
            // Not enough contiguous free space — compact first.
            compact_leaf(p);
            lo = free_lo(p);
            hi = free_hi(p);
            if (lo + needed > hi) {
                // Still not enough after compaction — split.
                return split_leaf(page_id, key, value, /*update=*/false, /*update_idx=*/0);
            }
        }

        // Shift slots to make room at position i.
        if (i < h->key_count) {
            std::memmove(&slots[i + 1], &slots[i],
                          (h->key_count - i) * sizeof(LeafSlot));
        }
        // Write key and value into the slab (from the top, backward).
        uint16_t value_off = static_cast<uint16_t>(hi) - value.size();
        uint16_t key_off = value_off - key.size();
        std::memcpy(p->bytes + value_off, value.data(), value.size());
        std::memcpy(p->bytes + key_off, key.data(), key.size());
        slots[i].key_off = key_off;
        slots[i].key_len = key.size();
        slots[i].value_off = value_off;
        slots[i].value_len = value.size();
        h->key_count++;
        update_fences_leaf(p);
        return {true, false, "", 0};
    }

    // Split a leaf page. Collects all entries (including the new one),
    // sorts, splits in half, writes two pages, returns the median key
    // and new page id for the parent to insert.
    InsertResult split_leaf(PageId page_id, const std::string& new_key,
                             const std::string& new_value,
                             bool update, size_t update_idx) {
        Page* p = pool_.get(page_id);
        PageHeader* h = header(p);
        LeafSlot* slots = leaf_slots(p);

        // Collect all entries.
        std::vector<std::pair<std::string, std::string>> entries;
        for (uint16_t i = 0; i < h->key_count; ++i) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            std::string v(reinterpret_cast<const char*>(p) + slots[i].value_off,
                           slots[i].value_len);
            if (update && i == update_idx) {
                // Replace with the new value.
                entries.emplace_back(std::move(k), new_value);
            } else {
                entries.emplace_back(std::move(k), std::move(v));
            }
        }
        if (!update) {
            entries.emplace_back(new_key, new_value);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        // Split into two halves.
        size_t mid = entries.size() / 2;

        // Save the old leaf's next_leaf_id BEFORE clearing the page.
        // The new leaf must inherit this pointer to maintain the chain.
        // (This was the split relink-order bug: the original code cleared
        //  h->next_leaf_id at line 544, then tried to read it at line 557
        //  to inherit into the new page — always getting 0.)
        PageId old_next = get_next_leaf(p);

        // Rewrite the original page with the first half.
        std::memset(p->bytes, 0, PAGE_SIZE);
        h->is_leaf = 1;
        h->key_count = 0;
        h->min_key_off = h->max_key_off = 0;
        h->min_key_len = h->max_key_len = 0;
        // Note: set_next_leaf(p, ...) will be called after the slots are
        // written, because the page was just memset to 0 and set_next_leaf
        // asserts is_leaf — is_leaf is already set above, so we can call
        // it now. But we need to set it to old_next ONLY after the new
        // page is created and linked. For now, set it to 0; it'll be
        // updated below to point to new_page_id.
        set_next_leaf(p, 0);
        for (size_t i = 0; i < mid; ++i) {
            insert_into_leaf_no_split(page_id, entries[i].first, entries[i].second);
        }
        // Allocate a new page for the second half.
        PageId new_page_id = pool_.alloc();
        Page* new_p = pool_.get(new_page_id);
        std::memset(new_p->bytes, 0, PAGE_SIZE);
        PageHeader* new_h = header(new_p);
        new_h->is_leaf = 1;
        new_h->key_count = 0;
        new_h->min_key_off = new_h->max_key_off = 0;
        new_h->min_key_len = new_h->max_key_len = 0;
        // New leaf inherits the old next pointer — this is the critical
        // relink step that was broken before.
        set_next_leaf(new_p, old_next);
        for (size_t i = mid; i < entries.size(); ++i) {
            insert_into_leaf_no_split(new_page_id, entries[i].first, entries[i].second);
        }
        // Link the original page to the new page.
        set_next_leaf(p, new_page_id);

        return {update ? false : true, true, entries[mid].first, new_page_id};
    }

    void insert_into_leaf_no_split(PageId page_id, const std::string& key, const std::string& value) {
        Page* p = pool_.get(page_id);
        PageHeader* h = header(p);
        LeafSlot* slots = leaf_slots(p);

        size_t i = 0;
        while (i < h->key_count) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            if (k > key) break;
            ++i;
        }
        if (i < h->key_count) {
            std::memmove(&slots[i + 1], &slots[i],
                          (h->key_count - i) * sizeof(LeafSlot));
        }
        uint16_t value_off = static_cast<uint16_t>(free_hi(p)) - value.size();
        uint16_t key_off = value_off - key.size();
        std::memcpy(p->bytes + value_off, value.data(), value.size());
        std::memcpy(p->bytes + key_off, key.data(), key.size());
        slots[i].key_off = key_off;
        slots[i].key_len = key.size();
        slots[i].value_off = value_off;
        slots[i].value_len = value.size();
        h->key_count++;
        update_fences_leaf(p);
    }

    // Insert (split_key, new_child) into an interior page. May recursively split.
    // v25.1 M1.6: insert_into_interior_nolatch — the insert logic WITHOUT
    // latch acquisition. The caller (put_recursive_held) holds the exclusive latch.
    InsertResult insert_into_interior_nolatch(PageId page_id, const std::string& split_key,
                                        PageId new_child) {
        Page* p = pool_.get(page_id);
        PageHeader* h = header(p);
        InteriorSlot* slots = interior_slots(p);

        // Find insertion point.
        size_t i = 0;
        while (i < h->key_count) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            if (split_key < k) break;
            ++i;
        }

        // Check space.
        size_t needed = sizeof(InteriorSlot) + split_key.size();
        size_t lo = sizeof(PageHeader) + (h->key_count + 1) * sizeof(InteriorSlot);
        // For interior pages, slab is just the keys.
        uint16_t hi = PAGE_SIZE;
        for (uint16_t j = 0; j < h->key_count; ++j) {
            if (slots[j].key_off < hi) hi = slots[j].key_off;
        }
        if (lo + needed > hi) {
            // Interior page is full — split it.
            return split_interior(page_id, split_key, new_child, i);
        }

        // Shift slots to make room.
        if (i < h->key_count) {
            std::memmove(&slots[i + 1], &slots[i],
                          (h->key_count - i) * sizeof(InteriorSlot));
        }
        uint16_t key_off = static_cast<uint16_t>(hi) - split_key.size();
        std::memcpy(p->bytes + key_off, split_key.data(), split_key.size());
        slots[i].key_off = key_off;
        slots[i].key_len = split_key.size();
        // The new child goes to the RIGHT of the split_key. The existing
        // child at position i (now shifted to i+1) was the right child of
        // the old slots[i].key, which is now still slots[i+1].key... wait,
        // this needs thought.
        //
        // Interior page layout: slots[i] = (key_i, child_i) where child_i
        // is the LEFT child of key_i. The rightmost child is in get_rightmost_child(p).
        //
        // When we insert (split_key, new_child) at position i:
        //   - new_child is the right half of what was slots[i].child (or rightmost).
        //   - The new slot goes at position i.
        //   - The old child at position i (slots[i].child before the shift,
        //     now slots[i+1].child after the shift) remains the left child
        //     of the OLD slots[i].key.
        //
        // Wait, this is getting confusing. Let me think again.
        //
        // Before insert: slots[0..n-1], rightmost child = get_rightmost_child(p).
        //   child_0 holds keys < slots[0].key
        //   child_i holds slots[i-1].key <= keys < slots[i].key
        //   rightmost holds keys >= slots[n-1].key
        //
        // After child split at position i (child_i split into left+right):
        //   The new split_key is the smallest key in the right half.
        //   We need to insert (split_key, new_child) such that:
        //     - new_child holds keys >= split_key (the right half)
        //     - the old child_i holds keys < split_key (the left half)
        //
        // In the interior layout, this means:
        //   - The new slot at position i has key = split_key and child = child_i (the left half, unchanged).
        //   - The slot at position i+1 (the old slot i, shifted right) has its child updated to new_child.
        //   - Wait, no. The new slot at position i has key = split_key. Its child is the LEFT child of split_key, which is the old child_i (left half). The slot at position i+1 (the old slot i, now shifted) has its own key and child; its child should now be new_child (right half).
        //
        // Hmm, this is wrong. Let me re-think the layout.
        //
        // Standard B+ tree interior layout:
        //   slots[0..n-1]: each slot has (key, left_child)
        //   rightmost_child: stored separately (in get_rightmost_child(p) for us)
        //
        // When child_i (which is slots[i].left_child) splits:
        //   - split_key is the smallest key in the new right half.
        //   - new_child is the right half.
        //   - We insert (split_key, new_child) at position i+1 (AFTER the current slot i).
        //   - The old slots[i].left_child remains the left half (no change).
        //
        // Wait, I think the standard layout is:
        //   slots[i] = (key_i, child_i)
        //   where child_i is the child to the LEFT of key_i (keys < key_i).
        //   The rightmost child (keys >= key_{n-1}) is stored separately.
        //
        // When child_i splits, the split produces (split_key, new_child):
        //   - new_child holds keys >= split_key.
        //   - child_i (the old one) now holds keys < split_key.
        //   - We need to insert (split_key, ???) into the parent.
        //
        // The slot we insert has key = split_key. Its left child is child_i
        // (the left half, which is already there). The right child of
        // split_key is new_child.
        //
        // In the standard layout, the right child of slots[i].key is slots[i+1].child.
        // So when we insert (split_key, child_i) at position i+1, the old slots[i].child
        // (which was the right neighbor's left child) gets shifted to slots[i+2].child,
        // and we need slots[i+1].child = new_child.
        //
        // Hmm, this is getting complicated. Let me simplify by using a different
        // convention: each interior slot stores (key, child) where child is the
        // RIGHT child of key (keys >= key go to child). The leftmost child
        // (keys < slots[0].key) is stored in a separate field.
        //
        // No, let me just use the standard left-child convention and be careful.
        // Let me rewrite this section.

        // CORRECTED INSERT LOGIC:
        //
        // Convention: slots[i] = (key_i, child_i) where child_i is the LEFT child of key_i.
        //   - child_0 holds keys < key_0
        //   - child_i (for i > 0) holds keys in [key_{i-1}, key_i)
        //   - rightmost child (get_rightmost_child(p)) holds keys >= key_{n-1}
        //
        // When the child at position i splits (producing split_key, new_child):
        //   - If i < h->key_count: the splitting child is slots[i].child_i.
        //     We insert (split_key, new_child) at position i+1.
        //     The old child_i remains as slots[i].child (left half).
        //     The new slot at i+1 has key = split_key, child = new_child.
        //   - If i == h->key_count: the splitting child is get_rightmost_child(p) (rightmost).
        //     We append (split_key, old_rightmost) at position h->key_count,
        //     and set_rightmost_child(p, new_child).

        if (i == h->key_count) {
            // Splitting the rightmost child.
            // New slot at position i: key = split_key, child = old rightmost.
            slots[i].key_off = key_off;
            slots[i].key_len = split_key.size();
            slots[i].child_page_id = get_rightmost_child(p);  // old rightmost
            set_rightmost_child(p, new_child);  // new rightmost
            h->key_count++;
        } else {
            // Splitting a non-rightmost child at position i.
            // We need to insert (split_key, new_child) at position i.
            // But wait — the splitting child is slots[i].child_i, which is the
            // LEFT child of slots[i].key. After split:
            //   - slots[i].child_i stays as the left half (keys < split_key).
            //   - new_child is the right half (keys >= split_key).
            //   - split_key is the smallest key in new_child.
            //
            // So we insert a NEW slot at position i:
            //   new_slots[i] = (split_key, child_i_left_half)
            //   ... but child_i_left_half IS the old slots[i].child_i (unchanged).
            //   new_slots[i+1] = (old_slots[i].key, new_child)
            //
            // No wait — that's not right either. The old slots[i].key was the
            // fence for the OLD child_i. After split, the old child_i holds
            // keys < split_key, and new_child holds keys >= split_key but < old_slots[i].key.
            //
            // So the new layout is:
            //   new_slots[i] = (split_key, old_child_i)   ← old_child_i holds keys < split_key
            //   new_slots[i+1] = (old_slots[i].key, new_child)  ← new_child holds [split_key, old_key)
            //
            // This means: insert (split_key, old_child_i) at position i, and
            // change slots[i+1].child to new_child (after the shift, slots[i+1]
            // is the old slots[i]).

            // First, do the shift (already done above with memmove).
            // Now slots[i] is the new empty slot (to be filled),
            // and slots[i+1] is the old slots[i] (shifted right).

            // The old child at slots[i] (now slots[i+1] after shift) needs to
            // be replaced with new_child.
            PageId old_child = slots[i + 1].child_page_id;  // the original slots[i].child
            // Wait — after the memmove, slots[i+1] = old slots[i]. So slots[i+1].child
            // is the OLD child that split. We need slots[i+1].child = new_child
            // (the right half), and slots[i] = (split_key, old_child) (the left half).

            slots[i].key_off = key_off;
            slots[i].key_len = split_key.size();
            slots[i].child_page_id = old_child;  // left half
            slots[i + 1].child_page_id = new_child;  // right half
            h->key_count++;
        }
        update_fences_interior(p);
        return {false, false, "", 0};  // inserted into interior, no further split
    }

    InsertResult split_interior(PageId page_id, const std::string& new_key,
                                   PageId new_child, size_t insert_pos) {
        Page* p = pool_.get(page_id);
        PageHeader* h = header(p);
        InteriorSlot* slots = interior_slots(p);

        // Collect all (key, child) pairs.
        struct Entry { std::string key; PageId child; };
        std::vector<Entry> entries;
        for (uint16_t i = 0; i < h->key_count; ++i) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            entries.push_back({std::move(k), slots[i].child_page_id});
        }
        PageId rightmost = get_rightmost_child(p);

        // Insert the new entry at the right position.
        // Convention: entries[i] = (key_i, child_i) where child_i is the LEFT child of key_i.
        // When child at position insert_pos splits (producing new_key, new_child):
        //   - The old child (entries[insert_pos].child) stays as the LEFT half.
        //   - new_child is the RIGHT half.
        //   - We insert (new_key, old_child) at position insert_pos.
        //   - The old entry at insert_pos (now shifted to insert_pos+1) has its child
        //     updated to new_child.
        //
        // For the rightmost case (insert_pos == entries.size()):
        //   - The splitting child is the rightmost (stored in `rightmost`).
        //   - We append (new_key, old_rightmost) at the end.
        //   - rightmost becomes new_child.
        Entry new_entry{new_key, new_child};
        if (insert_pos >= entries.size()) {
            // Inserting at the end — the new child becomes the new rightmost,
            // and the old rightmost becomes the child of new_key.
            entries.push_back({new_key, rightmost});
            rightmost = new_child;
        } else {
            // Inserting in the middle. The old child at insert_pos becomes
            // the LEFT child of new_key; new_child becomes the LEFT child
            // of the old key (now at insert_pos+1).
            PageId old_child = entries[insert_pos].child;
            new_entry.child = old_child;  // (new_key, old_child = left half)
            entries.insert(entries.begin() + insert_pos, new_entry);
            entries[insert_pos + 1].child = new_child;  // right half
        }

        // Split entries into two halves.
        size_t mid = entries.size() / 2;
        std::string median_key = entries[mid].key;

        // Rewrite the original page with the first half.
        std::memset(p->bytes, 0, PAGE_SIZE);
        h->is_leaf = 0;
        h->key_count = 0;
        set_rightmost_child(p, 0);
        for (size_t i = 0; i < mid; ++i) {
            insert_into_interior_no_split(page_id, entries[i].key, entries[i].child);
        }
        // The median key goes up to the parent; the child at position mid
        // becomes the rightmost child of the left half.
        set_rightmost_child(p, entries[mid].child);

        // Allocate a new page for the second half (entries[mid+1..]).
        PageId new_page_id = pool_.alloc();
        Page* new_p = pool_.get(new_page_id);
        std::memset(new_p->bytes, 0, PAGE_SIZE);
        PageHeader* new_h = header(new_p);
        new_h->is_leaf = 0;
        new_h->key_count = 0;
        set_rightmost_child(new_p, rightmost);
        for (size_t i = mid + 1; i < entries.size(); ++i) {
            insert_into_interior_no_split(new_page_id, entries[i].key, entries[i].child);
        }

        return {false, true, median_key, new_page_id};
    }

    void insert_into_interior_no_split(PageId page_id, const std::string& key, PageId child) {
        Page* p = pool_.get(page_id);
        PageHeader* h = header(p);
        InteriorSlot* slots = interior_slots(p);

        size_t i = 0;
        while (i < h->key_count) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            if (key < k) break;
            ++i;
        }
        if (i < h->key_count) {
            std::memmove(&slots[i + 1], &slots[i],
                          (h->key_count - i) * sizeof(InteriorSlot));
        }
        uint16_t key_off = PAGE_SIZE;
        for (uint16_t j = 0; j < h->key_count + 1; ++j) {
            uint16_t off = (j < h->key_count) ? slots[j].key_off : 0;
            // Find the lowest used slab byte.
        }
        // Simplified: just put the key at the top of the slab.
        // Find current top of slab.
        uint16_t slab_top = PAGE_SIZE;
        for (uint16_t j = 0; j < h->key_count; ++j) {
            if (slots[j].key_off < slab_top) slab_top = slots[j].key_off;
        }
        // After the memmove, slots[i] is the new empty slot.
        // The slot at i+1 (if exists) is the old slot i.
        // Recompute slab_top after the shift (slots shifted, but offsets unchanged).
        uint16_t key_off_new = slab_top - key.size();
        std::memcpy(p->bytes + key_off_new, key.data(), key.size());
        slots[i].key_off = key_off_new;
        slots[i].key_len = key.size();
        slots[i].child_page_id = child;
        h->key_count++;
        update_fences_interior(p);
    }

    // ---- Find the leaf page that would contain `key`. ----
    // v25.1 M1.6: find_leaf_crabbing does proper latch crabbing (hold parent
    // shared latch until child's is acquired). This is the correct concurrent
    // descent. Used by get_recursive, put_recursive, erase_recursive.
    //
    // find_leaf (non-crabbing) is kept for the Cursor's use — the Cursor has
    // its own M1.4 safety argument for split-during-cursor (per-page snapshot
    // via shared latch acquired in enter_leaf, next_leaf_id read under the
    // shared latch before release). The Cursor's race window between
    // find_leaf_for_scan returning and enter_leaf acquiring the latch is
    // closed by the Cursor's generation check + the fact that splits never
    // retire page IDs (the original page is shrunk in place). TSan confirms
    // this is race-free in the concurrent cursor test.
    PageId find_leaf(PageId root, const std::string& key) const {
        PageId id = root;
        while (true) {
            const Page* p = pool_.get(id);
            const PageHeader* h = header(p);
            if (h->is_leaf) return id;
            const InteriorSlot* slots = interior_slots(p);
            size_t i = 0;
            while (i < h->key_count) {
                std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                               slots[i].key_len);
                if (key < k) break;
                ++i;
            }
            if (i == h->key_count) {
                id = get_rightmost_child(p);
            } else {
                id = slots[i].child_page_id;
            }
        }
    }
    // v25.1 M1.6: latch crabbing — hold parent's shared latch until child's
    // shared latch is acquired, then release the parent. This closes the
    // race window where a concurrent writer could split the child between
    // parent-latch-release and child-latch-acquire. Returns the leaf ID
    // AND the shared latch (caller must hold it while reading the leaf).
    // The latch is returned via the out-parameter `held_latch`.
    // v25.1 M1.6: find_leaf_crabbing + capture the leaf's fence at descent
    // time. The fence (min_key, max_key, key_count) is read UNDER the shared
    // latch, so it's a consistent snapshot. After acquiring exclusive, we
    // compare: if the fence changed, a concurrent writer split/modified the
    // leaf between shared-release and exclusive-acquire → re-descend.
    struct LeafFence {
        std::string min_key;
        std::string max_key;
        uint16_t key_count;
    };
    PageId find_leaf_crabbing_with_fence(PageId root, const std::string& key,
                                          std::shared_lock<std::shared_mutex>& held_latch,
                                          LeafFence& fence) const {
        PageId id = root;
        auto cur_latch = latches_.lock_shared(id);
        while (true) {
            const Page* p = pool_.get(id);
            const PageHeader* h = header(p);
            if (h->is_leaf) {
                held_latch = std::move(cur_latch);
                // Capture fence under the shared latch.
                if (h->min_key_len > 0) {
                    fence.min_key.assign(reinterpret_cast<const char*>(p) + h->min_key_off, h->min_key_len);
                    fence.max_key.assign(reinterpret_cast<const char*>(p) + h->max_key_off, h->max_key_len);
                }
                fence.key_count = h->key_count;
                return id;
            }
            const InteriorSlot* slots = interior_slots(p);
            size_t i = 0;
            while (i < h->key_count) {
                std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                               slots[i].key_len);
                if (key < k) break;
                ++i;
            }
            PageId next_id;
            if (i == h->key_count) {
                next_id = get_rightmost_child(p);
            } else {
                next_id = slots[i].child_page_id;
            }
            auto child_latch = latches_.lock_shared(next_id);
            cur_latch = std::move(child_latch);
            id = next_id;
        }
    }

    // v25.1 M1.6: Compare the leaf's current fence (under exclusive latch)
    // against the fence captured at descent time. If any field changed, a
    // concurrent writer modified this leaf (split or insert) → re-descend.
    bool fence_unchanged(const Page* p, const PageHeader* h, const LeafFence& desc_fence) const {
        if (h->key_count != desc_fence.key_count) return false;
        if (h->min_key_len > 0) {
            std::string cur_min(reinterpret_cast<const char*>(p) + h->min_key_off, h->min_key_len);
            std::string cur_max(reinterpret_cast<const char*>(p) + h->max_key_off, h->max_key_len);
            if (cur_min != desc_fence.min_key || cur_max != desc_fence.max_key) return false;
        } else if (!desc_fence.min_key.empty()) {
            return false;  // was non-empty, now empty
        }
        return true;
    }

    // v25.1 M1.6: original find_leaf_crabbing (without fence capture) — kept
    // for callers that don't need the fence (Cursor's initial descent).
    PageId find_leaf_crabbing(PageId root, const std::string& key,
                              std::shared_lock<std::shared_mutex>& held_latch) const {
        PageId id = root;
        auto cur_latch = latches_.lock_shared(id);
        while (true) {
            const Page* p = pool_.get(id);
            const PageHeader* h = header(p);
            if (h->is_leaf) {
                held_latch = std::move(cur_latch);
                return id;
            }
            const InteriorSlot* slots = interior_slots(p);
            size_t i = 0;
            while (i < h->key_count) {
                std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                               slots[i].key_len);
                if (key < k) break;
                ++i;
            }
            PageId next_id;
            if (i == h->key_count) {
                next_id = get_rightmost_child(p);
            } else {
                next_id = slots[i].child_page_id;
            }
            // CRABBING: acquire child latch BEFORE releasing parent latch.
            auto child_latch = latches_.lock_shared(next_id);
            cur_latch = std::move(child_latch);  // parent latch releases here
            id = next_id;
        }
    }

    bool get_recursive(PageId page_id, const std::string& key, std::string* out_value) const {
        // v25.1 M1.6: crabbing — find_leaf_crabbing returns the leaf ID
        // AND holds the leaf's shared latch. We read the leaf under that latch.
        std::shared_lock<std::shared_mutex> leaf_latch;
        PageId leaf = find_leaf_crabbing(page_id, key, leaf_latch);
        const Page* p = pool_.get(leaf);
        const PageHeader* h = header(p);
        const LeafSlot* slots = leaf_slots(p);
        for (uint16_t i = 0; i < h->key_count; ++i) {
            std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                           slots[i].key_len);
            if (k == key) {
                out_value->assign(reinterpret_cast<const char*>(p->bytes + slots[i].value_off),
                                   slots[i].value_len);
                return true;
            }
        }
        return false;
    }

    // v25.1 M1.6: erase with OPTIMISTIC latch crabbing (same pattern as put).
    // Shared descent, exclusive at leaf with fence re-check, re-descend on
    // fence failure. Erase doesn't cause splits, so no split propagation.
    bool erase_recursive(PageId page_id, const std::string& key) {
        // v25.1 M1.6: Optimistic crabbing with fence re-check (same as put).
        while (true) {
            std::shared_lock<std::shared_mutex> leaf_shared;
            LeafFence desc_fence;
            PageId leaf = find_leaf_crabbing_with_fence(page_id, key, leaf_shared, desc_fence);
            leaf_shared.unlock();

            auto leaf_excl = latches_.lock_exclusive(leaf);
            Page* p = pool_.get(leaf);
            PageHeader* h = header(p);
            if (!fence_unchanged(p, h, desc_fence)) {
                leaf_excl.unlock();
                continue;  // re-descend
            }
            LeafSlot* slots = leaf_slots(p);
            for (uint16_t i = 0; i < h->key_count; ++i) {
                std::string k(reinterpret_cast<const char*>(p) + slots[i].key_off,
                               slots[i].key_len);
                if (k == key) {
                    if (i + 1 < h->key_count) {
                        std::memmove(&slots[i], &slots[i + 1],
                                      (h->key_count - i - 1) * sizeof(LeafSlot));
                    }
                    h->key_count--;
                    update_fences_leaf(p);
                    return true;
                }
            }
            return false;  // key not found
        }
    }

    size_t size_recursive(PageId page_id) const {
        const Page* p = pool_.get(page_id);
        const PageHeader* h = header(p);
        if (h->is_leaf) return h->key_count;
        size_t total = 0;
        const InteriorSlot* slots = interior_slots(p);
        for (uint16_t i = 0; i < h->key_count; ++i) {
            total += size_recursive(slots[i].child_page_id);
        }
        total += size_recursive(get_rightmost_child(p));
        return total;
    }
};

} // namespace chronokv_btree

// v25.1 M1.6: out-of-line definitions of ChronoKV index methods that use
// the B+ tree. Defined here (after BTree is complete) because BTree is
// forward-declared inside ChronoKV's class body.
inline std::pair<KeyEntry*, bool> ChronoKV::ensure_index(const std::string& k) {
    // v25.1 M1.6: Fast path — tree_->get (shared crabbing, no nm_).
    std::string buf;
    if (tree_->get(k, &buf)) return {decode_ptr(buf), false};
    // Slow path: new-key creation. nm_ serializes the check-then-put
    // (prevents double-allocation orphaning). tree_->put is under nm_ —
    // this is correct (no leak) and the put_leaf_gap stress_point was
    // removed from put_recursive (it caused deadlock when nm_ was held
    // during the gap hook). The fence re-check is tested via the
    // concurrent-puts approach (Test 5) without the deterministic hook.
    KeyEntry* e = new KeyEntry();
    // v25.1 M1.6: stress_point for the double-allocation race test.
    // This is OUTSIDE nm_ — the test hooks here to block thread A before
    // it takes nm_. Thread B (which also reaches here) then takes nm_,
    // inserts, and signals thread A. Thread A then takes nm_, sees the
    // existing entry, and deletes its own (loser-delete branch).
    stress_point("ensure_index_put_gap");
    std::unique_lock lk(nm_);
    if (tree_->get(k, &buf)) {
        KeyEntry* winner = decode_ptr(buf);
        if (winner != e) {
            delete e;  // we lost the race, use the winner
            tree_->ensure_index_loser_deletes_.fetch_add(1, std::memory_order_relaxed);
            return {winner, true};
        }
        return {e, true};  // winner == e (already inserted by us — shouldn't happen)
    }
    tree_->put(k, encode_ptr(e));
    return {e, true};
}

inline KeyEntry* ChronoKV::find_index(const std::string& k) const {
    // v25.1 M1.6: no nm_ — tree_->get uses shared latch crabbing internally.
    std::string buf;
    if (!tree_->get(k, &buf)) return nullptr;
    return decode_ptr(buf);
}

inline std::vector<std::pair<std::string, std::string>>
ChronoKV::tree_scan(const std::string& lo, const std::string& hi) const {
    // v25.1 M1.6: no nm_ — Cursor uses shared latch crabbing internally.
    return tree_->range_scan(lo, hi);
}

inline size_t ChronoKV::tree_size() const {
    // v25.1 M1.6: no nm_.
    return tree_->size();
}

// v25.1 M1.6: out-of-line definition (BTree must be complete).
inline uint64_t ChronoKV::fence_recheck_retries() const {
    return tree_->fence_recheck_retries_.load(std::memory_order_relaxed);
}

inline uint64_t ChronoKV::ensure_index_loser_deletes() const {
    return tree_->ensure_index_loser_deletes_.load(std::memory_order_relaxed);
}

namespace chronokv {
inline uint64_t Database::fence_recheck_retries() const {
    check_open();
    return engine_->fence_recheck_retries();
}
inline uint64_t Database::ensure_index_loser_deletes() const {
    check_open();
    return engine_->ensure_index_loser_deletes();
}
} // namespace chronokv

// v25.1 M1.6 (Phase 2): range_scan_stream pImpl + ChronoKV methods.
// ChronoKVRangeScanCursorState holds a BTree::Cursor + SnapshotGuard +
// the cached current (key, value) pair. Defined here (after BTree) because
// it uses BTree::Cursor (incomplete at ChronoKV's class body).
struct ChronoKVRangeScanCursorState {
    SnapshotGuard guard;
    uint64_t effective_ts;
    // v25.1 M1.6: no nm_ — Cursor uses shared latch crabbing internally.
    chronokv_btree::BTree::Cursor cursor;
    bool cached_valid;
    std::string cached_key;
    std::optional<std::string> cached_val;
    ChronoKV& kv;

    ChronoKVRangeScanCursorState(ChronoKV& k, const std::string& lo, const std::string& hi)
        : guard(k), effective_ts(std::min(guard.read_ts(), k.pub_.published())),
          cursor(k.tree_->open_cursor(lo, hi)), cached_valid(false), kv(k) {
        advance();
    }

    void advance() {
        if (cursor.valid()) {
            cached_key = cursor.key();
            KeyEntry* e = ChronoKV::decode_ptr(cursor.value());
            cached_val = kv.read_at_idx(effective_ts, e);
            cached_valid = true;
            cursor.next();
        } else {
            cached_valid = false;
        }
    }
};

inline std::unique_ptr<ChronoKVRangeScanCursorState>
ChronoKV::open_range_scan_stream(const std::string& lo, const std::string& hi) {
    return std::make_unique<ChronoKVRangeScanCursorState>(*this, lo, hi);
}

inline bool ChronoKV::stream_has_next(ChronoKVRangeScanCursorState& s) {
    return s.cached_valid && s.cached_val.has_value();
}

inline std::pair<std::string, std::string>
ChronoKV::stream_next(ChronoKVRangeScanCursorState& s) {
    if (!s.cached_valid || !s.cached_val.has_value())
        throw chronokv::Error("range_scan_stream: no next element");
    auto result = std::make_pair(s.cached_key, *s.cached_val);
    s.advance();
    return result;
}

namespace chronokv {

// v25.1 M1.6 (Phase 2): Database::RangeScanStream out-of-line methods.
inline Database::RangeScanStream::RangeScanStream(Database& db, std::string lo, std::string hi)
    : db_(db) {
    db_.check_open();
    state_ = db_.engine_->open_range_scan_stream(lo, hi);
}

inline Database::RangeScanStream::~RangeScanStream() = default;
inline Database::RangeScanStream::RangeScanStream(RangeScanStream&&) noexcept = default;

inline bool Database::RangeScanStream::has_next() {
    return ChronoKV::stream_has_next(*state_);
}

inline std::pair<std::string, std::string> Database::RangeScanStream::next() {
    return ChronoKV::stream_next(*state_);
}

} // namespace chronokv

namespace chronokv_latency {

// ======================== HDR Histogram ========================
//
// Fixed-range histogram from 1ns to 10s with 2-decimal precision
// (100 buckets per decade). Bucket boundaries are logarithmic:
//   bucket[i] covers [10^(i/100), 10^((i+1)/100)) nanoseconds.
// Total buckets: log10(10s / 1ns) * 100 = log10(1e10) * 100 = 1000.
//
// Memory: 1000 * 8 bytes = 8 KB per histogram. Cheap enough to have
// one per thread-local aggregation point.

class HdrHistogram {
public:
    static constexpr int kBuckets = 1000;  // 1ns to 10s, 2-decimal precision

    HdrHistogram() : counts_{} {}

    // Record a latency in nanoseconds. O(1).
    void record(uint64_t ns) {
        if (ns == 0) ns = 1;  // bucket 0 covers [1ns, 10^(0.01)ns)
        int idx = ns_to_bucket(ns);
        if (idx < 0) idx = 0;
        if (idx >= kBuckets) idx = kBuckets - 1;
        counts_[idx].fetch_add(1, std::memory_order_relaxed);
    }

    // Percentile in microseconds. Linear interpolation within bucket
    // (treating bucket as uniform — slight overestimate, acceptable
    // for tail-latency measurement).
    double percentile_us(double p) const {
        // Sum total.
        uint64_t total = 0;
        for (int i = 0; i < kBuckets; ++i)
            total += counts_[i].load(std::memory_order_relaxed);
        if (total == 0) return 0.0;

        uint64_t target = static_cast<uint64_t>(std::ceil(p * total));
        if (target == 0) target = 1;  // p=0 with count>0 should still find first non-empty bucket
        uint64_t acc = 0;
        for (int i = 0; i < kBuckets; ++i) {
            uint64_t c = counts_[i].load(std::memory_order_relaxed);
            if (acc + c >= target) {
                // Linear interpolation within the bucket.
                double bucket_lo_ns = bucket_to_ns(i);
                double bucket_hi_ns = bucket_to_ns(i + 1);
                double frac = (c > 0) ? static_cast<double>(target - acc) / c : 0.0;
                double ns = bucket_lo_ns + frac * (bucket_hi_ns - bucket_lo_ns);
                return ns / 1000.0;  // ns -> us
            }
            acc += c;
        }
        return bucket_to_ns(kBuckets - 1) / 1000.0;
    }

    uint64_t count() const {
        uint64_t total = 0;
        for (int i = 0; i < kBuckets; ++i)
            total += counts_[i].load(std::memory_order_relaxed);
        return total;
    }

    void reset() {
        for (int i = 0; i < kBuckets; ++i)
            counts_[i].store(0, std::memory_order_relaxed);
    }

    // Merge another histogram into this one (for thread-local aggregation).
    void merge(const HdrHistogram& other) {
        for (int i = 0; i < kBuckets; ++i)
            counts_[i].fetch_add(other.counts_[i].load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    }

    // Snapshot into a target histogram (atomics aren't copyable, so
    // we can't return by value — caller provides the destination).
    void snapshot_into(HdrHistogram& dst) const {
        dst.reset();
        for (int i = 0; i < kBuckets; ++i)
            dst.counts_[i].store(counts_[i].load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    }

private:
    static int ns_to_bucket(uint64_t ns) {
        if (ns <= 1) return 0;
        double lg = std::log10(static_cast<double>(ns));
        return static_cast<int>(lg * 100.0);
    }
    static double bucket_to_ns(int idx) {
        return std::pow(10.0, idx / 100.0);
    }

    std::array<std::atomic<uint64_t>, kBuckets> counts_;
};

// ======================== Thread-local aggregator ========================
//
// Each thread has its own HdrHistogram. Every 100ms (or 10K records,
// whichever comes first), the thread-local histogram is merged into
// the global histogram and reset. This keeps atomics off the hot path.

class ThreadLocalHistogram {
public:
    ThreadLocalHistogram(uint64_t flush_threshold = 10000)
        : flush_threshold_(flush_threshold) {}

    void record(uint64_t ns) {
        local_.record(ns);
        if (++local_count_ >= flush_threshold_) {
            std::lock_guard<std::mutex> lk(global_mu_);
            global_.merge(local_);
            local_.reset();
            local_count_ = 0;
        }
    }

    void snapshot_into(HdrHistogram& dst) const {
        {
            std::lock_guard<std::mutex> lk(global_mu_);
            global_.snapshot_into(dst);
        }
        // Merge local in-flight data (no lock needed; local is thread-local).
        dst.merge(local_);
    }

private:
    HdrHistogram local_;
    HdrHistogram global_;
    mutable std::mutex global_mu_;  // mutable: locked from const snapshot_into()
    uint64_t local_count_ = 0;
    uint64_t flush_threshold_;
};

// ======================== Slow-op log ========================
//
// 100% sampling of ops over a configurable threshold. Writes to a
// rotating file (max 100 MB, rotates to .1, .2, .3, then deletes oldest).

class SlowOpLog {
public:
    SlowOpLog(const std::string& path,
              uint64_t read_threshold_ns = 1'000'000,    // 1 ms
              uint64_t write_threshold_ns = 10'000'000)  // 10 ms
        : path_(path),
          read_threshold_(read_threshold_ns),
          write_threshold_(write_threshold_ns) {
        file_.open(path_, std::ios::app);
    }

    void record_read(const std::string& key, uint64_t ns) {
        if (ns < read_threshold_) return;
        std::lock_guard<std::mutex> lk(mu_);
        file_ << "READ  " << (ns / 1000.0) << "us  key=" << key << "\n";
        file_.flush();
        maybe_rotate();
    }

    void record_write(const std::string& key, uint64_t ns,
                      const std::string& conflict = "") {
        if (ns < write_threshold_) return;
        std::lock_guard<std::mutex> lk(mu_);
        file_ << "WRITE " << (ns / 1000.0) << "us  key=" << key;
        if (!conflict.empty()) file_ << "  conflict=" << conflict;
        file_ << "\n";
        file_.flush();
        maybe_rotate();
    }

    void record_range_scan(const std::string& lo, const std::string& hi,
                            uint64_t ns, size_t result_count) {
        if (ns < read_threshold_) return;
        std::lock_guard<std::mutex> lk(mu_);
        file_ << "SCAN " << (ns / 1000.0) << "us  [" << lo << "," << hi << "]"
              << "  results=" << result_count << "\n";
        file_.flush();
        maybe_rotate();
    }

private:
    void maybe_rotate() {
        // Check file size; rotate at 100 MB.
        std::error_code ec;
        auto sz = std::filesystem::file_size(path_, ec);
        if (ec || sz < 100ULL * 1024 * 1024) return;
        file_.close();
        // Rotate: .3 <- .2 <- .1 <- current, drop .3 if exists.
        for (int i = 3; i >= 1; --i) {
            std::string from = path_ + "." + std::to_string(i);
            std::string to = path_ + "." + std::to_string(i + 1);
            if (i == 3) std::filesystem::remove(from, ec);
            else std::filesystem::rename(from, to, ec);
        }
        std::filesystem::rename(path_, path_ + ".1", ec);
        file_.open(path_, std::ios::app);
    }

    std::string path_;
    std::ofstream file_;
    std::mutex mu_;
    uint64_t read_threshold_;
    uint64_t write_threshold_;
};

// ======================== RAII recorder ========================
//
// Usage: { chronokv_latency::Recorder r(histogram, log, "key", op_type); ... }
// Records the duration on scope exit.

enum class OpType { Read, Write, RangeScan };

class Recorder {
public:
    Recorder(ThreadLocalHistogram& hist, SlowOpLog* log,
             const std::string& key, OpType op,
             const std::string& lo = "", const std::string& hi = "")
        : hist_(hist), log_(log), key_(key), op_(op), lo_(lo), hi_(hi),
          start_(std::chrono::steady_clock::now()) {}

    ~Recorder() {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count();
        hist_.record(ns);
        if (log_) {
            switch (op_) {
                case OpType::Read:
                    log_->record_read(key_, ns);
                    break;
                case OpType::Write:
                    log_->record_write(key_, ns);
                    break;
                case OpType::RangeScan:
                    log_->record_range_scan(lo_, hi_, ns, 0);
                    break;
            }
        }
    }

    // Non-copyable, non-movable.
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

private:
    ThreadLocalHistogram& hist_;
    SlowOpLog* log_;
    std::string key_;
    OpType op_;
    std::string lo_, hi_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace chronokv_latency

