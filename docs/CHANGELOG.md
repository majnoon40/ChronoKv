# ChronoKV changelog

This file exists because `chronokv.hpp` has referenced `docs/CHANGELOG.md`
since the v22 arc while the file was never in-tree (fixed in 0.28.0).
Entries below 0.28.0 are backfilled from `docs/ROADMAP.md` and the header's
version history; 0.28.0 is recorded in full.

## Unreleased

- **docs (v29 M0 CLOSED): the delta confirmation round over 0.28.1 —
  verdict B+ (clean).** `docs/audits/2026-10-delta-confirmation-0.28.1.md`
  records the round the roadmap's M0 gate required: all four hunt-classes
  probed mechanically (20/20 PASS; the probe program is committed beside
  the report and re-runnable — rule 8's spirit applied to audits
  themselves). Class 1 (TXN-1 tracker): bounded under default GC
  (write-only holds entries at 0), long readers correctly pin their
  conflict set and GC prunes to 0 on release, 56 ms commit-validation at
  200k pinned mods (long-reader-borne, inherent to first-updater-wins
  range validation). Class 2 (TXN-2 point ranges): sound across prune
  pressure, net-absent churn, self-insert, empty key, 4048-byte key,
  oversized read-set key. Class 3 (WAL-1): every adversarial construct
  classifies on the loud side — double-damaged interiors CORRUPT, stale
  rollback debris TORN_TAIL (D2 preserved), unreachable-LSN jumps
  TORN_TAIL, 8 MiB worst-case scan 0 ms. Class 4 (BT-1): the
  `5*(height+1)+1` bound re-derived against the real planner and
  stress-probed in three packing regimes the battery did not cover
  (entry-cost just over budget/2, near-maximum, thirds — 84 exhaustion
  combos, zero lost acked keys, observed max 6 pages/put). **One Low
  finding, fixed in this commit:** four comments still described the
  pre-TXN-1 "existence transitions only" contract (`mods_by_ts_`,
  `has_phantom_in_range`, group_append's race-closure note, commit_txn's
  transitions note) — the CKV-017/021 doc-truth class, corrected; lint +
  ratchet + both batteries re-run green after the comment-only edit. The
  report also records the sandbox's environmental full-suite anomaly
  (thread-creation EAGAIN under anomalous kernel limits; three
  independent non-reproductions; CI authoritative) and the build-wall
  datum (bare `-O0` now needs `--param ggc-min-expand=5` on a 1 GiB
  container — verified workaround added to the Makefile note). **M0 is
  CLOSED; the M3+ gate is lifted** (order still governed by the arena
  green week and the catcher-first constraints).

- **ci (v29 M1, rule-13 follow-through): arena caps right-sized from the
  first completed ledger night.** Run #68 (2026-10-08) was the split
  vehicle's first full pass — and the ledger's first complete night:
  `arena-chronokv` **31m55s**, `arena-baselines` **15m33s**, both
  artifacts uploaded, nightly green end-to-end. The baselines' measured
  15m33s shows the pre-split cancellations were budget *allocation* (the
  ChronoKV legs eating 44 of 60 shared minutes), not baseline slowness —
  the killed lower bound was nearly the whole real cost. Per the
  tighten-after-measuring follow-up written into the job comment at
  `e9eb787`: baselines cap 180→60 (~4x headroom, sized to also absorb
  the coming D/E/F adapters); chronokv stays 90 (~2.8x over the faster
  measurement — shared-runner variance is the budget's job). The
  green-week clock (M1 acceptance) is running: night 1/7 banked.

- **docs (audit record — v29 M0 closure items 1 + 3):**
  `docs/audits/2026-10-audit-2-reconstructed.md` commits the Audit-2
  record per rule 8. The original audit session was **not preserved** —
  discovered at M0-closure time; which reviewer ran it, with what prompt
  and what verdict letter, is unrecoverable. Rather than leave the
  battery's finding IDs citing a document that exists nowhere (the exact
  rule-8 failure the round-1 lesson warns about), the record is
  **reconstructed and labeled as such**: findings, mechanisms, PoC results
  and detectors consolidated from the in-tree remediation artifacts (fix
  rationale comments, the 15-check battery, the 0.28.1 changelog entry,
  the header's v28.1 preamble) plus the independent cross-verification log
  (differential PoCs re-run against tag `v0.28.0`: TXN-1 anomalous commit,
  TXN-2 exhaustion at txn 16,554, EXTRA-2 four corrupted keys, API-3 zero
  observer fires — each reproduced pre-fix, closed post-fix). Severity
  classes are assigned-in-reconstruction from impact, not transcribed; the
  document states plainly what it cannot know. All 13 findings carry a
  rule-8 mapping row (finding → detector → commit). **Tag `v0.28.1`**
  rides this commit (rule 2; convention matches `v0.28.0` tagging the
  arc-completion commit). M0 stays open on its remaining item: the delta
  confirmation round over 0.28.1 — the gate on M3+.

- **ci (v29): the four experimental sanitizer legs are PROMOTED to
  gating.** The io_uring-disabled asan/tsan legs — the only config where
  the WAL write-path fault legs execute (`pwrite_all` WriteShort/WriteFail,
  the D2 async-write-stage) — and the clang asan/tsan legs shipped in
  0.28.1 under `experimental: true` with an explicit promotion policy
  ("green once → delete the flag and it gates"). Runs #64, #65 and #66
  made each green three times straight; the flags and both matrices'
  `continue-on-error` are deleted, and all 12 legs (9 engine-tests + 3
  tsan) now gate `ci-passed`. The clang *lint* informational step keeps
  its `continue-on-error` until the four catalogued clang warnings
  (3× `-Wunused-lambda-capture`, 1× `-Wunused-private-field`) are fixed —
  this promotion covers the sanitizer legs only, per the policy's own
  one-green-one-flag mechanics.

- **docs (roadmap rebase):** `docs/ROADMAP.md` is rebaselined against
  `main @ e9eb787` (**0.28.1**) as a new plan — not a revision block: the
  Audit-2 round changed the plan's own gate status (v29 M0 executed,
  closure pending: report in-tree, delta confirmation round, tag
  `v0.28.1`), its evidence base (two independent rounds, 21 + 13 findings,
  both remediated fail-first), and its process rules (11: verdicts are
  round-scoped — TXN-1 shipped under round 1's "verified sound"; 12:
  fixes are cross-verified independently of the fixer — the
  `put_with_old` parity gap; 13: CI vehicles fit their own measured
  budgets — the two dead nightlies; 14: contract changes flip tests,
  never delete them — v17 guard test 4). v29 M1's remaining list is
  reordered around the first completed nightly; M2 gains item 5 (the
  test-suite TU split API-1 unlocked — the 28,016-line TU OOMs 1 GiB at
  `-O1` where 0.28.0 fit); v30 M2/M3 gain the API-1-shipped and
  WAL-2/3-evidence notes. The 2026-09-27 plan is preserved verbatim as
  the appendix with a full disposition table in the new body — nothing
  silently dropped. README Roadmap paragraph updated in the same commit
  (rule 3).

- **ci (v29 M1 step 4, arena split):** the nightly ledger vehicle is now
  TWO parallel jobs — `arena-chronokv` (group + async legs; 90-min timeout
  sized from run #65's measured 44m01s, ~2x headroom) and `arena-baselines`
  (five configs; 180-min timeout deliberately generous — the pre-split
  cancellation gives only a lower bound, "tighten after the first
  completed night publishes real per-config durations" is written into the
  job comment). Two consecutive nightlies died identically (#62 on
  `8863bc8`, #65 on `b6fb181`): the ChronoKV legs consumed ~44 of the
  60-minute budget and the baselines were cancelled ~15.5 min in, every
  night — the ledger never completed, so M1's "one green ledger week"
  acceptance could never start. Parallelization fixes the budget without
  touching the geometry (geometry changes are ledger-format changes, rule
  9); artifact names follow the split (`arena-ledger-chronokv-<run_id>` /
  `arena-ledger-baselines-<run_id>` — nothing consumed the old single
  name; no committed ledger exists yet). Both jobs remain informational
  (NOT in `ci-passed` needs): gate enablement still needs one green week
  plus the calibrated noise bands.

- **docs (v29 M0, deliverable zero):** the two companion documents the
  roadmap leaned on but that never existed in-tree are committed under
  `docs/audits/`: the Full Adversarial Audit (2026-09-23 — 21 findings,
  verdict D, the probe9/probe15 re-verifications rule 8 cites, and the
  invariant matrix) and the Remediation Specification (2026-09-24 — the
  5-wave/20-commit plan executed as 0.28.0, including the 4044-byte
  page-safe key-bound derivation and the split_interior inclusion the
  audit itself missed). Every probe-ID, matrix and verdict reference in
  the roadmap is now checkable from the tree. M0's re-audit commission
  remains pending.

- **ci (v29 M1 step 4, start):** nightly `arena` ledger vehicle —
  schedule/dispatch-only job running the arena (ChronoKV group + async) and
  all five baseline configs over a FIXED calibration geometry (200k keys,
  100 B values, 4 threads, seed 42), uploading the TSVs (each with its
  rule-9 methodology header) as `arena-ledger-<run_id>` artifacts plus a
  throughput digest in the job log. **Informational by design**: NOT in
  `ci-passed` needs — the M1 acceptance requires one green ledger week
  before deltas-beyond-noise may fail anything, and a flaky shared runner
  must never block a PR. Gate enablement (= adding `arena` to ci-passed +
  committing calibrated noise bands) and the ledger-promotion decision
  (committed `bench/results/` needs a contents:write token or a bot
  commit) are the remaining step-4 work, both documented in the job
  header. actionlint clean.

- **bench (v29 M1 step 3, start):** `bench/baselines.cpp` — baseline
  adapters for SQLite (WAL; `synchronous=FULL` and `=NORMAL`), LMDB
  (defaults) and RocksDB (`sync=true` defaults + one documented tuned
  config) under the arena's identical methodology, geometry, seeds and
  distribution code; `make arena-baselines` (header-presence detection —
  a missing library omits that engine); `bench/third_party/README.md`
  (rule-10 pinning/vendoring policy + the durability-class mapping table).
  Workload subset: fillseq/fillrandom/readrandom/overwrite/ycsb_a/b/c
  (D/E/F adapters follow). Three adapter defects found and fixed during
  validation, each a fairness issue rather than a flake: LMDB dbi-0
  priming (EINVAL otherwise), RocksDB nested-dir creation + hollow-engine
  propagation (segfault on first put otherwise), and the SQLite WAL
  read-snapshot trap (an unreset SELECT keeps the connection's snapshot
  open; the next BEGIN IMMEDIATE on the same connection then returns
  SQLITE_BUSY immediately without invoking the busy handler — and a failed
  BEGIN deflates update latencies). Validated: all five configs clean over
  the subset at 20k keys (zero failed ops across repeats), plus ChronoKV
  group/async at the same geometry — the six-way table is the harness
  proof; the numbers themselves are sandbox-validation only (unclassified
  backing fs), not ledger material.

- **docs:** `docs/ROADMAP.md` replaced by the 2026-09-27 challenge roadmap
  (v29 → v30, revision 3 — two external review rounds; all 22 review items
  plus five citation-precision fixes incorporated, each mapped in the
  plan's own review-disposition table). The 2026-09-17 plan's v28–v30
  sections are superseded via the new plan's Old-arc disposition table
  (nothing silently dropped); its completed v26/v27 history is preserved as
  the file's appendix, with the superseded progress row marked in place.
  README Roadmap paragraph rewritten to match (this entry and it complete
  the adoption mechanics `843f01d` described; that commit's ROADMAP
  replacement landed before them because its README anchor mismatched —
  recorded here rather than papered over). No engine changes;
  `CHRONOKV_VERSION` stays **0.28.0** — versions track shipped code, and
  v29 releases 0.29.0 at its M7 gate.
- **bench (v29 M1, start):** `bench/arena.cpp` skeleton — the engine-side
  micro suite over the hooks-OFF consumer build (rule: the arena measures
  the product's real face): `fillseq`/`fillrandom` (isolating the new-key
  `nm_` path), `readrandom`, `overwrite`, `rangescan` 100/10k,
  `deletechurn` (P1's before-column), `ckptload` (checkpoint-under-load
  wall time), `coldrecovery` (WAL-only reopen — the v30-M1 bulk-load
  prerequisite anchor) and `memory` (RSS-level bytes/key: tree pool +
  version heap + slack). Every run prints a rule-9 methodology header
  before the TSV results. `make arena` target + `bench/README.md`.
  YCSB mixes, vendored baselines and the CI ledger follow in M1 steps 2–4.
  First sandbox run (100k keys, async, -O1, 2-CPU/1 GiB, 4.19 kernel):
  fillseq 31.9k/s · fillrandom 41.4k/s · readrandom 105k/s (p50 10 µs,
  p99 26 µs) · cold recovery 28.5k records/s — the recovery rate
  empirically confirms the roadmap's bulk-load prerequisite (100M records
  ≈ tens of minutes ≫ the 30 s target) — and `ckptload` caught the
  checkpoint stall live (writers completed 4 ops during a 91 ms
  checkpoint), the first datapoint for M6 item 6.

## 0.28.1 — Audit-2 remediation (complete)

The second external adversarial audit round over 0.28.0. Every fix ships
with a fail-first regression test in the Audit-2 battery (appended to
`CKV_ONLY_REMEDIATION=1`; each PoC was verified failing on pre-fix 0.28.0
first — TXN-1 reproduced as a value-based write skew, TXN-2 as pool
exhaustion at ~16.5k absent-read txns on a 1 MiB pool, EXTRA-2 as 4
corrupted neighboring keys, API-3 as silently dropped observers).

**Engine:**

- **TXN-1 (SSI soundness, critical):** `PhantomTracker::record_transition`
  records EVERY committed write, not only existence flips — an UPDATE of
  a key inside a concurrently scanned range is a read-write
  antidependency and now conflicts the scanner. Pre-fix, a
  scan-then-dependent-write transaction committed across a concurrent
  update of the scanned key (value-based write skew). Refinement in this
  release: a no-op delete of an ABSENT key (false→false) stays
  unrecorded — its tombstone carries a cts above every concurrent
  snapshot, so no visible scan output can change; the v17 no-op-delete
  contract (blind and transactional) remains conflict-free.
- **TXN-2:** absent read-set keys become point range reads `[k,k]`
  validated through the phantom tracker (`find_index`, non-creating)
  instead of materializing an invisible index entry per negative lookup
  — the unbounded pool leak on absent-read workloads is gone;
  check-then-insert still conflicts (covered by the `[k,k]` range read +
  TXN-1's insert recording).
- **TXN-3:** `ReadWriteTransaction` cleanup keys on ENGINE liveness (a
  `weak_ptr<ChronoKV>` pin from the public Transaction's keepalive), not
  the Database open-flag — `close()` with a live transaction no longer
  leaves its reader slot and phantom registration pinned forever. Member
  order fixed so `txn_` is destroyed while `engine_keepalive_` still pins.
- **WAL-1:** torn-tail-vs-interior classification never trusts the
  damaged frame's own length field (a corrupted length could point
  anywhere and make interior damage look like a torn tail — silent
  truncation of everything after it). On a parse failure the recovery
  scan looks for ANY CRC-valid frame with a continuing LSN at any later
  offset (cheap LSN + structural prefilter keeps it linear in practice):
  found → CORRUPT (loud, file untouched), none → TORN_TAIL.
- **WAL-2:** creating a segment — or reopening one left EMPTY by an
  earlier failed attempt — fsyncs the WAL DIRECTORY before any record
  in it can be acknowledged; a failed dir fsync fails the open.
- **WAL-3:** `fsync_dir(path)` syncs the PARENT of `path`; both
  rotation-path call sites passed the WAL directory itself, syncing the
  WRONG directory — the MANIFEST rename was never actually made durable
  on those paths. They now pass `manifest_path()`.
- **WAL-4:** recovery bounds the segment loop by what is on disk: a
  MANIFEST `active_id` beyond max-present+1 cannot come from any
  interrupted rotation and fails loud immediately (pre-fix: a ~2^64
  iteration loop); the legitimately checkpoint-deleted prefix is skipped
  in O(1).
- **BT-1:** `BTree::put` checks conservative split headroom
  (`5*(height+1)+1` pages) BEFORE any mutation — pool exhaustion
  mid-split-cascade can no longer orphan acked keys (pre-fix: lower
  levels split and relinked before upper-level allocations could fail).
  In-place updates (new value ≤ old) are exempt on BOTH standalone
  write entry points — `put` and `put_with_old` (the parity gap in the
  first cut of this arc was caught by an external-review probe; the
  extended exhaustion test was verified failing on `put_with_old`
  before the fix landed): they allocate nothing (`put_recursive`'s
  fits()/compact path), so an exhausted pool must not refuse them.
- **BT-2:** the standalone `BTree` serializes structural writers
  (`write_mu_`); under the engine the lock is uncontended (tree writes
  already serialize under `nm_`).
- **EXTRA-1:** the reservation-window throw latches the WAL fail-stop
  INSIDE `group_append` while `batch_mu_` is held — a concurrent
  committer can no longer reserve and write past the burned cts in the
  window before `commit_txn`'s (idempotent) latch runs.
- **EXTRA-2 (memory safety):** the leaf in-place-update path computed
  the slab offset in `uint16_t`; a value larger than `free_hi` wrapped,
  passed both size guards, and memcpy'd out of bounds across pages. Now
  computed in `size_t` with an exact `fits()` predicate.
- **API-1:** the header is a TRUE single header — `#pragma once`,
  `static` → `inline` free functions, `static inline` members. Both
  consumer shapes (double-include in one TU; two TUs linked together)
  are CI-GATED (`make lint-header`), not informational.
- **API-3:** the observer registry lives in a shared `ObserverState`
  that MOVES with the `Database`; async workers, `Batch` and
  `Transaction` capture it by `shared_ptr` — no raw `Database*` a move
  can orphan (pre-fix: observers silently stopped firing after a move;
  the unregister lambda of a surviving handle could dangle).

**Tests:** 15-check Audit-2 battery (`run_audit2_regression_tests`,
fork-isolated where the failure mode is death); v17 guard test 4 FLIPPED
to expect Conflict (its old expectation encoded exactly the TXN-1
anomaly — the flip is documented at the test, not silent); `main.cpp`
double-includes the header as a live API-1 check.

**CI/Make:** `lint` job (`-Werror` syntax gates over header+suite, hooks
on/off; `-Wconversion` count ratchet vs `ci/wconversion.baseline` = 33
via `scripts/lint_wconversion.sh` — the uint16-narrowing class behind
CKV-001/002 and EXTRA-2; header hygiene gating); `build-shared` compiles
the stress/release binaries ONCE per run for dst+crashfuzz (the one big
TU is >1 GiB RSS and minutes per compile; ccache cannot help a single
TU); experimental non-blocking sanitizer legs (io_uring-disabled asan/tsan
— the only config where the WAL write-path fault legs execute — and
clang asan/tsan with `-static-libsan`) until each has been green once;
`make asan CXX=clang++-18` links (gcc/clang static-sanitizer flag spell-
ing); per-leg artifact names via `strategy.job-index`.

**Measured cost (arena geometry, 200k keys / 100 B / 4 threads, group
durability, 2-core sandbox — floors, not ledger material):** ~10–13%
on write-heavy mixes (fillseq 30.1k→26.1k, fillrandom 39.5k→35.1k,
ycsb_a ~40k→~36k ops/s), reads unchanged; ycsb_a RSS +11% (the tracker
now holds updated keys until pruned). Accepted: SSI soundness and the
exhaustion-safety of BT-1 outrank the delta; the nightly ledger records
it per rule 9.

**Known build-memory note:** the patched TU OOMs a 1 GiB container at
`-O1` where 0.28.0 fit (the A2 battery's lambda/std::function density);
`-O0` builds in ~25 s. One more data point for the v30 source-split.

## 0.28.0 — v28 audit remediation (complete)

The 21 findings of the `docs/request.txt` adversarial audit (CKV-001…021)
plus the two platform defects from the external repository review (F1/F2).
Every fix shipped with a fail-first regression test in the remediation
battery (`CKV_ONLY_REMEDIATION=1`); "verified" findings ship contract tests
that pin the traced behavior. No on-disk format changes anywhere in the arc.

Shipped as individual commits over 0.27.0 (pre-0.28.0-stamp):

- **CKV-001** — keys that cannot fit a B+ tree page are rejected
  (`TooLarge`) before any reservation/WAL/index mutation.
- **CKV-002 (+ remediation Blockers 1–4)** — byte-aware, plan-before-mutate
  splits for leaf AND interior pages: full entry set materialized, byte
  costs computed, balanced two-way split chosen when one fits (never greedy
  left-packed), else the minimum greedy cascade; ALL destination pages
  allocated up front; mutation phase allocation-free. Additive-form
  capacity guards (no size_t subtractive underflow). k-way root install.
- **CKV-003a/b/c** — exception-safe tree mutation: `pool_.alloc` failure
  leaves the tree byte-identical; index fail-stop latch (invariant D4: no
  commit/checkpoint on a diverged index); checkpoint dirty-coverage
  validation + recovery dirty re-marking.
- **CKV-004** — WAL rollback truncation gated on the FAILURE STAGE (was:
  durability class): any WRITE-stage failure truncates; only a
  published-written ASYNC batch failing at FSYNC keeps its records
  (documented silent-loss contract, counted in `async_committed_then_lost`).
- **CKV-005** — internal full-range walks are unbounded (`tree_scan_all`,
  `hi_unbounded` cursors): keys at/above the old 255×0xFF sentinel are no
  longer invisible to checkpoint/GC/free walks.
- **CKV-006** — streaming range scans skip tombstones instead of
  truncating at the first deleted key (stream == vector == visible set).
- **CKV-012 (+012R)** — burn-on-throw for the `group_append` reservation
  window: an OOM-class exception between cts reservation and batch entry
  burns the cts via `on_abandon` and rethrows; no orphaned hole can wedge
  the publication barrier. **012R (Phase 2 follow-up):** the burn now also
  latches the WAL fail-stop — the burned cts has no WAL frame (the noop
  contiguity device is exactly what an OOM-class throw cannot safely
  write), so allowing later commits would write higher cts values past an
  unrecoverable interior hole and the next open would reject the whole
  directory (every later ACKED write unreachable). With the latch the
  on-disk WAL stays a contiguous prefix, reopen recovers everything acked
  before the throw, and the fresh instance is writable again.
- **CKV-018** — `pwrite_all` fault hooks: WriteShort/WriteFail reach the
  production WAL write path (de-vacuated under `CKV_IOURING_DISABLED`).

Shipped in the 0.28.0 commit series:

- **CKV-007** — observer callback exceptions are contained (counted in
  `diag::observer_exceptions`): a throwing callback can no longer invert an
  already-durable commit into a caller-visible exception.
- **CKV-008 (+ Blocker 5)** — leaf mutation epoch (`PageHeader`'s two
  former reserved uint16 halves, combined via one explicit
  widen-then-shift expression): the OLC fence now detects
  count/min/max-preserving mutations and in-place updates; split rebuilds
  resume above the captured epoch. In-memory format only — `sizeof(PageHeader)`
  stays 32, `TREE_MAX_KEY_BYTES` unchanged.
- **CKV-009** — verified (no defect): every public write path records
  phantom transitions under the reserved cts; point-read/write skew
  conflicts via read-set validation. Contract tests added; the
  `replay_records` replication spike (no phantom recording) is documented
  as not reachable from the public API.
- **CKV-010** — `restore_pitr` refuses a non-empty destination (was:
  silently mixed PITR state with pre-existing `dest/wal` contents).
- **CKV-011** — PITR opens are strictly read-only on the source: no
  `.chronokv.lock` creation, no writer flock, no torn-tail repair, no
  orphan-`.tmp` sweep; `backup()`/`checkpoint()` refused via a
  `checkpoint_locked` guard (backup previously rewrote the source chain
  AND rotated the source WAL from a "read-only" handle). Documented
  consequence: an as-of view of a LIVE directory is not serialized
  against its writer (loud failure, never silent corruption).
- **CKV-013** — io_uring registered-file slot keyed on `(st_dev, st_ino)`:
  fd-number reuse across segment rotation can no longer leave WRITE_FIXED
  batches targeting the sealed old inode; failed re-registration clears the
  cached identity (retry instead of targeting an empty slot).
- **CKV-014** — duplicate-key `Batch::commit` returns
  `InvalidTransaction` with the batch LEFT INTACT (was: silently consumed,
  making the next `commit()` an "OK" no-op).
- **CKV-015** — verified (no defect): abandoned-transaction lifetime
  traced (engine keepalive + liveness weak flag + guarded destructors);
  contract tests added (transaction outliving close() and outliving the
  Database object fail cleanly, never UAF, never process-abort).
- **CKV-016** — async futures resolve to `Status` for ANY engine failure
  (pool-exhaustion `bad_alloc` via the D4 latch-and-rethrow path included);
  `get()` never rethrows. Removes the forced-coverage runs' `std::terminate`
  escape point.
- **CKV-017** — README LSN claims corrected to the real contract:
  per-segment LSN continuity validation + commit-timestamp gap/duplicate
  rejection on replay (documentation only; recovery semantics unchanged).
- **CKV-019** — interior-hole WAL segments fail LOUD: torn-tail repair
  refuses to truncate a hole with parseable records after it (was: silent
  loss of acknowledged post-hole frames at open), `open_segment`
  propagates repair failure into the fail-stop, and the LSN resume scan
  throws on CORRUPT instead of reseeding from 0 (which re-issued LSNs and
  bricked recovery).
- **CKV-020** — `checked_close` calls `close()` exactly once (the EINTR
  retry loop was an fd-reuse hazard: on Linux an interrupted close has
  already released the descriptor).
- **CKV-021** — the stale `commit_locks`/`await_published` comments
  (claimed per-key mutexes released before the barrier; they are held)
  replaced with the actual lock set and the lock-before-reserve
  deadlock-freedom argument; stale CMakeLists/Makefile-header references
  fixed.
- **Review F1** — io_uring availability means I/O OPS work, not merely
  ring setup: one shared deep probe (real WRITE + CQE inspection) gates
  both the startup banner and the `iouring-real` e2e SKIP (pre-fix the
  test FAILED on blocked-ops kernels where the banner promised SKIP);
  the ring permanently degrades after 3 consecutive CQE-level write
  failures (pre-fix every batch paid a doomed io_uring attempt forever).
- **Review F2** — `wal_dir` is created recursively; an uncreatable
  directory reports the real cause instead of a misleading
  "inter-process lock" error. The README quick-start runs verbatim on a
  fresh machine.

## 0.27.0 — v27 M3: coverage aimed at error paths

gcov build (`make coverage`), `CKV_COVERAGE_FAULT` per-kind forcing,
`scripts/fault_coverage.py` publishing per-kind error-path coverage,
unique per-kind contributions, vacuity (0-fire) detection, and the ledger
of error-path lines no run reaches. CI `coverage` job folded into
`ci-passed`. Plus three unbumped hardening commits (data-preserving death
under forcing, bounded/resharded nightly DST, strong `__gcov_dump`
binding).

## 0.26.1 – 0.26.3 — v27 M0/M1/M2 on the v26 tail

- 0.26.1: PITR mid-window per-entry delta filtering + loud failure on
  unprovable windows (rank-1 review fix); strict-serializability checker
  (`lincheck`) start — found the live publication-prefix lag on its first
  engine run; commit-ack publication barrier + burn-on-throw install tail.
- 0.26.2: CI integration — dedicated `crashfuzz` and `dst` jobs (bounded
  PR / long nightly seed scaling), seed knobs echoed for replay.
- 0.26.3: deterministic scheduler (`dst::` seeded batons at every
  instrumentation point; `(seed, op-count)` replay; proven C1/H3 catch);
  `lincheck` completion (set-shape checker, range-scan modeling,
  async/batch recording); PITR fully-rotated-window loud refusal
  (unbumped review response); sharded nightly DST.

## 0.25.4 – 0.25.8 — v26 arc (M0–M4)

Durable rollback truncation (D2); adversarial fsync semantics (D3
fail-stop); randomized crash-point fuzzing (20 instrumented points,
every-point-reached assertion); v25.7 review fixes (MANIFEST ckpt_ts
across size rotation, GC idle-spin, observer data race); online backup
(B1, self-verifying marker); v25.8 close-race fix (rank 1), compound
fault×crash-point plans (rank 2/3); point-in-time restore (v26 M4).

## 0.25.3 and earlier

v25.3 review defect fixes (C1 leader-hang, H1, H3, H4); the v18–v25
history lives in `chronokv.hpp`'s header block and `docs/ROADMAP.md`.
