# ChronoKV changelog

This file exists because `chronokv.hpp` has referenced `docs/CHANGELOG.md`
since the v22 arc while the file was never in-tree (fixed in 0.28.0).
Entries below 0.28.0 are backfilled from `docs/ROADMAP.md` and the header's
version history; 0.28.0 is recorded in full.

## Unreleased

- **feat (v29 M3 slice 3/3, 2026-10-18): the PURGE — dead keys are
  physically evicted, and the index stops growing with lifetime distinct
  keys (README limitation #1, heap side closed).** A key whose head
  version is a tombstone at or below the GC pass threshold is dead for
  every live snapshot (threshold = min over live slots; every live
  `ts >= tombstone cts` reads "absent" identically via tombstone or via
  absence, no live `ts` sits below). `purge_dead_key`: re-validate +
  flag `KeyEntry::purged` under `commit_mu` → unlink the tree slot under
  `nm_` (`BTree::erase`; empty leaves stay linked until M4) → retire the
  entry through the epoch reclaimer (new `RetiredEntry` queue, same
  `min_active_pin_epoch` gate as versions; UNLINK-BEFORE-RETIRE per
  decision doc §7.3). `commit_txn` gained (a) a `SnapshotGuard` pin for
  the whole commit — blind writers hold raw `KeyEntry*` across the
  ensure→lock window and the pin folds them into the E-series algebra —
  and (b) an ensure→lock retry loop on the purged flag (resurrection =
  fresh entry; bounded, mutex-linearized against the flag-set).
  Budget `GC_PURGES_PER_PASS = 64`. Version-accounting parity: the
  purged entry's tombstone anchor is tallied into `reclaimed`, keeping
  `live == created - reclaimed` exact. CHECKPOINT INTEGRATION (found by
  the v19 soak on this slice's first full-suite run — see EVIDENCE): a
  purged key can still sit in `dirty_since_ckpt_`, and a delta capture
  of a dirty key that is ABSENT from the index trips the v28 CKV-003c
  coverage validation — correctly: a delta that does not cover the
  delete would RESURRECT the key on recovery once the checkpoint's WAL
  rotation truncates the delete record (base still holds the pre-delete
  value; pre-v28 that was the silent-loss path). So `purge_dead_key`
  records `(key → tombstone cts)` in `purged_since_ckpt_` (under
  `dirty_mu_`, BEFORE the unlink), the coverage validation SYNTHESIZES
  the delta entry `(key, cts, "", deleted)` from the record (genuine
  divergence still latches D4 — the CKV-003c fail-first test still
  trips), and the checkpoint tail clears records under the same cts
  filter as the dirty map (any throw before the tail leaves both maps
  intact; the next checkpoint re-synthesizes — idempotent). GC passes
  and checkpoints both hold `gc_active_mu_` end-to-end, so
  record/consume/clear never interleave; purge takes `commit_mu`,
  `dirty_mu_`, `nm_` strictly sequentially (no new lock nesting). New
  counter `synthesized_tombstones()` (+ `_for_test` forwarder). NEW API: `Database::pool_stats()`
  (`PoolStats`: capacity/live/peak-high-water/free/utilization),
  `GcStats.entries_retired/entries_reclaimed`, `health()` degrades at
  ≥80% pool utilization / fails at ≥95% (roadmap-fixed; early warning
  ahead of BT-1/D4 exhaustion). Memory-ordering audit extended
  (`KeyEntry::purged` + the GC SAFETY entry-retirement paragraph incl.
  the PITR-caveat resolution: purge is in-memory only; v26.1 coverage
  rules untouched). `LatchTable::erase` stays uncalled, documented
  (item 7's bounded horn; erasing under the immediate-return free list
  would race raw-mutex-pointer holders). EVIDENCE: BEFORE/AFTER churn
  driver (30 rounds, steady 400-key live set): live_versions
  200→6200 ratchet becomes FLAT 200 (exact live set), wall 19.1s→0.9s;
  `run_m3_purge_test` 29 controls in 5 blocks (lifecycle, pin-protection
  with the pinned txn still reading its pre-tombstone value, 24-round
  delete-then-reinsert flatness with exact 9600/9600-entry and
  `created-reclaimed==400` oracles, both health bands on a
  headroom-sized 1024-page fill, and the purge×checkpoint block: delta
  checkpoint over a purged key succeeds with exactly one synthesized
  tombstone, no D4 latch, and the recovery leg proves no resurrection
  across WAL rotation); mutants: PURGE-OFF rc9 6-FAIL (C4 green —
  isolation), NO-RETIRE rc9 7-FAIL (accounting identity = leak
  detector), NO-UNLINK ASan heap-UAF in `ensure_index`, SYNTH-OFF rc9
  5-FAIL reddening exactly the purge×checkpoint block (C1–C4 green);
  clean ASan 29/29 rc0. The checkpoint-integration fail-first was
  FOUND, not predicted: the first full-suite run failed the v19 soak
  with `noncommitted=5653351 threw=0`, `checkpoints=1` of ~60 (one
  delta checkpoint latched D4 on a purged key; every later commit
  fast-failed DatabaseFailed and the ckpt thread swallowed the throws);
  a deterministic 2s driver reproduces it pre-fix (`checkpoint aborted:
  1 dirty key(s) absent from the index ... first missing key: dk`, next
  put status 6, health level 2) and goes 8/8 post-fix — the on-disk
  delta is exactly the 37-byte synthesized `dk` tombstone. SCOPE,
  measured: monotonic-frontier churn flattens the
  heap but live pages still creep (11→50/25 rounds — emptied leaves
  stay linked); page-level `free()` callers + that acceptance shape ride
  M4's leaf unlink/merge. Release suite 533→562/0 (soak green again:
  checkpoints=54, zero noncommitted); DST 150/150; lint ratchet 33=33.

- **feat (v29 M3 slice 2/3, 2026-10-17): the G2 verification sites are
  live — `fence_unchanged` checks the slot generation FIRST (identity
  before content, W2/W3 closed), `Cursor::enter_leaf` verifies
  `leaf_gen_` after its latch and restarts from the last emitted key on
  mismatch (W1/W6 closed) — and the hazard-pointer machinery is
  DELETED, not wired. Two one-shot bump controls with exact mutant
  isolation; release suite 532→533/0; DST 150/150.** Per the HP4/ABA
  decision doc checklist items 3–5. FENCE SIDE: `LeafFence` gains
  `uint32_t gen` (captured in `find_leaf_crabbing_with_fence` under the
  same shared latch as the content fields); `fence_unchanged` compares
  `pool_.slot_generation(pool_.id_of(p))` before `mut_epoch`/count/min/
  max — the ordering is the point: a reused slot's `mut_epoch` restarts
  at 0 (alloc's memset) and CAN alias a captured epoch (the HP4 ABA),
  while the generation cannot; a mismatch rides the callers' existing
  re-descend path (`fence_recheck_retries` counts it; per the decision
  doc it is a normal control-flow event, never an error, never a health
  signal). `TestFence` + its capture/compare helpers carry `gen` too,
  so the CKV-008 remediation battery keeps working unchanged. CURSOR
  SIDE: `leaf_gen_` is captured latch-continuously in
  `enter_leaf_from_crabbing` (no gap ⇒ nothing to verify, just the
  baseline for the next switch) and under the PREDECESSOR's latch at
  switch time (sound: freeing the successor requires unlinking under
  this page's exclusive latch); `enter_leaf` verifies AFTER the latch,
  BEFORE any content read — the latch-then-gen-then-content ordering
  that also covers the decision doc's §7.2 latch-entry ABA (a thread
  blocked on a freed-then-reused slot's latch wakes holding the NEW
  occupant's latch). Mismatch ⇒ `note_cursor_restart()` + re-descend at
  the last emitted key under a new `lo_exclusive_` bound (keys are
  unique and ordered ⇒ the exclusive successor seek can neither
  re-emit nor skip; iterative by construction — the restart re-enters
  through the latch-continuous crabbing path, so no recursion; an
  empty descent sets `exhausted_`). `cursor_restarts` counter: BTree
  mutable atomic + `ChronoKV::cursor_restarts()` delegate beside
  `fence_recheck_retries` (the delete-churn table reads both). **HP
  MACHINERY DELETED** (decision §4.3 — both audits demanded
  redesign-not-wiring; the registry only ever covered cursor RESIDENCY,
  which the leaf latch already protects because any freer must take it
  exclusive to unlink, and it never covered the window that matters —
  `leave_leaf()` cleared the HP BEFORE the latch-free switch gap):
  `HazardPointer`, `HPRegistry` (incl. the H1 slot-recycling fix),
  `BTree::hps_`, `Cursor::hp_` + publish/clear/register/release, the
  two `hp_*_for_test` accessors, and the H1 review test — deleted WITH
  the machinery, recorded at both deletion sites (the leak class cannot
  recur without a registry; the cursor now holds no pooled resource
  beyond its RAII latches; discipline stays covered by the cursor
  batteries, the new controls, DST S3/S4/S5, and the sanitizer legs).
  SEAMS (TEST_HOOKS/STRESS only, zero production behavior change):
  `PagePool::bump_all_slot_generations_for_test` +
  `ChronoKV::test_bump_all_slot_generations` — CONTENT-INVARIANT
  invalidation (no page byte changes, so mut_epoch/min/max/count all
  stay equal and ONLY the generation checks can observe the bump: exact
  mutant isolation); the stress gap-hook dispatch gains exact-name
  strcmp branches — `gap_callback_leaf_latch` at `tree_leaf_latch_gap`
  and `gap_callback_cursor_switch` at `tree_cursor_leaf_switch` (both
  names start with 't', so the first-char scheme cannot separate them,
  nor from `tree_split_begin`). CONTROLS
  (`run_m3_gen_verification_test`, 4 checks, STRESS-gated like the m16
  battery, skip-PASS lines on release): **A (W2)** — one-shot bump at
  the writer's shared→exclusive gap: the fence refuses (hook fired,
  `fence_recheck_retries` delta ≥ 1) and the re-descended put lands
  byte-correct; **B (W1)** — one-shot bump at the cursor's leaf-switch
  gap: `cursor_restarts` delta EXACTLY 1 and the full 400-key scan
  stays byte-exact — the transparency leg doubles as the correctness
  proof of the restart logic (the risky new code). FAIL-FIRST, measured
  (scratch builds, driver-level): MUT-FENCE (the gen check deleted from
  `fence_unchanged`) → control A RED ALONE (`committed=1 fired=1
  retries_delta=0` — the silent-pass shape: the put "succeeds" on a
  fence that no longer detects reuse) with B fully green, rc 9;
  MUT-CURSOR (the `enter_leaf` check disabled) → control B's restart
  count RED ALONE (`restarts_delta=0`; transparency still green — with
  a content-invariant bump the data was never at risk, proving the gen
  check is the SOLE detector) with A green, rc 9; clean tree 4/4, rc 0.
  HONEST BOUND (decision doc §9, kept): the end-to-end
  reuse-corruption catchers stay UNREACHABLE until slice 3/M4 give
  `free()` its engine callers — slice 2 proves the detection-and-
  recovery machinery; the corruption mutants get run when the frees
  exist, recorded not faked. MEASURED (1 GiB sandbox, `-O0` bare +
  `ggc-min-expand=5`; floors, not ledger material): release full suite
  **532 → 533 PASS / 0 FAIL** (−1 the deleted H1 line, +2 the release
  skip lines), exit 0, banner 0.28.1; **DST 6 scenarios × 25 seeds =
  150/150 children green** (S3 gc-vs-scan, S4 cursor-vs-split and S5
  merge-grammar hammer the cursor paths under the baton scheduler — the
  restart machinery's first concurrency exposure); `make lint` green
  (all four TUs both hook modes, ratchet 33=33 — the new header code
  adds zero conversion warnings, member/init-list order -Wreorder-clean);
  STRESS-defs four-TU syntax gate green. Docs: ROADMAP M3 header →
  slices 1–2/3 landed, slice-2 record with the full control/mutant
  numbers, slice 3 → NEXT, progress row updated.

- **feat (v29 M3 slice 1/3, 2026-10-17): the pool-side slot-generation
  mechanism lands — invariant G1 is live in `PagePool::alloc()`, the
  HP4/ABA decision's chosen horn, with six unit controls and a measured
  no-bump mutant proof (4/6 red, driver rc 9). Production behavior
  UNCHANGED by construction; the G2 consumers (fence + cursor
  verification, HP deletion) are slice 2, inside the same milestone.**
  Per `docs/audits/2026-10-12-m3-hp4-aba-decision.md` checklist items
  1–2: `PagePool` gains a fixed-size per-slot generation table
  (`std::vector<std::atomic<uint32_t>>`, `capacity_/PAGE_SIZE` entries
  — 256 KiB at the engine's 256 MiB default, 1 MiB at PagePool's own
  1 GiB default; never grown). Pool-side, not header-side, by the
  decision's recorded rationale: the 32-byte `static_assert`-guarded
  `PageHeader`'s reserved bytes are already spent (CKV-008 took both
  halves for `mut_epoch`), and the generation belongs where its only
  mutator already is — the pool mutex in `alloc()`. **G1**: every
  alloc transition (free-list reuse AND virgin bump) bumps the slot's
  generation exactly once, under `mu_`, BEFORE the memset that makes
  the slot's contents reachable — release-ordered, pairing with the
  verifiers' acquire load after their latch acquisition (the ordering +
  32-bit-wrap arguments are the decision doc's §3, to be transcribed
  into the header's memory-ordering audit section with slice 2's
  consumers). Read side: `PagePool::slot_generation(id)` — null id
  reports 0 and live slots count from 1, so a zero-initialized capture
  can never alias a live slot; same no-bounds-check hot-path contract as
  `get()` — and `BTree::slot_generation(id)` forwarding through the
  tree face (consumers never touch the pool directly; documented as
  distinct from the TREE-wide `generation()` debug counter). `free()`'s
  v25.1 HP4 deferral comment ("the free-list reuse is tagged or Version
  allocations are never reused") is replaced by the decision's
  resolution: tagged horn, immediate free-list return, epoch deferral
  rejected (writer descents don't pin; deferral re-couples reuse to the
  slowest pin), HP machinery deletion scheduled for slice 2. The
  mechanism does NOT sit dead the way the HP registry did: it lands
  with its controls, and its production consumers land in the same
  milestone — the no-consumer-yet window is one slice, by plan, and
  `alloc()`'s only new instruction is a `fetch_add` under a mutex it
  already holds. CONTROLS (six, in `run_page_pool_selftest`): null id →
  0; virgin slots start at 1 with independent counters; free→alloc
  reuse hands back the same slot id; reuse bumps exactly once with
  siblings untouched; per-slot monotonic across repeated free/alloc
  cycles; and the ABA pair itself — a generation captured before a free
  is stale (`gen+1`, `!= captured`) against the reincarnated slot.
  FAIL-FIRST, measured (scratch build, driver-level): deleting alloc's
  bump line reddens 4 of the 6 (all four generation-value controls; the
  2 green — null-id and same-slot-id reuse — don't consult the counter
  by construction), `fails=4`, driver rc 9; clean tree 6/6, rc 0.
  SELF-CORRECTION recorded: the ABA control's first cut asserted
  `slot_generation(e) != 1` — true under the no-bump mutant (0 != 1),
  i.e. VACUOUS exactly where it mattered; caught by running the mutant,
  strengthened to capture-then-compare, re-proven in both directions
  (this is the fifth time this arc's discipline paid: the m2 stale-id
  mutant that was a no-op, the soak's empty-delta hole, the v18.2
  revert that survived, the in-sample band claim — detectors must be
  run against their mutants, not reasoned about). Suite: release
  **526 → 532 PASS / 0 FAIL**, exit 0, banner 0.28.1, measured on the
  1 GiB sandbox at `-O0 + ggc-min-expand=5`. BASELINE CORRECTION,
  recorded honestly: the gate-hardening entry above says "the
  519-check C++ suite" — that cited 52fd43c's count; 6bcb212's [k,k]
  release-mode checker added 7 legs AFTER it, so the pre-slice baseline
  was 526 (verified by counting 6bcb212's added check lines). `make
  lint` green: -Werror across all four TUs both hook modes, ratchet
  33=33 (the new header code adds zero conversion warnings), header
  hygiene incl. the STRESS 2-TU leg; STRESS-defs syntax gate green.
  Next: slice 2 (G2 verification sites — `LeafFence.gen` +
  `fence_unchanged`, `Cursor::leaf_gen_` + `enter_leaf` with
  restart-from-last-emitted-key, HP machinery deletion) then slice 3
  (first `free()` callers: purge shape with the PITR-horizon caveat,
  KeyEntry retirement via the epoch reclaimer with unlink-before-retire,
  stats/health thresholds, memory-ordering audit extension, acceptance
  battery).

- **fix+test (v29 M1 gate hardening round 2, 2026-10-17): DEFECT 5 —
  half-dead BASELINE legs weren't caught: one dropped baseline workload
  row passed the whole gate while silently shifting ratios (-15.9%
  measured), and a truncated baseline leg reddened the nightly with
  FALSE engine regressions. Found by the M1 owner reviewing the
  hardening itself; reproduced with fixtures before the fix; fixed in
  two layers; five new controls (selftest now 19 controls + 2 meta =
  21 checks).** The
  previous entry's hardening left a hole: `verify-ledger` required only
  >=1 valid row per engine, and `check`'s MISSING verdicts covered
  ChronoKV series only — baseline workload completeness was unguarded.
  BEFORE (both probes on copies of promoted night N7, pre-fix tool):
  *probe 1* — drop the single `fillseq` row from `sqlite-full.tsv`:
  verify-ledger exit 0 ("all 7 engines carry >= 1 valid data row"),
  check exit 0 ("30 series checked: 0 REGRESSION, 2 WARN") — while the
  geomean denominator fell from five engines to four and
  `chronokv-group/fillseq` silently moved 0.2069 -> 0.1740, a
  **-15.9% ratio shift** (it surfaced only as a WARN; the same drop on
  a FAST baseline — lmdb owns readrandom — shifts ratios UP and can
  MASK a real regression, the direction that motivated refusal over
  recomputation). *Probe 2* — truncate `rocksdb-sync.tsv` to 1 data
  row: verify-ledger exit 0, check exit 1 with **two FALSE
  REGRESSIONs** (`chronokv-async/fillrandom 0.206 < floor 0.231`,
  `chronokv-group/fillrandom 0.204 < floor 0.218`) — the geomean lost
  its slowest member and a dead vehicle leg would have been reported as
  an engine regression (cry-wolf: the re-run policy would have burned
  its one dispatch on nothing). FIX, two layers: (1) `verify-ledger`
  compares each engine's valid-workload SET to the calibrated one —
  reference: the most recent committed `bench/results/promoted/` week,
  resolved script-relative (any CWD; `--expect-from` overrides; an
  internally-inconsistent reference is a hard tool error, never a
  silent pick). Equality is BOTH directions: missing workloads fail and
  an UNEXPECTED new workload fails too — a workload-list change is a
  ledger-format change that owes a recalibration commit (the promoted
  reference updates with it), the same rule the geometry refusal
  already enforces. (2) `check`'s geomean went STRICT
  (`baseline_geo_strict`): a ratio is REFUSED whenever any of the five
  baseline legs lacks a valid row for that workload — fail-tier
  `BASELINE-GAP` verdict naming the offending engines and saying what
  it is ("a vehicle failure, not an engine regression"); the summary
  line gains the count; MISSING never double-counts a series already
  flagged BASELINE-GAP (attribution stays exact); ratio-less-by-design
  workloads (the chronokv-exclusive family — no baseline rows even in a
  pristine night, no band series) are explicitly NOT gaps. AFTER (same
  fixtures): probe 1 -> verify-ledger exit 1 (`WORKLOAD-SET
  sqlite-full.tsv 14 valid rows ... missing=['fillseq']`) and check
  exit 1 with exactly 2 BASELINE-GAP (both ckv fillseq series,
  sqlite-full named), **0 REGRESSION**; probe 2 -> verify-ledger exit 1
  listing all 14 missing workloads, check exit 1 with **28 BASELINE-GAP
  and 0 REGRESSION** (both false verdicts eliminated); masking-direction
  control (lmdb's readrandom row dropped) -> both ckv ratios refused,
  never computed inflated; extras-direction control (injected `brandnew`
  row) -> exit 1 `unexpected=`; pristine night -> 0 BASELINE-GAP (no
  false refusals). NEGATIVE controls: all seven promoted nights re-run
  through BOTH layers — verify-ledger 0, check 0, WARN counts identical
  to pre-fix (N1:6 N2:14 N3:0 N4:1 N5:1 N6:6 N7:1), 0 MISSING, 0
  BASELINE-GAP. Invariants honored: `bench/arena_bands.json` UNTOUCHED
  (sha256 unchanged; recomputation over the seven nights identical in
  keys/n/provisional flags with max relative float drift 1.143e-14 —
  the strict geomean sums in BASELINE_ENGINES order instead of parse
  order; zero verdict drift), tolerance untouched (0.15), LOO output
  unchanged (same 6 held-out regressions on N2/N6 — recorded, awaiting
  the band-policy decision, NOT widened), no engine file touched.
  Selftest: `scripts/test_arena_gate.py` grows the defect-5 section
  (both owner probes + masking + extras + pristine controls, asserting
  parsed verdict COUNTS from the summary line rather than substrings)
  -> 19 controls + 2 meta-controls (21 checks), all green in ~1 s; the `arena-gate-selftest`
  push/PR job picks it up with no workflow change (its comment now says
  five). Docs: bench/README gate-hardening record gains the defect-5
  bullet with the measured numbers; ROADMAP item-4 addendum gains
  paragraph (5), owner-review provenance kept honest.

- **fix+test (v29 M1 gate HARDENING, 2026-10-17): four verified defects
  in the arena regression gate enabled earlier the same day — each
  reproduced with fixtures BEFORE the fix, each fixed behind fail-first
  controls, all controls wired into a new push/PR job. Bands and
  tolerance UNTOUCHED (`bench/arena_bands.json` sha256 identical, git
  diff empty); no engine file touched.**
  * **DEFECT 1 — the ledger-completeness step passed GREEN on dead
    legs.** The ci.yml shell count `n=$(grep -cvE '^#|^[[:space:]]*$'
    "$f.tsv" 2>/dev/null || echo 0)` is broken two ways: `grep -c`
    prints `0` AND exits 1 when nothing matches, so `|| echo 0` appended
    a second `0` — `n="0\n0"`, `[ "$n" -lt 1 ]` died with `integer
    expression expected` (rc=2), the if-branch never ran, and EMPTY or
    COMMENT-ONLY TSVs passed; and any non-comment line counted, so a TSV
    containing only crash stderr passed. BEFORE (fixtures, step body
    verbatim): empty → `raw n bytes: [0|0]`, if-test rc=2, GREEN;
    comments-only → same, GREEN; stderr-only → `n=[3]`, GREEN; full step
    over a results dir with THREE dead legs → `STEP PASSED GREEN`, exit
    0 (only the missing-file case was caught: bare `n=[0]`). FIX: the
    logic moved into `scripts/arena_bands.py verify-ledger --night DIR`
    (per engine of the seven: file exists; ≥1 row with ≥7 tab-separated
    fields and numeric `ops_per_sec > 0` — stderr text has neither tabs
    nor a numeric fourth field; `memory`-style 0-ops rows don't count as
    throughput rows but no engine depends on them for validity), called
    from ci.yml. AFTER (same fixtures): empty exit 1
    `ZERO-VALID-ROWS`, comments-only exit 1, stderr-only exit 1 (first
    lines echoed for triage), missing-file exit 1 `MISSING-FILE`;
    pristine promoted night exit 0.
  * **DEFECT 2 — `check` ignored missing series.** `cmd_check` iterated
    the night's rows, not the bands' keys: a leg that died mid-run just
    produced fewer verdicts. BEFORE (promoted N7 mutated): dropped
    `ycsb_c.read` row → "29 series checked … exit 0"; deleted
    `chronokv-async.tsv` → "15 series checked … exit 0";
    `chronokv-group.tsv` truncated to 3 data rows → "18 series checked …
    exit 0". FIX: after the verdict loop, every `bands["ratio"]` key
    absent from the night — and every chronokv-engine `bands["rss"]` key
    absent — is a MISSING / MISSING-RSS verdict at fail tier (exit 1);
    the summary line gains the MISSING count. AFTER (same fixtures):
    (a) exit 1, 2 MISSING (ratio + rss rows of the dropped workload);
    (b) exit 1, 36 MISSING; (c) exit 1, 30 MISSING. Negative controls:
    the pristine night and ALL SEVEN promoted nights re-check exit 0
    with 0 MISSING and byte-identical WARN counts to the pre-fix runs —
    no false positives, no verdict drift on valid ledgers.
  * **DEFECT 3 — the "0 REGRESSION on all 7 calibration nights"
    self-consistency claim (this file's previous entry, the ROADMAP item
    4 record, and the enablement commit message) was VACUOUS BY
    CONSTRUCTION.** The floor is `band_min × (1 − tolerance)` and each
    calibration night is one of its own band's samples, so every night's
    ratio ≥ band min > floor: in-sample REGRESSION cannot fire for ANY
    series of ANY calibration night. Worked example (measured):
    `chronokv-group/ycsb_c.read` band min 0.501 — that IS night N2's own
    ratio — floor 0.426; N2 passing "consistently" carried zero
    information. The claim is corrected in place in the ROADMAP (kept,
    annotated — not deleted) and superseded by `arena_bands.py loo`:
    leave-one-out cross-validation, bands from the other six nights
    (`--min-nights 6`), held-out night checked at the same tolerance.
    **LOO numbers on the green week (recorded; NOT acted on — the task
    rule and the band-update policy both forbid widening): 6 held-out
    REGRESSIONs across 2 of 7 nights, concentrated entirely in the
    read/scan-heavy series.** N2 (Xeon Platinum 8573C): group/ycsb_c.read
    0.501 < floor 0.746; async/ycsb_c.read 0.730 < 0.806. N6 (EPYC
    9V74): group/ycsb_e.insert 0.522 < 0.826; group/ycsb_e.scan 0.523 <
    0.826; async/ycsb_e.insert 0.861 < 0.869; async/ycsb_e.scan 0.861 <
    0.869 (the async pair under the floor by 0.9%). N1/N3/N4/N5/N7: 0
    each. Reading: for ycsb_c/ycsb_e the cross-CPU-MODEL spread exceeds
    the 15%-below-min tolerance, i.e. the committed floors for those
    series are only as general as the observed hardware pool — they hold
    for the live gate (all seven nights are IN the bands, so any night
    resembling an observed one passes) but a genuinely new CPU shape
    could false-red them. `loo` is informational by design (always exit
    0; an automatic reaction would be exactly the in-flight widening the
    policy forbids); the numbers are a standing band-policy question for
    the M1 owner (hardware-conditional bands vs. fleet convergence vs.
    an engine-side look at the ycsb_c/e variance).
  * **DEFECT 4 — no synthetic-regression positive control existed for
    the gate itself** (no test script, no CI job — verified BEFORE:
    `ls scripts/test_arena_gate.py` → No such file; `grep
    arena-gate-selftest ci.yml` → nothing). Characterization on the
    current check (what the controls must pin): a promoted night with
    chronokv `ops_per_sec` × 0.5 → exit 1 with 27 REGRESSION verdicts;
    × 0.95 → exit 0, 0 REGRESSION, 2 WARN. FIX: `scripts/
    test_arena_gate.py` — 14 controls (defect 1's four fixtures +
    negative; defect 2's three mutations + negative; defect 3's LOO
    structure — completion, all seven held-out lines, totals line, with
    the REGRESSION numbers echoed to the CI log but deliberately NOT
    asserted, band policy being a maintainer decision; defect 4's ×0.5
    red / ×0.95 green) plus 2 meta-controls (sha256 of
    `bench/arena_bands.json` and a tree-hash of the promoted week
    identical before/after — the suite never mutates what it judges).
    All fixtures are built from the committed promoted TSVs in tempdirs:
    stdlib-only, no network, no runner hardware, ~0.6 s. Wired as the
    `arena-gate-selftest` job on EVERY push/PR and nightly, in
    `ci-passed`'s needs with NO skip tolerance (locally verified: push
    with arena skipped → 0; nightly green → 0; arena red → 1; selftest
    red → 1). AFTER: `python3 scripts/test_arena_gate.py` → all 14+2
    PASS, exit 0.
  Files: `scripts/arena_bands.py` (verify-ledger + loo subcommands,
  MISSING verdicts via a `run_check` core shared with loo, docstring),
  `scripts/test_arena_gate.py` (new), `.github/workflows/ci.yml`
  (completeness step → verify-ledger with the defect recorded in its
  comment; new selftest job; ci-passed needs), `bench/README.md` (gate
  section hardened record + usage), `docs/ROADMAP.md` (M1 header, item 4
  correction-in-place + hardening addendum, progress row). Suite/engine
  untouched: the 519-check C++ suite is unaffected by construction (no
  compiled file changed).

- **bench+ci+docs (v29 M1 COMPLETE, 2026-10-17): the green week closed
  7/7, the noise bands are calibrated and committed, the arena
  regression gate is ENABLED, and the README performance section
  shipped — constraint 2 is satisfied and the rewrite sequence (M3
  first) is unblocked.** The week on the re-merged single-job vehicle:
  #82/#88/#91/#96/#97/#98/#99 (2026-10-11..17), all green; the mid-week
  commits (52fd43c…6bcb212) were verified engine-binary-neutral before
  pooling (52fd43c's engine diff is pure deletion under the
  never-defined `CHRONOKV_VERIFY_FULL` — the seven nights measure one
  engine). All 49 rows promoted in-tree at
  `bench/results/promoted/green-week-2026-10-11_17/` (232 KB, every TSV
  carrying its rule-9 methodology header) — the acceptance's permanent
  evidence and the first application of the promotion rule.
  **Calibration** (the pre-built `scripts/arena_bands.py` over the seven
  nights → `bench/arena_bands.json`, CALIBRATED stamp at 7 ≥
  min-nights): the week re-measured the design fact — FOUR distinct
  runner CPUs across seven nights (EPYC 9V45 ×3, Xeon Platinum 8573C,
  EPYC 7763 ×2, EPYC 9V74) — so the primary tier is ratio-normalized
  (ChronoKV ops/s ÷ same-night five-baseline geomean). Calibrated
  medians: the durable write side at 0.20–0.44× (fillseq 0.20–0.22,
  fillrandom 0.27, ycsb_a 0.38–0.41, overwrite 0.41–0.44) — M5's
  measured before-column, exactly as claim 3 predicted; readrandom
  0.57–0.63× (lmdb's mmap owns it); ycsb_b reads 0.68–0.70×; and
  read-only ycsb_c 1.05–1.09× with scan-heavy ycsb_e 1.05–1.09× —
  above the power-loss-class field. Raw bands stay hardware-conditional
  secondary (≤3 nights per CPU, honestly stamped PROVISIONAL); the RSS
  tier bands cross-night directly and carries the deletechurn
  ratchet-watch — flat at 201.2–201.5 MB across all seven nights (P1's
  before-column; M3's acceptance tightens WARN to gate). Night-7
  self-consistency: 30 series, 0 REGRESSION, 1 WARN inside floor (async
  readrandom 0.470 vs median 0.565 — the widest band, honestly earned).
  **Gate enablement** followed the recipe the ci.yml comment itself
  carried: `arena` added to `ci-passed`'s needs — skip-tolerated on
  push/PR where the schedule-only job does not run (a flaky shared
  runner still never blocks a PR; the teeth are on the nightly, where
  "the gate must never learn to be ignored" is the point) — plus two
  enforcement steps: ledger completeness (all seven engines must
  produce DATA ROWS; a crashed leg writes its stderr INTO the TSV, so
  file-exists ≠ rows-exist) and the `check` verdict (exit 1 REGRESSION,
  exit 2 geometry mismatch — mixed geometries refuse to band). Policies
  landed explicitly: a red gate re-runs ONCE via dispatch (bands
  normalize the CPU lottery, not contention; two consecutive
  REGRESSIONs = bisect, don't re-run); bands are recomputed only in a
  dedicated commit citing the engine change that moved them (M5's
  writer thread is the planned one), never widened in-flight — the
  ratchet rule applies to bands. **Promotion decision (item 5):** raw
  nightly rows live as run artifacts (90-day retention); a milestone
  that CITES rows commits that night's TSVs under
  `bench/results/promoted/` in the citing commit — verifiable forever,
  no bot, no nightly commit churn. **Arena-side build cache: REJECTED
  as measured low-value** — the ~57–77 min nightly is dominated by the
  measured runs; apt + baseline build is minutes of that, and caching
  built baselines across nights risks binary↔recorded-version
  provenance drift, the fairness protocol's core asset; revisit only at
  the 120-min cap (the test-suite-side cache question was already
  answered by `build-shared` + the recorded ccache rejection). The
  actions v4→v5 bumps — explicitly deferred past the green week by
  52fd43c — ride along (checkout ×9, upload ×8, download ×3; clears the
  Node-20 deprecation warning flood). **README §Performance** (the
  acceptance's third leg) publishes the honest current picture with
  every number tied to its methodology: vehicle, pinned baselines
  (`bench/third_party/versions.txt`), the ratio-band rationale, the
  write-side gap named as M5's before-column, the deletechurn RSS as
  P1's, and the policy links. bench/README: status → M1 COMPLETE + the
  full bands/gate/policy section. ROADMAP: M1 header → COMPLETE,
  acceptance → MET (all three legs cited), items 1/4/5 → DONE/DECIDED
  with the numbers, progress row → one-line COMPLETE, constraint 2 →
  **SATISFIED 2026-10-17** (the rewrites now run against a GATED
  ledger), M3–M7 row + next-commits → UNBLOCKED, M3 first (its gates
  already wait: the S6 DST grammar, the deletechurn ratchet-watch, the
  HP4/ABA decision checklist). Engine/test/build files UNTOUCHED — the
  519-check suite at 6bcb212 stands unchanged by construction; this
  commit is bench artifacts + tool config + CI + docs. Tonight's
  nightly is the first GATED arena night; this commit's push run
  exercises the skip-tolerance path (arena skipped on push, `ci-passed`
  must stay green).

- **test+docs (v30 M0 gap closures G-3 + G-1, 2026-10-13): the `[k,k]`
  point-range equivalence row gets its release-mode checker, and the
  WAL-3 row's CONFIRM resolves by indexing — the INVARIANTS.md gap list
  shrinks 8 → 6, by evidence only.** G-3: `run_kk_equivalence_test`
  (7 legs) states the Audit-2 matrix row's OWN claim as a named checker
  — the value half (single-key range scan == point read at latest, at
  absence, at deletion, and at a transaction snapshot under version
  churn) and the conflict half (a user-API transactional `range_scan`
  over an absent key conflicts with a concurrent insert — the TXN-2
  route's user-level twin — with a no-false-positive control for inserts
  outside `[k,k]` and the v20.1-blocker-3 inclusive-upper-bound pin at
  the user API). Both halves mutation-proven with exact leg isolation:
  cursor hi made exclusive → legs A/D RED, conflict legs green;
  `has_phantom_in_range`'s `<=` made `<` → legs E1/E3 RED, value legs
  green. MEASURED co-flip sets (scratch, `CKV_ONLY_REMEDIATION`): the
  hi-exclusive mutant also reddens CKV-002a/e, CKV-006, CKV-003c,
  CKV-005; the boundary mutant reddens A2's TXN-1 scan/update detector
  — and A2's get-based check-then-insert detector does NOT flip: its
  absent-key route is evidently not validated through the range
  predicate, contradicting the TXN-2 fix-summary prose. The prose stays
  unrewritten (historical record); the test comment and the matrix row
  carry the measured truth — the battery is the truth. Honest scoping:
  the co-flips show the underlying LINES were not unguarded; what was
  missing was a checker stating the row's own equivalence claim
  (invariant → checker, not invariant → some test that happens to
  break), which is exactly what v30 M0's mapping deliverable demands.
  G-1 (WAL-3): resolved WITHOUT new code, by indexing — the SRC-C
  remediation leg arms `RotFinalDirFsyncFail` at the very call site the
  WAL-3 fix corrected (`fsync_dir(manifest_path())`, anchor comment on
  the line) with a fired-witness (`charge_remaining==0`) and pins the
  counted-not-fail-stop contract; the target-directory half is marked
  STRUCTURAL with the honest boundary: the fault fires regardless of the
  fsync_dir ARGUMENT, and no in-process proof of a wrong/missing fsync
  can exist (`_exit()` testing keeps the page cache — the decision doc's
  WAL-2 boundary sentence applies verbatim), so the mechanical
  target-level proof is v30 M3's dm-flakey leg. Docs: INVARIANTS.md
  `[k,k]` row → PROVEN with both mutants and co-flip sets named; WAL-3
  row → CHECKED (handling) + STRUCTURAL (target) with the boundary
  recorded; §5 G-1/G-3 → CLOSED with evidence (entries retained per the
  file's own never-delete rule). Suite: release **519 → 526 PASS / 0
  FAIL**, exit 0, banner 0.28.1; `make lint` green (ratchet 33=33,
  header hygiene); measured on the 1 GiB sandbox at `-O0 +
  ggc-min-expand=5`. Engine: zero change (test + docs only).

- **bench-tooling (v29 M1 remainder, pre-built 2026-10-13):
  `scripts/arena_bands.py` — the noise-band computation and regression
  checker for the arena ledger, staged ahead of the green week's
  completion so "noise bands + gate enablement" is a calibration
  exercise, not a design one.** Stdlib-only, transport-agnostic (inputs
  are local directories of ledger TSVs, e.g. extracted
  `arena-ledger-<run_id>` artifacts); subcommands `band` (compute +
  JSON), `check` (verdicts against a band file; exit 1 = REGRESSION,
  2 = geometry mismatch — CI-ready but UNWIRED by design: no ci.yml
  change during the green week), `report` (cross-night digest).
  THE DESIGN FACT ITS FIRST RUN MEASURED (rule 13 applied to the
  tooling itself, over the three banked nights #82/#88/#91 downloaded
  from the run artifacts): the nightly runner CPU differs NIGHT TO NIGHT
  even on the re-merged single-job vehicle — AMD EPYC 9V45, Intel Xeon
  Platinum 8573C, AMD EPYC 7763 across the three headers. Same-night
  cross-engine fairness holds by construction (one job, one machine —
  the headers are the enforcement, and they caught the earlier two-runner
  split); cross-night RAW throughput is a hardware lottery (observed
  ratio spreads 4–55% per workload). Bands are therefore two-tier:
  PRIMARY ratio bands (ChronoKV ops/s ÷ the same-night baseline geomean
  — hardware-normalized because numerator and denominator ran on the
  same CPU), SECONDARY raw bands grouped by (cpu model, engine,
  workload) — degenerate at one night per CPU and stamped as such, not
  hidden. The RSS tier bands cross-night directly (memory is
  hardware-independent: the first three nights' ckptload/coldrecovery
  RSS varied 0.0%) and carries a ratchet-watch (WARN above band max ×
  factor) whose deletechurn series is P1's before-column — M3's
  acceptance ("stabilizes instead of ratcheting") will tighten it into a
  gate. Provenance discipline per rule 9: every band records its nights,
  CPUs and pinned geometry; geometry mismatches REFUSE to mix (the
  arena job's own comment is the authority: "geometry changes are
  ledger-format changes"); everything below `--min-nights` (default 7 =
  the green week) stamps PROVISIONAL — reportable, not gateable. Validated
  end-to-end on the real artifacts: bands from nights 1–2, night 3
  checked against them (30 series, 0 regressions, advisory note),
  three-night digest printed. Calibration choices left honestly open for
  the M1 owner after the week: the floor formula (band min − tolerance)
  and the tolerance default (15%) are provisional knobs, and the
  gate-wiring shape (in-job step vs a ledger-promotion consumer) rides
  the promotion decision. No engine/test/CI change; the 519-check suite
  state of `52fd43c` is untouched.

- **docs (v30 M0 seeded early, 2026-10-13): `docs/INVARIANTS.md` — the
  living invariant→checker→CI-job matrix exists ahead of its milestone.**
  38 rows across 8 families (publication/replay order, durability/WAL,
  MVCC-SSI-epoch, tree/pages, checkpoint/recovery, backup/PITR,
  health/lifecycle/API, measurement), every cell read out of the tree at
  `52fd43c` rather than transcribed from the audits: enforcement anchor
  (function-scope, as-of tagged per the reading rules), the machine
  checker, the CI vehicle that would catch a break, and an honest status
  (PROVEN / CHECKED / STRUCTURAL / PENDING / CONFIRM). Contents the seed
  could only write now, after this arc: the **P1 ID collision** recorded
  with a resolution proposal (audit §16's PITR-read-only P1 vs the v29 M4
  page-steady-state P1 the reading rules reserve — disambiguated
  P1-PITR/P1-PAGE from this file forward, re-anchor owed to v30 M0);
  the **break-proof ledger** (§3) indexing every recorded scratch-build
  demonstration from the v29 arc — item 1's live pre-fix-flag/post-fix-pass
  TXN-1 proof, item 2's four-mutant DST matrix (incl. the epoch-gate
  off-by-one caught by the pinned-block witness alone), item 3's
  old-GREEN/new-RED side-by-side proofs, item 4's W2T/W2V witness
  mutations, the v18.2 three-mutant isolation, SRC-E4's M6 — plus the
  0.28.0/v25.x/v26.1 arcs by milestone; the **G1–G3 rows** carrying the
  M3 decision doc's proposed invariants with their pre-recorded fail-first
  plan; and the **§5 gap list** (eight items — WAL-3's proof indexing,
  I3/R1 assert-class demos, [k,k] release-mode coverage, delta-matrix
  indexing, T1's demo, the P1 re-anchor, the error-lines ledger, G1–G3
  activation) which is v30 M0's inherited worklist: the milestone's own
  acceptance ("breaking any invariant in a scratch build fails at least
  one CI gate — proven once per invariant, recorded in the audit trail")
  now has a document to record into. §6 is the maintenance rule that makes
  it living: any commit touching an invariant, checker, proof or vehicle
  updates its row in the same commit; rows are never deleted; the gap list
  shrinks only by evidence. ROADMAP: v30 M0 section gains the
  seeded-early pointer (the milestone inherits rather than authors).

- **engine-cleanup+test (post-M2 hardening round 2, 2026-10-13): the
  audit's vacuous-test enumeration is now EMPTY — both beyond-arc
  remainders closed, by deletion with proof.** Finding (5):
  `verify_full()` — the assert-based expensive-tier chain sweep behind
  `CHRONOKV_VERIFY_FULL`, a define no build ever set ("compiled never",
  per the audit; and non-compiling dead code between the v24
  using-directive removal and its M-cleanup repair) — is DELETED rather
  than wired, because it is SUBSUMED: `verify_version_chains()` (bool +
  reason; wired into the crash-fuzz child verification and the v23
  Phase C battery) enforces strict descent + committed-above-
  uncommitted, which mathematically imply its other two checks — a
  cycle must revisit a node, and any revisit violates strict descent
  (equal ts trips `ts >= prev_ts`, higher trips it, and a finite chain
  cannot descend forever), while a duplicate commit_ts is the equality
  case of the same rule. One wired verifier that REPORTS beats two
  where the second aborts on assert and compiles nowhere; the deletion
  site carries the subsumption argument as a permanent comment, and
  `verify_version_chains` gains the sole-verifier note. The dead call
  site (v15 concurrent-commits test, behind the same never-set define)
  goes with it. Finding (7): the `conc: no crash` check read a `crash`
  flag no code path ever set — noise, not an oracle: the process
  reaching the real checks IS the no-crash proof. Deleted with its
  flag; the test's four real oracles (duplicate / fabricated /
  key-value pairing / observed-count) are untouched. Suite: release
  **520 → 519 PASS / 0 FAIL** (the noise line removed), exit 0, banner
  0.28.1; `make lint` green (ratchet 33=33 — the header changed — and
  header hygiene incl. the STRESS 2-TU leg); STRESS-mode syntax gate
  green over all four TUs; measured on the 1 GiB sandbox at `-O0 +
  ggc-min-expand=5`. The engine diff is deletion-only (dead code +
  comments). Adjacent CI hygiene DEFERRED by explicit decision: the
  actions v4→v5 bumps (which would clear the 37 Node-20 deprecation
  warnings every run emits) wait until the arena green week completes —
  the nightly vehicle is constraint 2's clock and a cosmetic infra swap
  is not worth any mid-week perturbation; they fold into the M1
  remainder, which also owns the true open piece of "CI build cache"
  (the test-suite side is already delivered by build-shared with the
  ccache rejection recorded in ci.yml; what is left is arena-side
  baseline caching, M1-owner material). ROADMAP: the v29 state bullet's
  "remaining two beyond-arc remainder" sentence rewritten to the
  closed state with both proofs named.

- **docs (v29 M3 decide-before-build, 2026-10-12): the HP4 / page-reuse
  ABA decision is MADE and in-tree —
  `docs/audits/2026-10-12-m3-hp4-aba-decision.md`.** ROADMAP §M3 requires
  "the ABA (HP4) decision made **before** implementation"; this discharges
  it while constraint 2 (the arena green week) still gates the milestone
  itself. Decision, grounded in a full read of the reference windows at
  `33aab1b`: **generation-tagged page slots** — a pool-side per-slot
  `atomic<uint32_t>` table (fixed at construction; `alloc()` bumps under
  the pool mutex BEFORE the memset), verified after every latch handoff
  and before any content trust — which is exactly TWO sites, because the
  window inventory (W1–W6, function-anchored) shows crabbing is
  latch-coupled within descents and cursor residency is latch-protected;
  the unprotected windows are the cursor's leaf-switch gap (where the HP
  machinery — cleared *before* the gap — never even covered its own
  headline case) and the writers' shared→exclusive upgrade gap (where
  reuse can alias the full CKV-008 fence quadruple: `alloc()`'s memset
  restarts `mut_epoch` at 0). **`free()` returns slots immediately** —
  epoch-deferred page reuse was rejected because writer descents do not
  pin reader slots (covering them means `reader_mu_` on the commit hot
  path or a second barrier protocol) and because deferral couples reuse
  latency to the slowest pin (a long-lived `RangeScanStream` would
  ratchet the high-water — against M3's own acceptance). The heap layer
  (`Version*` today, `KeyEntry*` with M3's purge) **stays on the v23
  Phase C epoch-pinned reclaimer**, with the E-series argument re-derived
  for entries (reclamation is gc-pass-internal; readers and checkpoints
  pin slots; unlink-under-latch precedes `retire()`), not assumed. The
  governing split, proposed as canonical invariants **G1–G3** for the v30
  M0 matrix: *value-identity references are made safe by verification;
  address-identity references by deferral.* The **hazard-pointer machinery
  is deleted in M3, not wired** — both audits demanded redesign over
  wiring, the redesign that survives review IS the generation check, and
  the suite inventory shows zero test consumers (one historical comment).
  Also recorded: no format change anywhere (page images are never
  persisted — checkpoints serialize logical entries — so the gen table is
  memory-only and recovery rebuilds fresh; v29's first format change
  remains M6's PITR deltas); the latch-entry ABA argument (wake on a
  recycled slot's mutex ⇒ check gen AFTER latch, BEFORE content — G2's
  ordering is load-bearing); the 32-bit wrap argument (one bump per
  slot-reuse vs CKV-008's per-mutation epoch — eleven more orders of
  headroom); the measure-first plan (handoff-check overhead in the arena
  legs, delete-churn high-water with `cursor_restarts` published beside
  it, latch-table bound, stream immunity as a measured property); and the
  fail-first mutation gates for M3's landing — including the honest
  record that the fence-gen mutant is UNREACHABLE until M4 supplies the
  frees (recorded, not faked, per the S5-grammar lesson). Explicitly
  deferred to M3 implementation: the purge-trigger shape (with the PITR-
  horizon eligibility caveat — v26.1's delta-filtering lesson applies to
  physical purge), cursor-restart positioning cost, and the
  capture-vs-second-read detail. ROADMAP: M3 header + decision paragraph
  + progress row updated; the implementation itself remains gated on
  constraint 2.

- **test (v29 M2 post-item hardening, 2026-10-12): the v18.2
  dirty-clear filter gets a dedicated regression — the guard the
  de-vacuated soak provably cannot reach.** Measured, not argued: a
  scratch revert of the filter (clear `dirty_since_ckpt_`
  unconditionally instead of only `dcts <= ckpt cts`) passed the 30 s
  soak — 253k commits, 58 checkpoints — and then the FULL 515-check
  suite unmoved. Two reasons, both verified: at ~40 writes/s/key a
  wrongly-cleared key is re-dirtied within milliseconds and the
  surviving WAL segment re-covers every hot key; and, deeper, the
  capture→clear race the filter closes is STRUCTURALLY CLOSED under the
  current locks — `commit_txn` holds `checkpoint_mu_` SHARED across its
  whole span including the dirty marking while `checkpoint()` holds it
  EXCLUSIVE across the entire `checkpoint_locked` body (writer-
  preferring), so no interleaving can produce a `dcts > cts` dirty entry
  any more. The filter remains load-bearing as defense-in-depth (lock
  scopes are implementation, not frozen contract, and `backup_to()`
  already drives `checkpoint_locked` under a different call shape), so
  the regression pins it WHITE-BOX through two hooks-gated seams —
  `ChronoKV::mark_dirty_for_test` (replicates the commit-side max-dcts
  merge rule) and `dirty_pending_for_test` — in the v24 Fix-12
  phantom-race test's own shape: construct the state the race used to
  produce, assert the protection, and keep a control that fails if the
  assertion is trivial. The construction is faithful rather than
  arbitrary: the injected dcts is `published()+1`, exactly where the
  next real commit lands, so the end-to-end leg steps into the retained
  mark the way the racing commit's value would have. Five checks:
  witness (future-dcts mark pending pre-checkpoint), CKV-004R coverage
  must NOT refuse the future-dcts entry, oracle A (the entry SURVIVES
  the clear), oracle B control (the captured entry whose dcts equals the
  checkpoint cts IS cleared — proves the clear ran and pins the `<=`
  boundary), oracle C (real post-race update + a third checkpoint + two
  rotations + recovery reads every value exactly). THREE mutants,
  driver-level, each reddening exactly its own oracle and nothing else:
  unconditional clear (the v18.2 revert) → A RED (`pending=0 want=4`);
  `<=` → `<` → B RED at the boundary (`decoy pending=3 want=0`); no-op
  clear → B RED. Engine change is the two TEST_HOOKS-gated accessors
  (zero production behavior change; lint ratchet 33=33). Suite: release
  **515 → 520 PASS / 0 FAIL**, exit 0, banner 0.28.1, `make lint` green
  — measured on the 1 GiB sandbox at `-O0 + ggc-min-expand=5` (floors,
  not ledger material). Docs: ROADMAP M2 item 3 gains a post-item
  hardening addendum with the structural finding and the mutant matrix.

- **build (clang lint leg PROMOTED, 2026-10-11): the last
  `continue-on-error` in CI is gone — `make lint-werror CXX=clang++-18`
  is clean across all four passes and now gates.** The leg had been
  informational since the v29 CI hardening with a recorded debt of
  "4 errors" — a count that had only ever seen the FIRST pass (make
  stops at the first failing command). Fixed verified on clang-18
  18.1.8 (CI runs 18.1.3; project rule: no unverified warning fixes).
  The four header errors were all declarations outliving their
  hooks-gated consumers under `-DCHRONOKV_TEST_HOOKS`-off: 3×
  `-Wunused-lambda-capture` — `put_async`/`get_async`/`erase_async`
  captured the v27-M1 txnrec `rec_t0` unconditionally while the
  in-lambda recording blocks that use it compile out (the old
  `#else const uint64_t rec_t0 = 0;` branches existed solely to keep
  the capture lists mode-independent; g++ has no unused-capture
  warning, which is why only clang ever saw this) — and 1×
  `-Wunused-private-field` — `ReadWriteTransaction::rec_txn_id_`, whose
  both consumers (`set_rec_txn_id`, the `range_scan` recording block)
  are hooks-gated while the `chronokv::Transaction` wrapper's own
  `rec_txn_id_` for the SAME feature was already gated (the RWT copy
  was the oversight). Fix: gate each declaration/capture exactly like
  its consumers — behavior identical in both modes (under hooks-off
  the removed `rec_t0` was a zero-initialized dead local feeding a
  compile-time-absent `if`; under hooks-on every use remains). Fixing
  the header pass unmasked three suite-pass errors: the checkpoint
  corruptor's case-0 one-liner (`-Wmisleading-indentation` — the
  bodyless `for` followed by `break;` on one line; loop body braced,
  semantics unchanged), an unused `t` capture in the v21
  retry-on-conflict test, and a constexpr `OPS` capture in the m16
  concurrent-split test that the constant-expression use never
  required. Verified: clang-18 all four lint passes clean; g++
  `make lint` green (ratchet 33=33, header hygiene incl. the STRESS
  2-TU leg); release full suite 515 PASS / 0 FAIL, exit 0, banner
  0.28.1 — the count is unchanged by construction (the diff adds and
  removes no checks); hooks-off public-API smoke green. Engine
  behavior UNCHANGED (preprocessor gating + capture-list hygiene
  only). ROADMAP next-commits item 2 closed with the same record.
- **test+build (v29 M2 item 4, 2026-10-11): the crash-fuzz plan grammar
  COMPOSES — the fixed-shape Plan struct becomes a seeded window-sequence
  grammar with seven named composition templates covering the roadmap's
  list (GC sweeps x checkpoint x rotation x the merge-window slot) and the
  three Audit-2 surfaces (WAL-2 segment-creation dir-fsync, WAL-1
  offset-scan classifier, EXTRA-1 reservation-throw under batch pressure);
  kAll grows 26 → 28; the fault matrix widens to all 12 kinds — M2
  COMPLETE with this item.** The v25.7 lesson (H1 bricked the database in
  a regime COMBINATION the grammar could not express; the fix bolted on
  one `rotate_after_ckpt` flag) is now the grammar's structure: plans are
  sequences over {Puts, BigPuts (~70 KiB values — batch extents span
  multiple 64 KiB fill chunks, the WAL-1 classifier's multi-chunk
  surface), PressurePuts (concurrent committers — EXTRA-1's batch
  pressure, composed with AllocFail's reservation-window throw),
  Structural (split-forcing insert bursts — THE MERGE-WINDOW SLOT: when
  M4 lands borrow/merge/cascade, this window grows the merge forcing and
  its compositions are already in the plan space), Rotate, Ckpt, GcSweep
  (new `Database::gc_pass_for_test` seam), Backup}, and seven templates
  splice named compositions into the stream: gc x ckpt; rotate-after-ckpt
  (the H1 shape, guaranteed); Structural x Ckpt x Structural;
  PressurePuts x AllocFail; BigPuts x WriteFail/WriteShort; Rotate x
  DirFsyncFail (WAL-2) WITH A FIRED-WITNESS — the child asserts its armed
  charge was consumed before a clean exit (rc 22 = vacuous composition;
  sampled BEFORE `fault::disarm()` zeroes `remaining` — the first cut
  read it after and could never fire, caught by the W2V mutation run
  staying green when it had to go red, recorded in the comment) — and
  the gc x rotate x ckpt three-way. Fault-bearing templates splice at
  position 1 so no earlier window's dir-fsync/write site can eat the
  charge. Round 1 of every stream cycles all seven templates
  deterministically (point-index % 7); the composition ledger is
  published and asserted (a named composition that never occurs = a
  regime the grammar lost). Parent-side classification: the documented
  loud-refusal outcomes of {BigPuts x write-fault} plans (interior zeros
  before surviving CRC-valid frames — the SRC-E4 shape family) are
  counted and published instead of mis-scored as violations, narrowly —
  a corruption signature qualifies, "interior gap" (the EXTRA-1
  regression signal) NEVER does. Two new crash points instrument the
  WAL-2 window itself: `seg_before_dir_fsync` / `seg_after_dir_fsync`
  bracket `open_segment`'s fresh-path dir fsync (reachable through any
  rotation regime; the reachability assert now covers 28 points). The
  Ledger is mutex-serialized with a short-write loop (pressure windows
  commit from several threads; big-record lines are ~70 KiB). Background
  fault sampling widened Kind 1..8 → 1..12 (AllocFail, ManifestOpenFail,
  TruncateFail, RotFinalDirFsyncFail join); the per-kind anti-vacuity
  assert counts all twelve. FAIL-FIRST (scratch builds, full 336-plan
  streams, measured): reverting the empty-orphan tolerance → **16
  violations** concentrated in the seg_after_dir_fsync /
  rot_before_manifest / man_after_tmp_write plans — the new windows bind
  the recovery contract they were written for; removing the DirFsyncFail
  hook from `fsync_dir` → **exactly 7 rc-22 witness violations**. Clean
  stream (1 GiB sandbox, `-O0`): 336 plans / 28 s, 182 reached an armed
  point, compositions gc_x_ckpt=56 rot_after_ckpt=190 structural=132
  pressure_x_alloc=18 bigrec_x_writefault=18 dirfsync_probe=17, all 12
  kinds planned, **0 violations**; loud_refusals=0 at this scale
  (published, not asserted — the deterministic refusal pin lives in
  SRC-E4). CI cost config unchanged (PR: 2 bases x 24 rounds x 28
  points = 1,344 plans). Suite: release 514 → 515 PASS / 0 FAIL (+1:
  the composition-ledger assert), exit 0, banner 0.28.1; `make lint`
  green (ratchet 33=33). With this item v29 M2 is COMPLETE — all five
  items shipped; constraint 3 (catchers before rewrites) is satisfied
  for the arc.
- **test (v29 M2 item 3, 2026-10-11): the audit's four remaining vacuous
  tests de-vacuated — v17 GC-boundedness, v18 LSN-contiguity,
  `m2_phase1` size-rotation, the 30 s soak — each upgraded in place and
  proven by scratch-build reversion (old check GREEN / new check RED on
  every mutant, old blocks extracted from HEAD and run side by side).**
  **v17 GC-boundedness** asserted only that latest values read back
  ("cannot fail on unbounded version or entry growth" — the audit's
  words); it now asserts the reclamation ACCOUNTING via `gc_stats()`
  after two 5,000-commit churn rounds with deterministic explicit gc
  passes (no sleep): live == KEYS exactly (created − reclaimed),
  `retired_pending() == 0`, and round 2 does not ratchet; readback kept.
  Mutant `reclaim_retired() → return 0`: new FAILS ("round 0:
  retired_pending=4990 …; round 1: retired_pending=9990"), old GREEN.
  **v18 LSN-contiguity** asserted only that `recover()` does not throw;
  it now writes unique per-key values and checks replay accounting
  (`recovery_records() == 2000`, `published_watermark() == 2000`) plus a
  full 2,000-key differential readback. Mutant: `recover()` silently
  skips APPLYING every 1000th record (contiguity gate intact, no throw):
  new FAILS ("key b0_259 missing/wrong after recovery"), old GREEN.
  **`m2_phase1` size-rotation** never exceeded SEGMENT_MAX_BYTES (its
  own comment admitted it) and asserted "WAL has >= 1 segment"; a
  hooks-gated knob (`WalSegments::segment_max_bytes_`,
  `Database::set_wal_segment_max_bytes_for_test` — mirroring the public
  `set_rebase_threshold` pattern; production behavior unchanged,
  `force_rotation_for_test` saturates against the same member) lowers
  the cap to 64 KiB, and 448 ~1-KiB puts rotate through the SIZE
  admission ALONE to 8 segments (asserted ≥ 4) with byte-exact
  cross-segment recovery of every key. Mutant: size admission neutered:
  new FAILS (seg_count=1), old GREEN (≥1 holds). **The 30 s soak** was
  "a hang/crash detector with no oracle" (check 1 tolerated any value;
  check 2's unpadded writer keys mostly fell outside its own padded scan
  bounds; check 3 ignored recovered values); writers now own disjoint
  padded key ranges with lock-free shadow models, and the oracle is a
  full differential (every point read AND the whole-range scan) against
  the merged model on the LIVE engine and again through checkpoint+WAL
  RECOVERY, plus every-op-committed and health-level-0 assertions.
  Mutant: `recover_with_checkpoint` skips applying the last 3 WAL
  records: new FAILS ("recovered engine: key sk119 expected 'tail:17'
  got ABSENT"), old GREEN. The soak's fail-first run additionally
  exposed a SECOND vacuity in the upgrade's own recovery leg: the ckpt
  thread's loop shape (`while(!stop){ sleep; checkpoint; }`) always
  takes one final post-stop checkpoint capturing the entire final state,
  so the WAL delta was empty on every run and the first mutation run
  passed — fixed with a post-join tail-write phase (20 commits) that
  guarantees a non-empty delta, the lesson recorded as a permanent
  comment. Engine changes: the one hooks-gated segment-size knob only.
  Suite: release **513 → 514 PASS / 0 FAIL**, exit 0, banner 0.28.1;
  `make lint` green (ratchet 33=33) — measured on the 1 GiB sandbox at
  `-O0 + ggc-min-expand=5`. The audit's other two vacuous-test findings
  (never-compiled `verify_full`; the `conc: no crash` noise flag) remain
  recorded beyond-arc remainder.
- **test (v29 M2 item 2, 2026-10-11): the DST plan grammar grows
  merge/rebalance/cascade and reclamation-retirement scenarios —
  written BEFORE the code they gate, green against the current
  (merge-free) tree, four-mutant fail-first matrix.** New fork-isolated,
  seed-replayable scenarios at registry indices 4–5 (appended after the
  v27 four so historical `(seed, op-count)` replay handles keep their
  meaning): **S5 `tree_merge_cascade`** (M4 grammar) composes a seeded
  delete-window grammar over a prebuilt multi-leaf tree — THIN
  (sibling-borrow shape), RUN (emptied-leaf merge shape), STRIDE
  (separator/interior-key shape), REVIVE (split x merge interleave),
  SWEEP-ALL on 1/4 of seeds (cascade-to-root + collapse) — with a
  concurrent latest scanner (order/uniqueness/legitimacy in flight), an
  as-of prober under a main-held pin (the pinned view must be
  BYTE-STABLE across every window — M4's "concurrent cursor +
  concurrent merge" acceptance, and M3's epoch-pin safety contract at
  the version layer), and quiescence oracles: differential-vs-`std::map`
  (M4's acceptance sentence applied at rest), sampled point reads,
  drain-to-empty + regrow-from-empty. **S6 `reclaim_retire_pin`** (M3
  grammar) runs deep version chains (8 keys x 6 versions) plus 240
  page-churn e-keys (120 pre-deleted into `[tombstone, v1]` retirement
  material) under a pinned reader: capture control, in-flight pinned-view
  stability, a deterministic SAFETY WITNESS (`retired_pending() >= NK`
  after every gc pass ran under the pin — reclamation blocked), post-unpin
  DRAIN (`retired_pending() == 0` within 8 passes — the liveness
  direction of M3's P1, observable today at the version layer) with
  non-vacuity controls (`retired > 0` cumulative; reclaimed-delta `> 0`
  across the unpin), differential, and health. Engine UNCHANGED
  (test-side only; today's deletes are tombstone versions — nothing
  physically merges or retires pages yet — so both scenarios are green
  by construction and become must-pass gates when M3/M4 land).
  FAIL-FIRST (scratch builds, 3 seeds each, all caught 3/3 where
  targeted): reintroducing the v19-M2-fix defect class (`gc_threshold`
  without the reader-slot floor) is caught by BOTH scenarios — pinned
  scans silently lose keys once a tombstone above the pin makes the
  visible version severable; neutering `reclaim_retired` is caught by
  S6's drain oracle (`retired_pending=208` after unpin+8 passes) with S5
  correctly unaffected (oracle scoping); neutering `retire()` is caught
  by S6's witness; and the epoch-gate off-by-one (`<` → `<=`) is caught
  by S6 **by the witness alone** (measured `pending_under_pin=0`,
  stability scans all green) — below-anchor Version suffixes are
  unreachable from any legal snapshot walk today, which is exactly why
  the witness pins the gate contract before M3's page retirement makes
  the same off-by-one a use-after-free (cursors hold raw page pointers
  across latch gaps). Two first-cut harness lessons recorded as
  permanent comments in `tests/tests_dst.cpp`: scenario `model_mu` must
  be brace-scoped (the first S6 cut's unbraced guard reached a second
  acquisition in the same thread — a non-recursive-mutex self-deadlock
  the steal window cannot break; every seed hung to the 20 s fork
  deadline, triaged via gdb on an in-process scratch driver), and
  flag-gated wait loops polled through `dst::point` self-grant the baton
  and hot-spin (first cut: S5 burned 32k–47k scheduling ops for ~6k of
  real work; S6 ground to the deadline) — hence the main-as-pinner
  structure (pin captured quiescent before `dst::arm`, zero gates,
  bounded probers). MEASURED (1 GiB sandbox, `-O0` bare +
  `ggc-min-expand=5`; floors, not ledger material): DST 6 x 100 seeds =
  600/600 children green (109 s) + alternate base 6 x 25 = 150/150; S5
  ~6.0–7.3k ops/child, S6 ~2.9k ops/child (~100 ms); release full suite
  513 PASS / 0 FAIL, exit 0, banner 0.28.1 (non-stress count unchanged
  by design — the battery compiles only under STRESS); `make lint` green
  (four TUs, both hook modes, ratchet 33=33, STRESS 2-TU leg). CI: the
  nightly `dst` per-shard `CKV_DST_SEEDS` rebalanced 3,125 → 2,083 (8 x
  2,083 x 6 = 99,984 ≈ the roadmap's N=100k scenario-runs; per-shard
  child count ~12.5k unchanged, so the wall-time band and the
  shard-disjointness argument both stand); push/PR unchanged at
  100/scenario. Docs in the same commit: ROADMAP M2 item 2 → DONE with
  the numbers and both harness lessons, progress row + next-commits
  updated (remaining catcher work: items 3–4), README's DST paragraphs
  de-staled four → six, ci.yml dst-job comments de-staled.
- **test+fix (SRC review round 2, 2026-10-10): the partial-fill zero
  gap (SRC-E4) — the loud interior-CORRUPT refusal pinned with the
  operator repair executed end to end; the incomplete-fill WARNING now
  carries the repair facts.** SRC-E2/E3 armed the fill-write failure
  against a single-record 44-byte batch, so the failed fill landed ZERO
  bytes; the shape where SOME zeros land before the fill dies
  (multi-chunk extent, Nth pwrite fails) was untested. New test-gated
  one-shot hooks with consumption witnesses:
  `wal_zerofill_fail_pwrite_nth_for_test` (fail the N-th fill pwrite
  attempt), `wal_zerofill_short_write_for_test` (a GENUINE short write —
  half the remaining chunk lands, the loop must continue),
  `wal_zerofill_pwrite_attempts_for_test` + `nth_fired`/`short_fired`,
  and the `wal_zerofill_last_run_for_test` run record {segment path,
  batch_start, extent, bytes zeroed, fill_ok, sync_ok} — the same facts
  the WARNING prints, which makes the repair testable; Database seam
  `rollback_indeterminate_for_test()` reads the raw latch. SRC-E4: a
  4×40 KiB-record batch (163,992 B = three 64 KiB fill chunks); attempt
  1 short-writes 32,768 B, attempt 2 completes chunk 1, attempt 3 dies
  EIO → exactly 65,536 of 163,992 bytes zeroed. Pinned: byte read-back
  (all-zero prefix, surviving frame data right after it);
  `rollback_indeterminate_` STAYS set (raw latch + health reason);
  fail-stop (level 2, next put Failed, size frozen); reopen THROWS
  `Recovery failed: WAL corruption in segment 1` (interior zeros before
  surviving CRC-valid frames — the WAL-1 `valid_after` scan; no silent
  replay is possible because the database does not open); and the
  operator repair — copy aside, `truncate -s <offset>` the segment,
  restart — recovers with acked keys intact, ALL rejected keys absent,
  and writes working again. Only production change: the incomplete-fill
  WARNING now names the segment, the batch offset, the
  zeroed-of-attempted counts, and says the WAL may refuse to open until
  the segment is manually truncated to that offset (README D2 documents
  the procedure). Fail-first: with the OLD warning, E4b FAILED
  (`INCOMPLETE=1 may_refuse=0 segment=0 offset=0 counts=0`) while E4a
  passed — the refusal is pre-existing WAL-1 behavior, the observability
  was the gap; both PASS after the upgrade (battery 90/90). Mutation M6
  (classify a zeroed length field as TORN_TAIL despite CRC-valid frames
  after it) → SRC-E4a FAILS with `refused=0` as the ONLY flipped bit —
  the partial-fill reopen would silently auto-truncate instead of
  refusing; header restored byte-identical (sha256 `c081efad…b24e`).
  Also this round: the provenance note in the audit addendum — the
  `scripts/lint.sh` cited by earlier chat reports never existed in any
  ref, sandbox copy, or patch iteration (lines citing it are void; the
  real gates are the Makefile `lint` aggregate and its four sub-targets),
  and the fee2789-era numbers (FULL SUITE 508 PASS / 0 FAIL; ratchet
  33=33) were re-derived by execution from a fresh worktree of the real
  fee2789 tree on 2026-10-10 — reproduced exactly — and are superseded
  by the 5d037ba re-run regardless (511 PASS / 0 FAIL; four lint gates
  exit 0; CI run #80 success, 23/23 executed jobs).
- **feat (SRC-B DECISION IMPLEMENTED 2026-10-10): zero-fill
  neutralization of the rollback-ftruncate-failure window; persisted
  marker DEFERRED to v30 M3.** The ftruncate-failure branch now zeroes
  the FULL attempted batch extent — every record of a multi-record
  group-commit batch, `[batch_start, batch_start + batch_bytes)` — via a
  chunked `pwrite` loop (short writes and `EINTR` retried; `k==0` treated
  as EIO) followed by `fdatasync`. Recovery sees zeros-at-EOF =
  `TORN_TAIL` (never interior-CORRUPT: no CRC-valid frame survives the
  extent) and the existing tested repair truncates it — rejected frames
  cannot replay on a future open. `rollback_indeterminate_` clears ONLY
  on fill+fdatasync success; `failed_` stays latched regardless (D3 and
  the fail-stop untouched; append-prevention after the extent cited in
  the code comment and asserted in SRC-E1). Fill or sync failure is
  harmless: WARNING logged, flag stays, no throw out of the failure path,
  no new Status. The rollback-FSYNC-failure branch is deliberately
  untouched (power-loss-only window). Window NARROWED, not closed —
  residual exposure (README D2): the fill's fdatasync failing, death
  before the fill completes, and pwrite-broken errno classes (device EIO,
  EROFS, EBADF). Test seams (no production behavior):
  `wal_zerofill_fail_{write,sync}_for_test` one-shot hooks with
  consumption witnesses (anti-vacuity), and `leader_pre_drain_hook_`
  pausing the elected leader in the election→drain window — without it,
  multi-record batches are scheduler-dependent (16/16 attempts split on
  the 2-core box). Tests: SRC-D B2 flipped to the neutralization contract
  (reason CLEARED + rejected ABSENT after clean reopen); SRC-E1
  (4-record batch — 3 small + one 8 KiB page-spanning — byte-exact extent
  assertion, rejected absent / acked intact / reopen size ==
  batch_start / open does not throw / post-failure put refused and file
  size frozen), SRC-E2 (fill-write failure keeps flag+reason, hook
  consumed), SRC-E3 (fill-sync failure keeps flag+reason, hook consumed).
  Fail-first: all four FAIL without the fill block, PASS with it.
  Mutations: M4 (extent shortened by one record) → SRC-E1 FAILS with
  `reopen threw: WAL corruption in segment 1` (a surviving frame after
  zeros classifies interior CORRUPT — loud, as the probe predicted); M5
  (flag cleared unconditionally) → SRC-E2+E3 FAIL, nothing else; header
  restored byte-identical after each.
- **fix (SRC-B option 4, maintainer decision): `rollback-indeterminate`
  health reason.** Both rollback-failure branches (the ftruncate failure
  and the post-truncate fsync failure) set an in-memory latch on
  WalSegments, surfaced through `health()` beside the D3 fail-stop
  reason: the WAL tail may hold a rejected batch whose frames recovery
  cannot distinguish from legitimate data. No on-disk change, no new
  public Status (per the decision). Fail-first: SRC-D (both branches,
  exact `Status::WalFailure` + level 2 + reason presence + fault-charge
  accounting) FAILED pre-fix (`reason_present=0`), PASSES post. SRC-A
  follow-ups from the review: errno captured into a local BEFORE
  `filesystem::exists()` at all three throw sites (exists() could
  clobber the diagnostic), and new SRC-A2 control check pins the
  genuinely-absent-MANIFEST fresh-database path (static `{0,0}`,
  instance `{1,0}` + segment creation, recover_all filename inference)
  against regression by the new throw. Option 3 (bounded retry) SKIPPED
  per the decision's safety condition — analysis in the audit addendum.
  Zero-fill neutralization candidate PROBED (since IMPLEMENTED per the
  2026-10-10 decision — see the top bullet): zero tail at EOF classifies TORN_TAIL and the existing
  repair truncates it (interior zeros stay CORRUPT/loud); with
  TruncateFail injected, an external zero-fill + fdatasync keeps the
  rejected key ABSENT across a clean reopen (poison tail → ordinary
  torn-tail shape); errno-realism table in the addendum (helps: EINTR +
  setattr-broken-data-alive class; does not help: device EIO, EBADF,
  EROFS — fails harmlessly there, flag stays set).

- **fix (SRC audit round, 2026-10): MANIFEST open-failure conflation
  (finding A, Medium) — all three readers now fail loud.** `read_manifest`,
  `read_manifest_dir` and `recover_all`'s read-only open treated EVERY
  ifstream failure (EACCES/EMFILE/EIO — anything setting failbit) as
  "no manifest → new database", contradicting read_manifest's own
  Kimi-review-1.3 comment. Executed repro on the audit's prescribed shape
  (two non-empty size-rotated segments, MANIFEST naming segment 2,
  faithful failbit injection): open SUCCEEDED silently, `active_id`
  reseeded to 1, a write was ACCEPTED into the stale segment behind
  `health level=0` (probe + battery outputs in
  `docs/audits/2026-10-src-audit-findings.md`). Fix: only a
  confirmed-ABSENT path (`std::filesystem::exists` — stats, needs no fd,
  answers under EMFILE) returns the sentinel; an existing-but-unopenable
  MANIFEST throws with errno (D1); genuinely-absent behavior unchanged.
  Fail-first: SRC-A checks FAILED pre-fix, PASS post-fix; post-fault
  reopen asserts the exact pre-test key set. Injection-fidelity note: the
  first-cut injector (bare `close()`) produced a misdiagnosed truncation
  throw instead of the silent path — the fail-first run caught the
  unfaithful injector itself.
- **test (SRC round): three dedicated fault kinds + the SRC battery.**
  `ManifestOpenFail` (all three MANIFEST readers), `TruncateFail` (the
  rollback ftruncate), `RotFinalDirFsyncFail` (the post-unlink dir fsync
  ONLY — DirFsyncFail cannot target it; checkpoint/manifest charges
  consume earlier sites). All CHRONOKV_FAULT_INJECTION-gated; names added
  to `kind_from_name` and the CKV_COVERAGE_FAULT help list. SRC-C pins
  the rotation-final fsync's intended contract end-to-end (checkpoint
  succeeds, health < 2, counter moves, writes keep working, reopen clean)
  — mutation-verified (making the site fail-stop flips the check).
  CKV-004R's first check TIGHTENED from `st != Status::OK` to
  `!rethrew && st == Status::WalFailure` (the catch branch's
  `Status::Failed` is a distinct CKV-016-violating outcome the loose form
  accepted) — mutation-verified (WalFailure→Failed mapping flip fails
  the tightened check; the CKV-004 stage→class gate flip fails the
  sibling resurrection check; tree restored byte-identical after).
- **docs (SRC finding B — D2 contract gap): README narrowed, engine
  UNCHANGED, decision pending.** Executed repro
  (`docs/audits/2026-10-src-b-probe.cpp`): rollback-fsync-failure (B1)
  keeps D2 at clean-restart strength (rejected key absent after clean
  restart; power-loss window as documented); rollback-FTRUNCATE-failure
  (B2) leaves the rejected batch's CRC-valid frame in the WAL and it
  RESURRECTS on a plain clean restart — no power loss involved. Both
  sub-cases fail-stop the instance and count in `truncate_fails`, and
  recovery cannot distinguish the poisoned tail from legitimate data.
  README D2 now states the window; D3 names its single deliberate
  exception (SRC-C's site). Four options with tradeoffs are recorded in
  the audit doc (wording-only [landed] / persisted poison marker /
  bounded truncate retry / distinct health signal) — NO public Status or
  on-disk behavior changed pending the maintainer decision. Modeling
  limit stated: injected faults + clean restarts only; no power-loss
  claim (dm-flakey remains v30 M3).

- **build (v29 M2 item 5): the test-suite TU split — four translation
  units, and the wall comes down.** `main.cpp` (14,921 → 10,587 lines)
  keeps main(), the smoke section, the inline engine batteries and the
  crash-fuzz/review/durability runners; `tests/tests_lincheck.cpp`
  (lincheck namespace + run_lincheck_test, 1,919 lines),
  `tests/tests_remediation.cpp` (a2 namespace + Audit-2 + v28 batteries,
  2,053 lines) and `tests/tests_dst.cpp` (dstscn + run_dst_test, 462
  lines) carry the extracted batteries; `tests/test_decls.hpp` declares
  the three cross-TU entry points. Content is byte-identical in the move
  except the entry points lose `static`; every TU rides the same
  `CHRONOKV_TEST_HOOKS` gate as before, so the hooks-off smoke build
  compiles the batteries to nothing and links clean. Makefile: one
  `mk_test_bin` eval template replaces the nine hand-written rules;
  objects stay FLAT per mode dir so the coverage job's
  `gcov build/coverage/*.gcda` glob keeps working untouched; CI build
  steps gain `-j4` (four TUs, four cores). **Measured acceptance on the
  1 GiB confirmation sandbox** (the box where the monolith died): bare
  `make release RELEASE_FLAGS="-O1"` builds in 68 s (monolith:
  OOM-killed, needed `-O0 --param ggc-min-expand=5`); `make stress` at
  `-O1` builds; ASan+UBSan builds at `-O0` with the ggc param and the
  full remediation battery passes under it — the first sanitizer run
  that box ever had; full suite **503 PASS / 0 FAIL bare at `-O1`**
  (the environmental `-O0` thread-EAGAIN abort did not reproduce at
  `-O1`); lint green incl. the ratchet at 33=33; DST 3×10 green on the
  `-O1` stress build; hooks-off smoke green. **The split found a real
  bug on its first link**: `dst::my_tid` was a namespace-scope
  `thread_local` WITHOUT `inline` — every TU got its own definition, so
  the STRESS-mode 4-TU link failed with multiple-definition (the
  monolith hid it; the API-1 hygiene gate missed it because it never
  compiled `-DCHRONOKV_STRESS`). Fixed to `inline thread_local` (single
  identity is also the semantically correct baton state — per-thread,
  not per-TU) and the class is now permanently gated: `make
  lint-header-2tu` gained a STRESS-defs 2-TU link leg. Engine-side split
  + amalgamated release artifact remain v30 M2; the embed story is
  unchanged (consumers still compile exactly one header). README
  compile-memory limitation and main.cpp's "only this file is compiled"
  header updated in the same commit (rules 2/3/7).

- **bench (v29 M1 step 3, COMPLETE): YCSB D/E/F baseline adapters +
  version pins.** `Engine::scan` joins the adapter interface — and it
  MATERIALIZES rows (key+value bytes touched), because the arena's
  `range_scan` returns `vector<pair<string,string>>` and a count-only
  baseline scan would do strictly less work: a fairness requirement, not
  a detail. Per-engine scans: SQLite prepared `k>=? AND k<=?` with
  reset-before-return (the WAL read-snapshot trap the step-3 validation
  paid for once is now LIVE in ycsb_e's scan+insert alternation on the
  same connection); LMDB `MDB_SET_RANGE`+`MDB_NEXT` cursor walk in a
  read txn; RocksDB iterator `Seek`+`Slice::compare` bound.
  `w_ycsb_def` mirrors `bench/arena.cpp`'s w_ycsb operation-for-
  operation (same per-thread seeding, same pick() order, D's
  geometric(0.001) read-latest over the shared insert counter, E's
  zipfian-start/len-1..100, F's combined R+W rmw rewriting the value it
  read). Validated on sqlite-full/sqlite-normal/lmdb: zero FAILED-OPS,
  op-mix counts IDENTICAL across engines at equal seeds (the fairness
  property), `rows/scans=51` matching the arena's #75 E rows exactly;
  the rocksdb leg is stub-syntax-checked locally (no bookworm package)
  and gets its first full compile in CI. Default workload set is now the
  full ten. **Pins:** `bench/third_party/versions.txt` — sqlite 3.45.1,
  lmdb 0.9.31, rocksdb 8.9.1 (the versions the CI ledger builds
  against, cross-checked against run #75's methodology headers) with
  each release artifact's sha256 (fetched over TLS from canonical
  origins) and the GitHub-archive re-record rule; `fetch.sh` verifies
  every pin (mismatch fatal, never blind-accepted; curl/wget/python3
  fallbacks); `src/` gitignored. CI keeps installing distro packages
  during calibration — headers record per-run versions; build caching
  lands with gate enablement.
- **ci (v29 M1 step 4, FAIRNESS REVERT): the arena ledger vehicle is
  ONE job again.** The e9eb787 split fixed the budget and ran green
  (#68/#74/#75) — but parallel jobs mean parallel RUNNERS, and #75's
  methodology headers caught the cost: baselines on "AMD EPYC 7763",
  ChronoKV on "Intel Xeon 6973P-C". The fairness protocol this ledger
  exists for says "same harness, same hardware"; a cross-engine
  comparison across two CPUs is not the product. Re-merged to a single
  job (same machine by construction) with a MEASURED 120-minute cap
  (#65's sequential need ~60 min killed-at-budget; split legs summed
  ~48 min; >=2x headroom — rule 13), artifact name reverted to
  `arena-ledger-<run_id>` (the split-era part names never gained a
  consumer). **The green-week clock RESTARTS at the first completed
  night on the merged vehicle**: nights 1–3 ran cross-machine — their
  per-engine rows stay valid for same-engine trend (each row carries
  its own hardware header), their cross-engine comparisons are not
  ledger material. Rule 9 over sunk nights; the headers catching this
  is rule 9 working exactly as designed.
- **docs (roadmap): the soak vehicle is NAMED** (M1 spec item 4): the
  rolling nightly chain with persisted state + aggregate verdict —
  hosted 6-h caps rule out literal 24-h/7-day jobs, no self-hosted
  machine exists or is depended on, and the ledger artifact machinery
  plus the coming noise bands ARE the state + verdict mechanisms.
  24-h soak = chained nightly segments; P1's 7-day soak = a 7-night
  chain; segments rebuild from pinned geometry so runner variance lands
  in the bands. Self-hosted remains an upgrade path before v30 M1;
  acceptances are vehicle-independent. M1's remaining list also records
  the ledger's first honest comparison rows (#75): ChronoKV
  group-durability fills/updates at 2.3k–5.4k ops/s vs power-loss-class
  baselines at 3.9k–9.9k, reads competitive (ycsb_c 943k vs rocksdb
  915k; lmdb's mmap owns readrandom at 4.0M) — the writer-thread
  rewrite's before-column, exactly as claim 3 predicted.

- **test (v29 M2 item 1, CI-hardening follow-up): the write-skew
  mutation cannot starve.** Run #72 (all legs) failed exactly one check —
  the Section 3b fabricated-anti-dependency-pair mutation reported "no
  suitable A/B pair": its first predicate required a COMMITTED
  scanning-writer plus a write-only UINT64_MAX-snapshot writer inside
  the scan range, and on CI's faster runners (real io_uring, -O2, 4
  cores) SSI refused every scanning writer on the recorded seed — the
  engine working correctly starved the detector (the checker itself was
  clean: every engine-workload aggregate passed on every leg, zero
  false positives across runners). Fixed per the anti-vacuity culture:
  (1) exhaustive pair search — any committed writer B whose key falls in
  ANY scan of ANY committed scanning-writer A, B's read/snapshot shape
  unrestricted; (2) a fallback that cannot starve while the workload's
  nonvacuity gate holds — two REAL committed write txns, A gaining a
  fabricated point scan over B's key at snap cts_B−1 (synthetic scan,
  engine-real txns/keys/cts, identical cycle algebra) — with the
  fallback use printed so it can never pass silently unused-shaped.
  The starvation detail line now names the real invariant (fewer than
  two committed writers ⇒ the nonvacuity gate should have failed
  first). Verified: 5 seeds green locally (primary path), lint +
  ratchet clean.

- **test (v29 M2 item 1): `lincheck::check_write_skew` — the SSI
  anti-dependency checker, closing the blindness Audit-2's TXN-1
  demonstrated.** lincheck validated view consistency (reads match their
  snapshot) and real-time order; the TXN-1 history satisfies BOTH and is
  still not serializable — the missing layer was the multiversion
  dependency graph. The checker builds it (ww version chains; wr
  observed-version edges; rw anti-dependency edges from point reads AND
  scans: any committed write to a read key / in-range scan key above the
  reader's snapshot forces reader-before-writer) and flags cycles as
  `anti-dependency-cycle`. Scope discipline: writes inside
  (observed_version, snapshot] stay check_cts_order's snapshot-soundness
  class — one violation class per checker. Aborted txns contribute
  nothing; write-only txns cannot close cycles alone (mirroring the
  engine's entry-lock FUW + phantom-validation split). Ships with: the
  Section 1d synthetic battery — classic x/y write-skew, the TXN-1 PoC
  shape pinned VERBATIM (W2), lost update, phantom crossed-scan-inserts,
  plus serial/disjoint/post-fix-abort controls; wiring into both engine
  workload aggregates (every seed's committed history must be acyclic);
  and an engine-shaped Section 3b mutation (fabricated missed-
  anti-dependency pair on real recorded txns — in a clean SSI history
  every committed read sits at-or-below its snapshot, so a genuine cycle
  takes BOTH missed halves, reconstructed). **Live roadmap acceptance**
  (same driver, extracted checker, both engines): against tag `v0.28.0`
  the anomaly commits and the checker flags the exact cycle
  (`T2 -[rw 'x': read@2 missed write@4]-> T1 -[rw scan[k,k]@snap 2
  missed 'k'@3]-> T2`); against the post-fix engine T1 is refused
  (Conflict) and the committed history is CLEAN. Suite grows 495 → 503
  checks; verified green (full suite completes at 503 PASS + the known
  ptrace-only async-p50 artifact under gdb; the bare-run environment
  abort is unchanged and already classified in the delta-confirmation
  report).

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
