# GC persistent cursor + coverage escape-point hardening — work orders (findings #8, 2026-11-14)

Status: WORK ORDERS (engine milestones, sliced like M3/M4/M5 were). The
specs are the deliverable; nothing here races a landing decision that the
roadmap gates on its own evidence.

## Part A — GC persistent cursor (replace the per-pass full-tree rescan)

**The measured problem.** `ChronoKV::gc_once()` (chronokv.hpp, the
`gc_once` body below `start_gc`) opens EVERY pass with
`tree_scan_all()` — a full B+ tree range scan collecting every
`(key, KeyEntry*)` pair into `all_entries` — then processes at most
`GC_KEYS_PER_PASS` (256) of them and stores `gc_cursor_` as an INDEX into
that pass's SNAPSHOT. The next pass re-collects the whole tree. That is
O(N) collection per 256-key budget: a full sweep is O(N²/256) scan work,
exactly the README's "GC sweep cost" limitation. The v25.7 H2 fix stopped
the idle SPIN (1100 passes/s burning one core); the per-pass rescan
remains (the tsan.supp-line-547 note: "a persistent-cursor sweep is the
follow-up").

**Goal.** Replace index-into-snapshot with a persistent B+ tree cursor
(the same `Cursor` machinery `RangeScanStream` rides, minus its
SnapshotGuard): hold it across passes under its own bookkeeping, resume
where the last pass stopped, wrap to the start when it exhausts. New keys
inserted AHEAD of the cursor position are missed until the wrap — the
same semantics today's `gc_cursor_ >= total` wrap has (the existing
"new keys added during this scan are not visited this pass" note), so the
change is cost-shape, not semantics.

**What must stay green (the acceptance net, all named):**
- DST S3 (`gc_epoch_scan`) and S6 (`reclaim_retire_pin`) — the GC class
  scenarios; S6's pinned-witness (`retired_pending() >= NK`) semantics
  cannot survive a sweep that retires the wrong things.
- The v17 boundedness accounting (live == created − reclaimed exact at
  the version layer).
- The v25.7 H2 idle detector (`run_gc_idle_test`) — the cursor must not
  reintroduce the busy-spin (the wake/`dirty_` discipline stays).
- Pinned readers still block reclamation (E-series; the epoch gate is
  upstream of the sweep and untouched).
- The M3 slice-3 purge accounting (`entries_retired/reclaimed`) — the
  purge loop hangs off the same walk.

**Fail-first (required, in the house style):**
1. A sweep-cost measurement harness proving LINEAR, not quadratic, work:
   pass-cost (ns) and keys-scanned per pass vs N at N = 10k / 100k / 1M
   under identical churn; the pre-fix tree must show the quadratic curve
   (the H2 regression's own measurement idiom — its 1100-passes/s
   number is the precedent for quoting engine costs).
2. A H2-class regression control: an idle DB with > 256 keys must reach
   the gc_cv_ wait between sweeps (pass-rate ~ the dirty_ wake rate, not
   a spin).
3. The S6/S3 reversion proofs re-run on the cursor tree (the recorded
   mutants: epoch-gate off-by-one, reclaim neutered, retire neutered).

**Acceptance ties (from the finding):** the arena `deletechurn_gc`
series' retired/reclaimed deltas hold on the cursor tree (the series
landed 2026-11-14 as the calibration vehicle), the RSS ratchet on
deletechurn holds, and the 100M-key soak prerequisite's per-pass cost is
flat in N. Roadmap M3 (page reclamation) and the README "GC sweep cost"
limitation both close on this.

## Part B — Coverage escape-point hardening (all 8 kinds to full depth)

**The measured problem.** The nightly coverage vehicle
(`.github/workflows/ci.yml`, job `coverage`) runs each of the 8 forced
fault kinds in TWO stages — `CKV_ONLY_REVIEW=1` first (proven to survive
all 8), then the best-effort FULL suite — because (its comment, citing
run #21) six of eight kinds died in ~1.5 s at full-suite depth: global
fault forcing makes an async `fut.get()` rethrow or a `stoi` cascade
terminate the process long before the suite's later batteries run. The
libgcov merge keeps the union, but those 6 kinds never exercise
full-suite depth — the untested-error-lines ledger keeps those rows
empty, and the two-stage vehicle exists only to paper over it.

**Goal.** Harden the escape points so every kind survives to full-suite
depth: the suite's own arm/disarm discipline (faults fire inside a
scoped window and are DISARMED before the child leaves it) is the
existing pattern — the forced-coverage forcing must adopt the same
containment, and the two raw escape classes must be fixed:
1. `fut.get()` rethrows from `put_async/erase_async` futures whose
   engine call failed under forcing: the futures' bodies already
   resolve-with-`Status::Failed` on engine exceptions (CKV-016); the
   coverage escape is the paths where a forced fault THROWS past the
   containment — enumerate and wrap them (the CKV-016 fix sites are the
   map).
2. `stoi` cascades: test batteries parse stored counter values with
   `std::stoi` (main.cpp's value-roundtrip tests); under forcing the
   stored value is a fresh `make_value` string (non-numeric) or a
   truncated frame, and `stoi` throws `invalid_argument` through the
   battery. Guard with the existing try/catch-and-count idiom.

**Acceptance (from the finding):** all 8 kinds produce full-suite gcov
data (no vacuous kinds — the fired-count print guards that), the
untested-error-lines ledger (INVARIANTS §5 G-7's durability-path subset)
shrinks in the coverage-analyze output, and the nightly two-stage
vehicle collapses to single-stage — the `CKV_ONLY_REVIEW` stage's only
reason to exist was the early deaths.
