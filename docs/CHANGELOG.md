# ChronoKV changelog

This file exists because `chronokv.hpp` has referenced `docs/CHANGELOG.md`
since the v22 arc while the file was never in-tree (fixed in 0.28.0).
Entries below 0.28.0 are backfilled from `docs/ROADMAP.md` and the header's
version history; 0.28.0 is recorded in full.

## Unreleased

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
