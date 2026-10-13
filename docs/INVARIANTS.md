# ChronoKV invariant matrix — the living test-plan document

**Seeded:** 2026-10-13 (post-M2, at `52fd43c`). **Calibration point:** v30 M0
("every canonical invariant ID gets a runtime checker, a CI job, or a
reviewed checked-by-construction argument; the 14-row matrix becomes a
living test-plan document; docs gain the invariant→checker→job mapping").
This file IS that document, started early: everything below was read out
of the tree at the seed commit, not transcribed from memory of the audits.

**Why it exists now:** the audit's §16 matrix (2026-09-23) was a snapshot;
three arcs later the ID set has grown (E-series, G-series proposal, the
Audit-2 rows, the SRC contracts) and the checker/vehicle landscape changed
under it (DST grammars, crash-fuzz composition, the de-vacuated batteries).
A map that is only accurate inside audit reports is prose; this file is
the single place where ID → enforcement → checker → vehicle → proof lives,
and it is maintained by the rule in §6.

**Status legend.**
- **PROVEN** — machine checker runs in a CI vehicle AND a scratch-build
  break (mutant/reversion) of the invariant is recorded in tree history
  with old-GREEN/new-RED (or equivalent) attribution.
- **CHECKED** — machine checker runs in a CI vehicle; no recorded
  scratch-break demonstration (v30 M0's acceptance owes one per invariant:
  "intentionally breaking any invariant in a scratch build fails at least
  one CI gate — proven once per invariant").
- **STRUCTURAL** — enforced by construction/argument, no runtime checker.
  v30 M0 must either build the checker or record the reviewed argument.
- **PROPOSED / PENDING-WITH-Mx** — decided but not yet implemented.
- **CONFIRM** — cell needs verification at calibration (honest gap, listed
  in §5 rather than guessed).

Citations are function-scope anchors with as-of tags (reading rules:
line numbers rot; this repo has proven it repeatedly).

---

## 1. The canonical ID series, and one collision on the record

Series in use (reading rules: "canonical IDs for new invariants
(R/I/D/E/P/B/T)"): **R** publication order · **I** index/WAL replay
integrity · **D** durability & fail-stop · **E** epoch-pinned reclamation ·
**P** page/tree steady-state · **B** backup · **T** transactional
semantics. Adjacent families that predate or extend the series and are
tracked here: **CKV-nnn** (0.28.0 remediation regressions), **WAL-n /
TXN-n / BT-n / SRC-x / API-n / EXTRA-n** (Audit-2 and SRC findings),
**G-n** (proposed with the M3 HP4/ABA decision doc), **C1/H3** (historical
review-finding anchors kept as scenario names).

**COLLISION, recorded for resolution at v30 M0:** `P1` names two different
invariants. (a) Audit §16's P1: "PITR opens are read-only and
non-destructive to the source" (CKV-011 remediated). (b) v29 M4's P1: the
delete-heavy page-pool steady-state invariant ("bounded utilization, no
use-after-free, no ABA"), which the reading rules explicitly reserve
("P1 pending with M4"). This document writes them **P1-PITR** and
**P1-PAGE** and proposes v30 M0 re-anchor the audit-side row (e.g.
PITR-RO) so the reserved ID lands clean. Neither usage is rewritten in
historical documents; the disambiguation lives from this file forward.

---

## 2. The matrix

### 2.1 Publication & replay order

| ID | Contract | Enforced at (as of `52fd43c`) | Checker / oracle | Vehicle | Status | Recorded break-proof |
|---|---|---|---|---|---|---|
| **R1** | `published_ < clock_` — publication never outruns allocation | asserts after commit and in recovery; `pub_.recover_to` ordering | `verify_publication()` (bool+reason) | crash-fuzz child verification (`run_crash_fuzz`), full suite asserts | CHECKED | crash-fuzz regime mutants across v26–v27 arcs (see §3 note on indexing) |
| **I2** | recovery sees every cts exactly once, in order (contiguity) | `recover_all`/`recover`: duplicate-ts and interior-gap throws; `ReplayResult::GapDetected` | v18 batch test: `recovery_records()==acked`, `published_watermark()==acked`, 2,000-key unique-value differential readback | engine-tests (all legs), stress suite | **PROVEN** | `79c4490`: recover skips applying every 1000th record (counters/contiguity intact) → new RED / old GREEN; plus this arc's tail-drop measurement (`recovered=1, records=1999` — the old no-throw shape) |
| **I3** | WAL replay non-decreasing per record | assert in the replay loop (`recover_with_checkpoint`'s record walk) | assert-active in every test build (no `-DNDEBUG` anywhere in the matrix) | all suite vehicles | CHECKED | — (assert-class; a violating record shape is constructed by the recovery corruption batteries) |

### 2.2 Durability & WAL

| ID | Contract | Enforced at | Checker / oracle | Vehicle | Status | Recorded break-proof |
|---|---|---|---|---|---|---|
| **D1** | MANIFEST integrity (magic+CRC+min length) before trust; an open failure is an ERROR, not an absence (SRC-A) | `read_manifest` validation; SRC-A fixed all three readers | corrupt-MANIFEST battery; SRC-A legs (`6f9305b`) | engine-tests | PROVEN | 0.28.0-wave fail-first; SRC-A fix commit carries its own confirmation |
| **D2** | a batch any caller observed `WalFailure` for is absent from the WAL after any crash (rollback durable) | `group_append` rollback + `checked_fsync`; SRC-B zero-fill neutralization of the ftruncate-failure window | `run_v26_durability_tests` (D2a/D2b), CKV-018 async write-stage leg, SRC-B/SRC-E4 battery (`5d037ba`, `e075a47`) | engine-tests; crashfuzz | **PROVEN** | v26 M0 fail-first; CKV-018 charge-fired anti-vacuity; SRC-E4b FAILED against the old warning (observability gap) and passes post-upgrade |
| **D2-indet** | a rollback that could not be COMPLETED (ftruncate/fsync failed) is surfaced, never silently trusted | `rollback_indeterminate_` latch + health reason + zero-fill; loud interior-CORRUPT refusal on reopen | SRC-E4 battery: byte read-back, latch + health, reopen refusal, operator repair executed end-to-end | engine-tests | **PROVEN** | `e075a47` M6 mutant (zeroed length classified TORN_TAIL despite CRC-valid frames) → E4a RED `refused=0`, the ONLY flipped bit |
| **D3** | WAL-path fsync errors latch fail-stop permanently; one health model, not two | `failed_` latch; `is_failed()` checks; index-side analogue = D4 | D3a–e battery; health-level assertions | engine-tests | PROVEN | v26 M0 fail-first legs |
| **D4** | index-side failure latches fail-stop pre-mutation; checkpoint/backup refuse while latched | `index_failed_` latch; BT-1 headroom throw → catch latches; `checkpoint_locked` refusal | CKV-003b/016 battery; refusal legs in checkpoint tests; S6 health oracle | engine-tests; dst | PROVEN | 0.28.0-wave fail-first (battery is fail-first by construction) |
| **WAL-1** | recovery offset-scan classifier: interior zeros before surviving CRC-valid frames = CORRUPT (loud), never silent truncation | `recover_all` `valid_after` scan; `truncate_torn_tail` refusal path | SRC-E4a/E4b; CKV-004R tightened status pin | engine-tests | **PROVEN** | `e075a47` M6 mutant (above); item-4 T4 grammar composes multi-chunk extents × write faults around the classifier |
| **WAL-2** | segment creation's dir-fsync window is crash-safe (empty-orphan residue tolerated; non-empty orphans refused loudly) | `open_segment` fresh-path fsync_dir; `recover_all` orphan classification | crash points `seg_before_dir_fsync`/`seg_after_dir_fsync` (kAll 28) + crash-fuzz T5 template with FIRED-WITNESS (rc-22) | crashfuzz | **PROVEN** | `7d74627` MUT-W2T (empty-orphan tolerance reverted → 16 violations at the seg_*/man_* plans) and MUT-W2V (DirFsyncFail wiring removed → 7 witness violations; clean 0) |
| **WAL-3** | the rotation dir-fsync syncs the RIGHT directory (`fsync_dir(path)` syncs path's PARENT) | `rotate_after_checkpoint`'s final `fsync_dir(manifest_path())` — the fixed call carries the anchor comment "WAL-3: a file IN dir_, so dir_ itself is synced" | SRC-C remediation leg arms `RotFinalDirFsyncFail` AT that call site with a fired-witness (`charge_remaining==0`) + counted-not-fail-stop contract; crash-fuzz rotation regimes; fault matrix per-kind anti-vacuity | engine-tests; crashfuzz; coverage | CHECKED (handling) + STRUCTURAL (target) | G-1 CLOSED 2026-10-13 by indexing, with the honest boundary: the fired-witness proves the call exists and its failure contract holds, but the fault fires regardless of the fsync_dir ARGUMENT — target-dir correctness is comment-anchored and was verified by reading (Audit-2's own method), and NO in-process proof of it can exist (a missing/wrong fsync is invisible to `_exit()`-based testing — the page cache survives; the decision doc's WAL-2 boundary sentence applies verbatim). The mechanical target-level proof is v30 M3's dm-flakey leg. |
| **R-REBASE** | rotation/rebase never deletes WAL needed for recovery | `rotate_after_checkpoint` under `batch_mu_` + ckpt ordering; H1: size rotation PRESERVES `last_ckpt_ts_` in the MANIFEST | m2p1 phase-1b (≥2 segments + pre/post-ckpt keys); H1 regression; crash-fuzz T1 (Ckpt→Rotate, "guaranteed forever"); size-rotation admission via the hooks-gated cap (8 segments, byte-exact) | engine-tests; crashfuzz | **PROVEN** | H1 historical (the bricked-database incident); `79c4490` mutant (`maybe_rotate_segment` early-return → seg_count=1, old GREEN); `7d74627` T1/W2T |

### 2.3 MVCC, SSI, epoch-pinned reclamation

| ID | Contract | Enforced at | Checker / oracle | Vehicle | Status | Recorded break-proof |
|---|---|---|---|---|---|---|
| **T1** | read-your-writes inside a transaction | `ReadWriteTransaction::read/range_scan` `ws_` overlay | v22 M2 battery | engine-tests | CHECKED | — |
| **TXN-1** (recording completeness) | EVERY committed write is phantom-recorded (sole exemption: no-op delete of an absent key), so SSI sees rw-anti-dependencies | `record_transition` (0.28.1 refinement) | `lincheck::check_write_skew` (multiversion dependency graph, rw ANTI edges from point reads AND scans) + 4-anomaly/3-control synthetic battery (W2 = the TXN-1 PoC verbatim) + engine-workload aggregates + engine-shaped mutation with non-vacuous fallback | dst job's lincheck leg; engine-tests | **PROVEN** | v29 M2 item 1 (2026-10-08): live pre-fix-flag on `v0.28.0` / post-fix-pass on 0.28.1, the exact cycle named; `f163bc1` CI-hardening after run #72 starved the first predicate |
| **SSI claim** (serializability, not just SI) | committed histories are serializable | commit validation (rs under `commit_mu` + phantom re-check under `batch_mu_`) | the five lincheck checkers (cts-order, real-time, list-append, scan-exactness, set-algebra) + write-skew; every engine workload history must pass all and be non-vacuous | dst lincheck leg (4 seeds PR / 64 nightly) | PROVEN | item-1 battery + mutation legs (checker flags fabricated anomalies) |
| **E-series (E1–E16)** | epoch-pinned reclamation safety: pins before a sever block reclamation; pins after cannot reach severed suffixes | `acquire_slot_locked` (snapshot+pinned_epoch under `reader_mu_`), `retire()` epoch tagging, `reclaim_retired()` `retired_epoch < min_active_pin_epoch()` | v23 Phase A/C battery; DST S3; **DST S6 `reclaim_retire_pin`** (capture control, in-flight pinned stability, pinned-block WITNESS `retired_pending()>=NK`, post-unpin DRAIN, retired/reclaimed controls); v17 boundedness accounting | dst (6 scenarios × seeds); engine-tests | **PROVEN** | `f64dfb6` S6 mutants: epoch-gate off-by-one `<=` → witness-only RED (`pending_under_pin=0`, stability green — the gate is defense-in-depth at the version layer and the witness pins it before M3 makes it UAF-class); reclaim neutered → drain RED (`retired_pending=208`); retire neutered → witness RED. v17 threshold mutant → boundedness RED (`live=5000 want<=10`, legacy half green — the vacuity made visible) |
| **Reader-slot bookkeeping** (CKV-015 class) | free slots are snapshot-0/epoch-0, no dup/OOB indices | `release_slot` under `reader_mu_` | `verify_reader_slots()`; Fix-12 test WITH post-release prune control | engine-tests | PROVEN | Fix-12's control leg (prune erased after release — "a detector that has never failed is not a detector") |
| **[k,k] point-range equivalence** (Audit-2 row) | a single-key range scan equals the point read at the same snapshot AND a scan-based negative lookup conflicts like the get-based route | one `read_at_idx` resolution path; one `RangeRead{lo,hi}` registration feeding `has_phantom_in_range` | **`run_kk_equivalence_test`** (release mode, 7 legs: latest/absent/deleted/as-of value equivalence + scan-based negative-lookup conflict + outside-range control + inclusive-upper-bound pin) + DST S6 prober's per-round single-key as-of probe (stress) | engine-tests; dst | **PROVEN** | 2026-10-13: cursor hi-exclusive mutant → legs A/D RED; phantom-bound `<` mutant → legs E1/E3 RED; measured co-flips recorded at the test (CKV-002a/e, 006, 003c, 005; A2 TXN-1 — and A2's get-based TXN-2 detector does NOT flip, prose corrected by measurement) |

### 2.4 Tree & pages

| ID | Contract | Enforced at | Checker / oracle | Vehicle | Status | Recorded break-proof |
|---|---|---|---|---|---|---|
| **PAGE** | every key/value write stays inside its 4096-byte page | `put_leaf_nolatch` space check; byte-aware split planner; `TREE_MAX_KEY_BYTES` (4044) pre-check in `commit_txn` (CKV-001) | CKV-001/002 battery (large keys, split-rebuild path), btree fuzz | engine-tests (ASan legs are the historical detector) | **PROVEN** | 0.28.0-wave ASan PoCs, fail-first at landing |
| **OLC** | the fence re-check rejects any concurrent structural change raced with | `fence_unchanged` + `page_bump_mut_epoch` at EVERY in-place mutation site (CKV-008) | m16 concurrent-putters battery; `fence_recheck_retries` stress asserts; DST S4 `tree_cursor_split` | engine-tests; dst; stress suite | **PROVEN** | CKV-008 deterministic count-preserving-split PoC (pre-fix RED) |
| **BT-1** (headroom bound) | descent reserves `5*(height+1)+1` pages pre-mutation; exhaustion throws BEFORE any page is touched → D4 | headroom check in the put path (Audit-2: bound re-derived with M3/M4) | CKV-003b bounded-pool legs (`PageCapacityError`→`TooLarge`); pool-exhaustion battery | engine-tests | PROVEN | 0.28.0-wave fail-first; re-derivation PENDING-WITH-M3/M4 (roadmap-assigned) |
| **G1/G2/G3** (PROPOSED) | G1 slot identity: every free→alloc transition bumps the slot generation under the pool mutex before reachability; G2 verification: no content trusted across a latch handoff without gen check (after latch, before content); G3 scope: heap-address references stay epoch-governed, never gen-checked | `docs/audits/2026-10-12-m3-hp4-aba-decision.md` §6 checklist | fail-first plan specified (decision doc §9): alloc-without-bump, cursor-switch-check-removed, fence-gen-removed (the last UNREACHABLE until M4 supplies frees — recorded, not faked) | PENDING-WITH-M3 | **PENDING-WITH-M3** | n/a (mechanism does not exist yet — the decision precedes the build, as the milestone requires) |
| **P1-PAGE** (v29 M4) | sustained delete-heavy workload: pool steady state, bounded utilization, no UAF, no ABA | PENDING-WITH-M4 | pre-gates already live: DST S5 `tree_merge_cascade` grammar (written before the code, green vs merge-free tree, MUT-B-proven at the pinning layer); arena delete-churn + space-amp tables (M1 vehicle); 7-day soak on the M1 vehicle | dst; arena; soak chain | **PENDING-WITH-M4** | S5's mutation record (`f64dfb6`) covers the pinning contract the merge code must survive |

### 2.5 Checkpoint & recovery state

| ID | Contract | Enforced at | Checker / oracle | Vehicle | Status | Recorded break-proof |
|---|---|---|---|---|---|---|
| **CKPT-C** | a checkpoint base/delta contains every live key visible at its cts; loss is impossible across rotation | `checkpoint_locked` capture (full-scan base / dirty-filtered delta); CKV-004R coverage refusal + D4 latch; v18.2 dirty-clear filter | v19 soak recovery differential (shadow model over live AND recovered state; post-join tail-write phase guarantees a non-empty WAL delta every run); m2p1 batteries; `verify_ckpt_file`/`verify_checkpoint_chain` | engine-tests; stress suite | **PROVEN** | `79c4490` soak mutant (recovery skips the last 3 WAL records → `sk119 expected 'tail:17' got ABSENT`, old GREEN) — the run that also exposed and fixed the empty-delta second vacuity |
| **v18.2 filter contract** | dirty entries with dcts > ckpt cts survive the post-write clear (defense-in-depth: the race is structurally locked out by the checkpoint_mu_ discipline — MEASURED: a full revert passes the 515-check suite) | `checkpoint_locked`'s filtered clear | `run_v182_dirty_filter_test` (witness, CKV-004R non-refusal, oracle A survival, oracle B boundary control, oracle C end-to-end) | engine-tests | **PROVEN** | `33aab1b`: three mutants, exact oracle isolation (unconditional clear → A RED; `<` → B RED at dcts==cts; no-op clear → B RED) |
| **Delta-chain integrity** | base+deltas+WAL compose to exactly the pre-close state; rebases are format-safe | delta sequencing + `REBASE_AFTER_*` thresholds; `verify_checkpoint_chain` | v19 WS3 delta-chain crash matrix (6-boundary fork battery); m2p3 rebase legs | engine-tests; crashfuzz-adjacent | CHECKED | CONFIRM per-boundary proof indexing; §5 gap |
| **CKV-005 class** (sentinel-invisible keys) | full-range walks never express "all keys" as a 255×0xFF bound | `tree_scan_all` / `Cursor(hi_unbounded)` | CKV-005 battery; the 256×0xFF-key roundtrip legs | engine-tests | PROVEN | 0.28.0-wave fail-first |

### 2.6 Backup & PITR

| ID | Contract | Enforced at | Checker / oracle | Vehicle | Status | Recorded break-proof |
|---|---|---|---|---|---|---|
| **B1** | a backup is self-verifying and complete (marker last; verify checks size+CRC+chain+WAL parse) | `backup_to` marker machinery | `verify_backup` + round-trip differential + per-`bk_*` interruption battery with anti-vacuity charge-fired witnesses; crash-fuzz Backup window (item-4 grammar) | engine-tests; crashfuzz | PROVEN | interruption legs are fail-first by construction (each `bk_*` point proven reachable + fired) |
| **P1-PITR** (audit §16 "P1") | PITR opens are read-only and non-destructive to the source | `pitr_as_of_` gates; `wal_read_only` construction (CKV-011); checkpoint/backup refusals | CKV-010/011 battery; source-mutation assertions | engine-tests | PROVEN | 0.28.0-wave fail-first; ID collision with P1-PAGE recorded in §1 |
| **v26.1 delta filtering** | mid-window `as_of` × WAL rotation cannot silently lose acknowledged commits; unprovable partial windows fail LOUD | per-entry delta filtering; refusal path; published-watermark coverage of delta-only versions | PITR battery incl. the 6d8a13d rewrite detector; boundary `as_of` legs | engine-tests | PROVEN | 0.26.1: the reviewer's positive control passes and was verified to fail pre-fix |

### 2.7 Health, lifecycle, API contracts

| ID | Contract | Enforced at | Checker / oracle | Vehicle | Status | Recorded break-proof |
|---|---|---|---|---|---|---|
| **Fail-stop health model** | `health()` levels 0/1/2 with reasons; WAL and index domains diagnosable separately; S6 asserts level 0 after clean runs | `HealthInfo` derivation | health legs across D3/SRC/CKV batteries; DST S6 D4 oracle | engine-tests; dst | CHECKED | — |
| **Close/lifecycle safety** (v25.8 rank-1) | close() races with any API mode are clean-failure only | `close_mu_` + `alive_` weak-pin discipline | lifecycle battery (40 iters × 5 API modes); TXN-3 engine-weak legs | engine-tests | PROVEN | v25.8 fail-first |
| **CKV-016 async contract** | async failures rethrow at the future, never vanish | `*_async` wrappers | CKV-016 battery | engine-tests (the sanitizer legs historically caught it in-process) | PROVEN | 0.28.0-wave |
| **CKV-006/007** | streams see tombstones; observer exceptions contained | stream/observer paths | CKV batteries; observer reentrancy pin | engine-tests | PROVEN | 0.28.0-wave |
| **EXTRA-1** | reservation-window throws under batch pressure latch fail-stop, not corrupt | group_append reservation path | crash-fuzz T3 (PressurePuts × AllocFail) with the interior-gap refusal NEVER classifiable as acceptable | crashfuzz | **PROVEN** | `7d74627` composition ledger asserts T3 fired (18 at 336-plan scale); grammar's loud-refusal classification pinned narrow |

### 2.8 Measurement invariants (non-safety, arena-owned)

| ID | Contract | Enforced at | Vehicle | Status |
|---|---|---|---|---|
| **Fairness** | same harness, same HARDWARE, pinned geometry; every ledger row carries its hardware header | arena job (re-merged to ONE job after #75's headers caught the two-runner CPU split) | arena nightly | CHECKED — the header mechanism itself caught the split (rule 9/13 working as designed) |
| **Green week** | 7 consecutive green nights on the re-merged vehicle before gate enablement | nightly chain | arena | IN PROGRESS: nights 1–3 banked (#82/#88/#91, ledger artifacts up) as of 2026-10-13 |
| **Ledger honesty** | numbers are budgets, not trophies; variance lands in bands, not in state | segment rebuilds from pinned geometry | arena | STRUCTURAL (band calibration pending week completion) |

---

## 3. The recorded break-proof ledger (index)

v30 M0's acceptance is per-invariant scratch-break demos "recorded in the
audit trail". Proofs recorded IN-TREE since the v29 arc began (the modern,
indexed era — older arcs recorded proofs in commit messages and audit
docs, indexed here by milestone rather than sha):

| Commit / milestone | Invariant(s) demonstrated | Mutant shape → catcher |
|---|---|---|
| v29 M2 item 1 (`f163bc1` + the 2026-10-08 landing) | TXN-1, SSI claim | live engine reversion: `v0.28.0` records the anomaly committing, checker flags the exact cycle; 0.28.1 refuses |
| v29 M2 item 2 (`f64dfb6`) | E-series gate, pin-threshold contract (S5+S6 grammars) | gc_threshold-without-slot-floor → both scenarios RED; reclaim neutered → drain RED; retire neutered → witness RED; epoch-gate `<=` → witness-only RED |
| v29 M2 item 3 (`79c4490`) | I2, CKPT-C, R-REBASE admission, GC boundedness, soak oracles | skip-every-1000th-apply; last-3-records; rotate early-return; reclaim neutered — each old-GREEN/new-RED side by side |
| v29 M2 item 4 (`7d74627`) | WAL-2, EXTRA-1, R-REBASE compositions | MUT-W2T (16 violations), MUT-W2V (7 witness violations, clean 0) |
| Post-item hardening (`33aab1b`) | v18.2 filter contract | unconditional clear / `<` / no-op — three mutants, exact oracle isolation |
| SRC-E4 round (`e075a47`) | WAL-1, D2-indet | M6 classifier mutant → E4a RED `refused=0`, one flipped bit |
| 0.28.0 remediation arc (`9016918`…`3161c17`) | PAGE, OLC-precursor, D4, BT-1, CKV-005/006/007/010/011/016, D1 | the wave-by-wave fail-first battery (every CKV test FAILED on audited commit `8c77a7c` by construction) |
| v25.7/v25.8 review arc | R-REBASE (H1), lifecycle rank-1, C1/H3 (DST acceptance) | historical incidents + the DST C1-hang/H3-SEGV reintroductions (caught 3/3 and 12/12) |
| v26.1 | v26.1 delta filtering | reviewer's positive control, verified failing pre-fix |

## 4. ID → CI vehicle map (which job would catch a break)

- **lint** — the static gates (Werror both hook modes × four TUs, clang
  leg promoted `0f46002`, -Wconversion ratchet 33=33, header hygiene incl.
  the STRESS 2-TU link). Catches: ODR/API-1 class, ratchet growth.
- **engine-tests (9 legs** — release ×5 toolchains incl. io_uring-disabled,
  asan+ubsan ×2, +matrix siblings**)** — the full hooks-on suite: every
  battery-named checker in §2 (CKV, D2/D3, B1, PITR, v17/v18/v18.2/v19
  soak, m2 phases, Fix-12, lifecycle).
- **tsan (3 legs)** — the concurrency claims: E-series ordering audit
  ("corroborated by TSan, including -DCHRONOKV_STRESS"), OLC, T1.
- **public-api-smoke (4 legs, hooks-OFF)** — the consumer face compiles
  and behaves without test hooks (API-1's true-single-header claim).
- **dst** — PR: 100 seeds × 6 scenarios + stress full suite (seed 7) +
  lincheck workloads (4 seeds). Nightly: 8 shards × 2,083 × 6 ≈ 100k
  scenario-runs + seeds {7,21,42} + 64 lincheck seeds. Catches: C1/H3
  classes, S3 GC-vs-scan, S4 cursor-vs-split, **S5 merge/cascade grammar
  (P1-PAGE's pre-gate), S6 retirement grammar (E-series)**, TXN-1/SSI.
- **crashfuzz** — the composed plan grammar (7 templates, 28 crash points,
  12 fault kinds, ledger + composition asserts): WAL-1/2/3, R-REBASE,
  EXTRA-1, B1 interruptions, R1/I-series via child verification.
- **coverage** — per-fault-kind forced fires + the untested-error-lines
  ledger (v30 M0 drives the durability-path ledger to zero; the 0-fire
  kinds are VISIBLY vacuous per vehicle by design).
- **arena (nightly, informational until gate enablement)** — §2.8 rows.
- **ci-passed** — the aggregate gate every push must satisfy.

## 5. Gap list for v30 M0 calibration (honest CONFIRM/pending cells)

- **G-1 (WAL-3): CLOSED 2026-10-13** — resolved by indexing: no
  in-process reversion proof exists or CAN exist (wrong-target fsync is
  invisible to `_exit()` testing — the decision doc's boundary); the
  row now names the SRC-C fired-witness leg as the handling-contract
  checker, marks the target half STRUCTURAL with its anchor, and defers
  the mechanical proof to v30 M3's dm-flakey leg.
- **G-2 (I3, R1):** assert-class invariants lack a modern indexed
  break-demo; v30 M0 owes one each (a corrupted-record scratch build for
  I3; a publication-order mutant for R1) or a reviewed structural argument.
- **G-3 ([k,k] equivalence): CLOSED 2026-10-13** — release-mode checker
  landed (`run_kk_equivalence_test`, 7 legs across the row's value and
  conflict halves), row upgraded to PROVEN with both mutants and the
  measured co-flip sets recorded (including the prose correction: A2's
  get-based TXN-2 detector does not ride the range predicate).
- **G-4 (delta-chain integrity):** index the 6-boundary crash matrix's
  per-boundary proofs into §3's ledger shape.
- **G-5 (T1):** overlay semantics are battery-checked but have no indexed
  break-demo post-audit; assign one.
- **G-6 (P1-PITR re-anchor):** execute the §1 collision resolution.
- **G-7 (untested-error-lines ledger):** the durability-path subset to
  zero (v30 M0's own text); coverage job publishes the current ledger.
- **G-8 (G1–G3):** rows go live at M3; the decision doc's §9 fail-first
  plan is the pre-recorded proof design.

## 6. Maintenance rule (what makes it "living")

Any commit that (a) lands a new invariant, (b) adds/removes/renames a
checker, (c) records a new break-proof, or (d) changes a CI vehicle's
scope, updates its row HERE IN THE SAME COMMIT (rules 2/3/7 — docs in the
commit they describe). Rows are never deleted; superseded statuses carry
their date. The §5 gap list shrinks only by evidence, never by edit.
Calibration owner: v30 M0, which inherits this file as its matrix and its
mapping deliverable.
