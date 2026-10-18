# M4 leaf merge & rebalance: the protocol decision

**Date:** 2026-10-18
**Status:** DRAFT — DECIDED pending the maintainer's answers to §12 (open
questions). This is the "made **before** implementation" decision the v29
M4 milestone requires, in the shape of the M3 precedent
(`2026-10-12-m3-hp4-aba-decision.md`). No M4 engine code exists at the
code-basis commit.
**Scope:** the leaf/interior merge, borrow, cascade and root-collapse
protocol; `PagePool::free()`'s first engine callers; the invariant-P1
checker battery; the G1 amendment free-then-alloc bumping requires. NOT in
scope: BT-3 (separate document,
`2026-10-18-bt3-split-visibility-window.md` — M4 DEPENDS on its §7.2 root
revalidation for the collapse); the GC-sweep linearization (M6); pool
sizing/health thresholds (shipped in M3 slice 3); any on-disk format
(there is none — §6).
**Code basis:** every anchor below was read at 0.28.1, commit `1ab7c8c`
(v29 M3 slice 3/3: the PURGE, release 562/0, DST 150/150). Citations use
function-scope anchors — line numbers rot in this repo; names do not.

---

## 1. The decisions

1. **Reuse safety stays the generation tag; `free()` gains a bump.**
   `PagePool::free(id)` bumps `slot_gen_[id-1]` (release) under the pool
   mutex, in addition to the existing `alloc()` bump (G1 unchanged at
   alloc). **Contract: `free()` is called only while the caller holds the
   page's exclusive latch** — the merge protocol does by construction
   (§3.4). Rationale: a freed-but-not-yet-reallocated slot ("limbo") keeps
   its old generation and its old contents (`free()` does not memset;
   `alloc()` does), so with an alloc-only bump a cursor entering a limbo
   successor would pass its G2 check and emit from a dead page, and a
   writer's `fence_unchanged` would pass on a limbo leaf and mutate a page
   about to be memset — an acked-write loss. A free-time bump makes limbo
   detectable at every existing verification site with NO new sites:
   `Cursor::enter_leaf` restarts, `fence_unchanged` re-descends. The
   wrap argument survives verbatim: aliasing now needs 2^31 free+alloc
   cycles inside one latch acquisition instead of 2^32 allocs.
2. **Merges never hold a latch upward.** A rebalance is a fresh TOP-DOWN
   exclusive crabbing descent (root → … → parent → {sibling, leaf}), run
   under `write_mu_` AFTER `erase_recursive` released the leaf. Acquiring
   an ancestor while holding a descendant deadlocks against crabbing
   readers (reader holds parent shared, waits child shared; merger holds
   child exclusive, waits parent exclusive; no timeouts in
   `std::shared_mutex`). This is the same reason `put_recursive` releases
   the leaf before the parent splice — the delete path inherits the
   existing protocol's spine instead of inventing a second one.
3. **Ancestors hard-lock; siblings try-lock; abandon is a normal event.**
   The top-down path (root..parent) takes exclusive latches with blocking
   acquisition — its holders are transient (parked cursors sit on LEAVES
   only; interior shared holders are passing descents). The leaf, its
   sibling, and any interior sibling enter via `try_lock`; on failure the
   WHOLE attempt is abandoned (latches released top-down, counter
   `rebalance_abandons_lock++`). Rationale: a hard lock on the leaf lets a
   parked `RangeScanStream` freeze the tree WIDE (the merger would hold
   every ancestor exclusive while waiting) — a stall class that does not
   exist today (today's erase blocks writers only, never readers-at-root).
   Merges are best-effort space reclamation: correctness NEVER depends on
   a merge happening, so abandonment costs nothing but a retry on the next
   erase. P1's steady state relies on progress-over-time, not
   progress-per-attempt (§7).
4. **Survivor is the LEFT sibling; a leftmost LEAF never merges right.**
   Leaf merge = N's entries copied into its left sibling S, N unlinked and
   freed. A leftmost underflowed leaf may BORROW from its right sibling
   but is never merged into it. Rationale: an in-flight (parked) cursor
   inside N drains before the merge can take N's exclusive; with a LEFT
   survivor the copies land BEHIND the cursor (it already emitted S), so
   its continuation to `N.next` can neither duplicate nor drop (§4 MW2).
   With a RIGHT survivor the copies land AHEAD of the cursor inside the
   page it enters next — duplicates — unless the cursor gains a monotonic
   emission filter, i.e. a semantic change to every scan in the engine.
   One filter-free rule beats one engine-wide semantic change. Cost: at
   most ONE underflowed page (the leftmost leaf) can persist — bounded,
   recorded, and absorbed by root collapse at the endgame. INTERIOR
   leftmost pages are exempt from the prohibition (survivor may be the
   right sibling): no cursor parks on an interior page, and descents are
   latch-coupled through the parent (§4 MW4).
5. **Byte-aware, plan-before-mutate, allocation-free merges.** The merge
   planner materializes the combined entry list, prices it with the SAME
   capacity model as the split planner (per-entry
   `sizeof(slot) + key + value` against `PAGE_ENTRY_BUDGET` — the
   roadmap's "split planner as the single source of capacity truth"), and
   mutates only on an exact single-page fit. `!fits` → fall back to a
   borrow; borrow `!fits` (including the parent separator replacement,
   which can LENGTHEN) → abandon. No merge ever allocates a page, so
   BT-1's `5*(height+1)+1` split headroom is untouched by construction;
   the re-derivation the roadmap asks for is the observation that merges
   only ever RAISE `pages_available()`, plus a control that runs the A2
   exhaustion battery with merges active (§8 slice 2).
6. **Unlink-before-free, both links, under held exclusives.** Order within
   a leaf merge (all latches already held from the crabbing descent):
   (a) copy entries into S, refence/bump S; (b) remove N's separator slot
   from parent P; (c) `S.next := N.next` (chain unlink); (d)
   `pool_.free(N)` (generation bump, decision 1). (b) and (c) before (d)
   is the load-bearing contract `Cursor::advance_to_valid` already
   documents: *"while the successor is linked from a latched page it
   cannot be freed (freeing requires unlinking under this page's
   exclusive latch)"* — M4 makes that sentence true for the first time.
   Interior merges: pull the separator down into the survivor, reattach
   the freed page's children (children are NEVER freed by an interior
   merge — only the interior page itself), parent slot removal before
   `free()`.
7. **Root collapse reuses BT-3's root revalidation; the old root is freed
   like any page.** When the root interior reaches 0 slots (one child C):
   `root_id_.store(C, release)` under the held path exclusives, then
   `free(old_root)` under its exclusive (bump per decision 1). Descents
   that loaded the stale root id latch the slot and revalidate
   `latched == root_id_.load(acquire)` BEFORE dereferencing any byte —
   mismatch retries without ever reading the freed slot, so the
   alloc-memset of a reused old-root slot cannot race a byte read, and a
   benign ABA (the slot re-allocated AS a new root) passes the check with
   valid current content. No epoch deferral, no quarantine list: root
   slots recycle through the ordinary free list. **Hard dependency:
   BT-3 §7.2 must land first (or in the same slice) — without
   revalidation, collapse turns the today-benign stale-root descent into a
   use-after-free.**
8. **Underflow trigger is byte-based; root exempt.**
   `used_bytes(page) < PAGE_SIZE / 4` (tunable constant, additive-form
   arithmetic per EXTRA-2's no-unsigned-underflow lesson) marks a page
   underflowed; the check runs on the erase path's leaf after a successful
   removal, and on interior pages as the cascade unwinds. The root (leaf
   or interior) is never underflow-triggered; a root leaf may legally sit
   empty. Rebalance is attempted at most once per successful `erase`
   (bounded work per delete; the cascade itself is bounded by height).
9. **No cursor semantic change.** No monotonic emission filter, no
   restart-behavior change: decision 4's left-survivor rule plus decision
   1's free-time bump make every interleaving class provably emission-exact
   with the cursor EXACTLY as shipped in M3 slice 2 (its G2 restart path
   becomes live production machinery for the first time — slice 2's
   controls are its first real consumers, which is the point of having
   shipped it fail-first).
10. **No on-disk or format change anywhere; no new durability surface.**
    Pages are never persisted (`checkpoint_locked` serializes logical
    entries via `tree_scan*`; recovery rebuilds the pool fresh — the M3
    decision doc §6 argument carries verbatim). Merges are invisible to
    WAL, checkpoint, backup, PITR. The crash-fuzz grammar's structural
    merge-window slot needs NO new crash points: it interleaves
    checkpoint/rotation with tree churn, and its recovery differential
    binds merge×checkpoint through the cursor-completeness argument
    (§4 MW6) instead.
11. **The latch table's `erase()` stays deliberately uncalled** (M3 slice
    3 checklist item 7's recorded rationale is re-confirmed, not
    re-litigated): entries are keyed by slot id, bounded by pool capacity,
    recycled by slot reuse; erasing on free would be WRONG under the
    immediate-return free list (a concurrent `get_or_create` pointer could
    outlive the erase). M4 adds a comment citing this document at the
    free() call sites.
12. **Diagnostics and seams ship with the machinery, not after it.**
    Tree face: `merge_stats()` — `leaf_merges, interior_merges, borrows,
    root_collapses, abandons_lock, abandons_fit, pages_freed` — beside the
    existing `cursor_restarts()` / `fence_recheck_retries()` the
    delete-churn table already reads. Stress points (auto-bound into S5 by
    the seeded scheduler): `tree_rebalance_begin` (path descent start),
    `tree_merge_copied` (after (a), before (b)), `tree_merge_unlink_gap`
    (after (b)+(c), before (d) — THE limbo window),
    `tree_root_collapse` (before the store), `tree_leaf_trim` (BT-3's
    Phase 3, lands with that remediation).

**The principle in one sentence:** *a page dies the way a key dies —
unlinked before retired, identity bumped at the unlink, and every trust
point that already exists (G2 cursor restart, fence recheck, root
revalidation) rejects the corpse without learning a new trick.*

---

## 2. Why a decision was required (the problem, precisely)

At `1ab7c8c`: dead KEYS are physically purged (M3 slice 3) but dead PAGES
are not — `PagePool::free()` has no engine callers, emptied leaves stay
linked, splits only ever add pages, and under a monotonic new-key frontier
the pool is a measured creeping ceiling (M3's recorded scope note:
11→50 pages/25 rounds of frontier churn; the deletechurn RSS column sits
flat at ~201 MB — P1's before-column, ratchet-watched by the arena bands).
On exhaustion `alloc()` throws `std::bad_alloc`; `health()` already
degrades at ≥80% and fails at ≥95%. M4 gives `free()` its callers and
closes the monotonic shape: borrow/merge on the delete path, interior
cascade, root collapse.

The M3 decision doc deliberately left TWO questions open that this
document answers: (i) what makes a freed slot's LIMBO phase (freed, not
yet reallocated — old contents, old generation) safe for the two G2
verification sites, whose checks were designed against the REUSE phase
(decision 1); (ii) what the merge protocol's latch shape is, given the
tree's optimistic-descent writer protocol and parked-cursor readers
(decisions 2–4). The DST grammars S5 (`tree_merge_cascade`) and the
crash-fuzz structural slot were written BEFORE this code, fail-first, and
bind it on landing (v29 M2 item 2's acceptance already recorded).

## 3. The protocol

### 3.1 Trigger and placement

`erase_recursive` removes the slot as today (fence-verified, under the
leaf exclusive, epoch bump). On success, still under `write_mu_`, it calls
`maybe_rebalance(leaf_id)` — a SEPARATE descent (decision 2): reload
`root_id_`, crab top-down taking exclusives (root..parent hard, then
try_lock sibling + leaf; any try_lock miss → abandon everything,
`abandons_lock++`). Re-verify under the held leaf exclusive: same slot
generation as the erase left (it must be — `write_mu_` serializes
structural writers; the check is the belt to that suspenders), still
underflowed (decision 8's byte test — a concurrent reader cannot refill
it, but the check costs one pass and pins the trigger honestly).

### 3.2 Leaf level

*Sibling S = left neighbor if it exists (decision 4).*
**Merge** (planner says S ∪ N fits one page, byte-aware, decision 5):
execute §1.6's (a)–(d). **Borrow** (merge !fits, or S itself underflow-
adjacent): rotate one entry — from S: `S.max → N.front`, parent separator
for N becomes the new `N.min` (the OLD separator, `S.max`'s successor key
convention: separators are first-keys-of-right-subtrees, so the parent
slot `(N.min, S)` gets its key REPLACED); from the right sibling (only
when N is leftmost): `R.min → N.back`, parent slot `(R.min, N)` key
replaced by the new `R.min`. Separator replacement is a page rewrite under
P's exclusive with an additive fits-check first (the new key can be
LONGER than the old — a 4048-byte key replacing a 4-byte one is legal);
!fits → abandon (`abandons_fit++`).

### 3.3 Interior level and cascade

After a leaf merge removes a slot from P: if P is underflowed and not the
root, borrow/merge P with ITS siblings under the same rules (the path
exclusives are already held — no re-descent; an interior sibling enters by
try_lock like a leaf sibling). Interior merge (right R into left L,
separator G in P between them): G becomes L's last slot key; L absorbs
R's slots and R's rightmost child; P's slot `(G, L)` is removed;
`free(R)`. L's surviving fence/child set is exact by construction; no
child page is ever freed here (children move, never die). **Cascade:**
each level's slot removal re-triggers that level's underflow check, up to
the root. **Collapse:** root with 0 slots → decision 7.

The cascade is bounded by height per erase; height only ever SHRINKS here
(collapse), so BT-1's headroom bound (recomputed per put from the
leftmost-spine walk under `write_mu_`, where structure is stable) needs no
change — the roadmap's "re-derived together" is §1.5's observation plus
slice 2's exhaustion-with-merges control.

### 3.4 Latch inventory for one merge attempt

Held simultaneously, top-down order: root..P exclusives (hard), S
exclusive (try), N exclusive (try), [interior sibling exclusives (try) as
the cascade unwinds — each acquired while its parent's exclusive is
already held]. Released strictly reverse-order on abandon or completion.
`free(N)` runs with N's exclusive held (decision 1's contract). No latch
is ever acquired upward; no exclusive is held across a `pool_.alloc()`
(merges allocate nothing).

## 4. Window analysis (the classes, and what closes each)

* **MW1 — cursor leaf-switch vs successor free (limbo).** Cursor on P
  captures `(N, gen_N)` under P's latch; the contract P's latch enforces
  (N linked ⇒ N unfreeable without P's exclusive) serializes capture vs
  unlink. Capture-before-unlink ⇒ free bumps gen (decision 1) ⇒
  `enter_leaf`'s G2 check mismatches ⇒ restart from last-emitted key
  (exclusive) ⇒ no emission from the corpse. Capture-after-unlink ⇒ P
  yields the new successor; N was never captured. Mutant that reddens the
  control: NO-FREE-BUMP (limbo entry emits stale, then the alloc-memset
  race follows under ASan).
* **MW2 — parked cursor inside the merge target.** Merge needs N's
  exclusive; the parked cursor holds N's shared; merge WAITS (hard lock
  only on the ancestor path — the leaf itself is try_lock, so a parked
  cursor ABANDONS the attempt instead of freezing the tree, decision 3).
  If the cursor drains first: left-survivor copies land behind it (its
  continuation via stale `N.next` skips S correctly — S was already
  emitted). Emission-exact per decision 4's argument; S5's in-flight
  order/uniqueness/legitimacy oracles bind it, plus a dedicated
  parked-cursor control at `tree_merge_copied`.
* **MW3 — writer fence recheck vs freed leaf.** In-engine unreachable
  (all structural writers serialized by `write_mu_`/`nm_`); the G2 gen
  check in `fence_unchanged` (shipped M3 slice 2, gen FIRST) covers it for
  standalone-tree consumers regardless. No new machinery; the existing
  MUT-FENCE/MUT-CURSOR isolation carries.
* **MW4 — descent vs interior merge/free.** Latch coupling: a reader
  routes to child C through parent P only while holding P shared; the
  merger's P exclusive waits. Reader takes C's shared BEFORE releasing P;
  the merger's C exclusive then waits for the reader to crab onward. C is
  freed only after both unlinks under held exclusives ⇒ no reader is ever
  inside a freed page, and no fresh descent routes to an unlinked one.
  Children of a merged interior page are reattached, never freed — the
  reader below C is untouched by C's parent-level surgery.
* **MW5 — root collapse vs stale-root descent.** Closed by BT-3 §7.2's
  revalidation (decision 7): latch-then-check-then-read ordering means a
  stale root id never reaches a byte read; the freed old-root slot's
  reuse (alloc memset under pool mutex, no page latch) cannot race a
  reader because the only readers of that slot are (i) revalidation-failed
  descents (no byte read) and (ii) the new occupant's own latch-holding
  builders. Control: collapse + forced slot reuse + stale-id seam ⇒
  correct reads; mutant NO-REVALIDATE ⇒ ASan or wrong-subtree control RED.
* **MW6 — merge × checkpoint/scan completeness.** `checkpoint_locked`'s
  `tree_scan_all` is a Cursor: MW1+MW2 apply verbatim. A missed key would
  be a checkpoint data loss, so this window gets its own control:
  hooks-gated merge parked at `tree_merge_unlink_gap` WHILE a checkpoint
  scan crosses the region, then recovery + differential vs the model.
  (Cursors are miss-free: the merge either waits for them or forces a
  restart that re-descends from the last emitted key — restart can skip
  nothing because keys are unique and ordered, the M3 slice-2 argument.)
* **MW7 — purge × merge.** `purge_dead_key` unlinks tree slots under
  `nm_` exclusive via `tree_->erase` — the SAME serialized path that
  triggers rebalance; no new interleaving (purge's erase may itself
  trigger a merge; that is desired: purge empties leaves, merges reclaim
  them — the two M3/M4 halves of "the index tracks the live key set").
* **MW8 — abandonment liveness (P1's progress leg).** Not a safety
  window: every abandoned attempt is retried by the next erase, and the
  delete-heavy workloads P1 names are erase-dense by definition. The
  churn driver's high-water oracle (§8 slice 4) is the measured check;
  `abandons_lock` in diagnostics makes starvation visible instead of
  theoretical. If a soak ever shows high-water creep WITH a growing
  abandon count, the fix is a quiescent rebalance sweep (a tracked
  follow-up), not a latch-protocol change.

## 5. Invariants

* **P1 (new, roadmap text unchanged):** under a sustained delete-heavy
  workload the page pool reaches steady state with bounded utilization,
  with no use-after-free and no ABA. Checker column (§8 slice 4): churn
  driver high-water flat/shrinking (the M3 driver's 6200-ratchet→flat-200
  shape extended to the monotonic frontier that today ratchets 11→50),
  ASan+TSan clean under S5/S6 seeds, `merge_stats` nonzero (anti-vacuity:
  the battery must MEASURE merges happening), deletechurn RSS movement in
  the arena (band update by dedicated commit per the standing policy —
  never in-flight).
* **G1 (amended):** every slot LIFETIME transition — `alloc()` AND
  `free()` — bumps the slot generation exactly once under the pool mutex,
  release-ordered; verification sites unchanged (G2). INVARIANTS.md row,
  the M3 decision doc's §3 wrap argument (re-derived for 2 bumps/cycle),
  and slice-1 controls updated in the same commit as the code (rule 3).
* **G2 (unchanged, now load-bearing in production):** the cursor restart
  and fence recheck were shipped fail-first with no production producer of
  freed slots; M4 slice 2 is their first. Their existing controls stay;
  the new controls compose with them (MUT isolation must stay exact —
  a merge mutant may not redden a G2 control).

## 6. What does NOT change

On-disk formats (none exist for pages); WAL/checkpoint/backup/PITR paths;
`write_mu_`/`nm_` lock topology; the split planner (reused as the capacity
oracle); cursor semantics (decision 9); `LatchTable::erase()`'s
deliberate non-use (decision 11); BT-1's bound (§3.3); the arena geometry
(rule 9 — deletechurn band MOVEMENT comes from the engine, and its band
update is a dedicated after-the-fact commit).

## 7. Acceptance mapping (ROADMAP §M4, verbatim legs)

| Leg | How it is met |
| --- | --- |
| P1 under a 7-day soak, ASan/TSan interleaved, on M1's soak vehicle | **OPEN QUESTION §12.2** — vehicle choice (self-hosted / rolling nightly chain / offline rig). The battery below runs regardless; the soak is its long-duration wrapper. |
| concurrent cursor + concurrent merge clean under TSan AND M2's DST | S4+S5 under `make tsan`; S5 seeds at PR/nightly scale (grammar already written fail-first — slice 3 is where it BINDS) |
| differential-vs-`std::map` passes with merges active | S5's quiescence differential + the engine-level churn driver's exact-count oracles (M3's 9600/9600 + created-reclaimed pattern) |
| arena delete-churn and space-amplification tables move | deletechurn RSS (ratchet-watch band) + `pool_stats` high-water in the churn table; band update per policy |

## 8. Slice plan (each slice: green suite + controls + mutants + ASan, house norm)

* **Slice 0 — BT-3 remediation (LANDED 2026-10-18, same day as this
  decision doc; BEFORE any merge code, per rule 11).** Reproduced
  fail-first (verbatim RED: controls A–D, `missing_in_gap=32 first=k00368`
  / `92 first=k00092` / `found_in_stale_descent=0` / engine-level
  `reader_val=<absent>`), fixed by the copy-out split + deferred trim +
  root revalidation package, controls bt3 A–E green, mutants MUT-REBUILD
  {A,B,D} / MUT-NOREVALIDATE {C} / MUT-NOTRIM {E} with exact co-flip
  isolation, full stress + release suites ALL TESTS PASSED. Versioning
  follows the arc's Unreleased pattern (the 0.28.2 proposal was
  superseded — recorded in the finding doc §9). Repro log: finding doc
  §11. **§7.2's root revalidation is LIVE — slice 3's collapse
  dependency is discharged.**
* **Slice 1 — free-time generation bump (pool-side only).** `free()` bump
  + contract comment + G1 amendment (INVARIANTS.md + decision-doc
  cross-reference) + unit controls driven through test hooks (free a
  slot under its exclusive; witness the bump; limbo-entry restart via the
  cursor seam). Production behavior UNCHANGED (still no engine callers) —
  the M3 slice-1 pattern. Mutant: NO-FREE-BUMP reddens the limbo controls
  alone.
* **Slice 2 — leaf borrow + leaf merge.** `maybe_rebalance` (leaf level
  only), the planner, unlink-before-free ordering, `merge_stats`, the
  five stress points, S5 goes BINDING for leaf shapes. Controls:
  deterministic merge-shape driver (differential vs `std::map`,
  `pages_freed` exact-count, high-water shrink oracle); parked-cursor
  emission-exactness at `tree_merge_copied`; limbo-restart at
  `tree_merge_unlink_gap`; checkpoint-over-merge recovery differential
  (MW6); A2 exhaustion battery re-run with merges active (§1.5). Mutants:
  FREE-BEFORE-UNLINK (ASan/differential RED), NO-PARENT-UNLINK
  (descent-corruption differential RED), NO-TRYLOCK (parked-cursor
  control DEADLINES red — the C1 parent-deadline pattern),
  RIGHT-SURVIVOR-LEAF (parked-cursor duplicate-emission RED — decision
  4's fail-first proof).
* **Slice 3 — interior merge, cascade, root collapse.** Collapse consumes
  slice 0's revalidation. Controls: drain-to-empty (S5's `empty_tree`
  seeds bind; exact final shape: single leaf root, `pages_freed ==
  built − 1`), regrow-from-collapsed-root (split path from height 1),
  stale-root-reuse ABA control (collapse + forced reuse of the old root
  slot + stale-id seam ⇒ reads correct; mutant NO-REVALIDATE RED here if
  slice 0's control were ever weakened), multi-level cascade shape control
  (interior high-water shrink). Mutants: NO-CASCADE (interior pages leak →
  high-water oracle RED), NO-COLLAPSE (depth oracle RED).
* **Slice 4 — P1 battery + docs + arena.** The 7-day soak on the chosen
  vehicle; churn-driver BEFORE/AFTER published in the commit message
  (M3's pattern: the monotonic-frontier ratchet measured BEFORE, flat
  AFTER); deletechurn RSS movement measured on the nightly arena; band
  update as its own dedicated commit citing this arc; README §Known
  limitations rewritten (the "creeping ceiling" paragraph becomes the
  merge story; the GC-rescan and parked-cursor-blocks-writer notes stay);
  INVARIANTS.md P1 row + G1 amendment + the eight-item gap list
  disposition; CHANGELOG; ROADMAP disposition table row; version per
  §12.3.

## 9. Named risks (recorded, not hidden)

* **R-1 slice-2 scope creep:** the planner reuse (split planner as
  capacity oracle) may surface model mismatches (slot-size differences
  leaf vs interior). Mitigation: interior planner lands in slice 3 with
  its own controls; slice 2 touches leaf geometry only.
* **R-2 abandonment starvation under adversarial contention** (MW8):
  visible via `abandons_lock`; the tracked follow-up (quiescent sweep) is
  the escape hatch. The soak is the measured answer.
* **R-3 soak vehicle does not exist yet** (§12.2) — acceptance leg 1 is
  blocked on an infrastructure decision, not on code.
* **R-4 TSan on the widened latch path:** the top-down exclusive crabbing
  is new lock-ordering territory for TSan's lock-order checker even though
  the protocol is deadlock-free by §1.2's argument; budget one
  investigation cycle in slice 2.

## 10. Checklist the implementation owes this document

1. `free()` contract comment cites decision 1 at every call site (rule 8:
   the code says what the doc decided).
2. Every unlatched page-id walk is classified (see §11) — the inventory
   is re-run at slice 2, not trusted from this document.
3. All new counters appear in `merge_stats()` AND in the deletechurn
   table's reads (the arena's before/after columns need them).
4. Stress points are registered where the seam list expects them
   (`crashfuzz`'s every-point-reached assertion covers crash points; the
   dst seam inventory covers these — an unreachable seam is a control
   failure, per the fired-witness norm).
5. Slice commit messages carry the measured numbers (suite deltas, mutant
   rc/FAIL counts, control counts) — the v29 arc's break-proof ledger
   pattern.

## 11. Unlatched-walker inventory (at `1ab7c8c`; re-run per §10.2)

`free()` gaining callers makes every page dereference outside a latch a
use-after-free candidate. Classified walk-throughs of `pool_.get(...)`:

| Walker | Class | Verdict under M4 |
| --- | --- | --- |
| `find_leaf_crabbing`, `find_leaf_crabbing_with_fence`, `find_parent_crabbing`, `find_leaf_for_scan_crabbing`, Cursor | latched crabbing | safe by MW1/MW4/MW5 (+ revalidation from slice 0) |
| `put_leaf_nolatch`, `erase_recursive` body, `split_leaf`, `interior_install` | caller holds exclusive | safe |
| `require_split_headroom`, `install_new_root`'s walk | under `write_mu_` (structure stable) | safe — merges run under the same mutex |
| `size_recursive` (via `BTree::size`/`tree_size`) | UNLATCHED. Engine caller: `recover_with_checkpoint`'s empty-engine check (quiescent, pre-threads). Diagnostics/tests otherwise. | safe today by quiescence; slice 2 adds a comment marking it quiescent-only and a debug-build latch assert. NOT to be called from live paths — if M5+ ever wants a live size, it uses a cursor. |
| `depth()` | UNLATCHED, diagnostics/tests only (no live engine caller at this commit) | same treatment as `size_recursive` |
| `verify_*` walkers (test hooks) | quiescent by test construction | safe; comment |
| GC's whole-tree rescan | `tree_scan_all` = Cursor | safe (MW1/MW2/MW6) |

## 12. Open questions (maintainer decisions this document defers)

1. **BT-3 sequencing:** land slice 0 as 0.28.2 remediation BEFORE M4
   slice 1 (this document's recommendation, per rule 11), or fold the
   revalidation half into slice 3 and the copy-out half into the M4 arc?
   The repro build (§6 of that document) is the immediate next action
   either way — and it requires lifting this session's no-build
   constraint for a targeted hooks-on build.
2. **P1 soak vehicle** (ROADMAP: GitHub kills jobs at 6 h): self-hosted
   runner, rolling nightly chain with persisted state + aggregate verdict,
   or offline rig with committed logs? Slice 4 is blocked on this; slices
   1–3 are not.
3. **Version identity:** M1–M3 slices shipped under a static 0.28.1 with
   the arc's MINOR (0.29.0?) at completion. M4 follows the same pattern?
   (Rule 3's "version identity in the same commit" is satisfied at the
   arc's merge commits; slice commits cite the arc.)
4. **Decision 4's leftover:** is the bounded one-page leftmost-leaf
   residue acceptable permanently, or should a later slice add the
   monotonic emission filter to unlock right-survivor leaf merges? (This
   document: acceptable; the filter is a scan-semantics change that would
   need its own decision round.)
5. **Underflow fraction:** `PAGE_SIZE / 4` is the classic quarter-fill;
   the byte-based test makes the exact value a tuning knob, not a
   correctness parameter. Ship with 1/4, record churn-driver sensitivity
   in slice 4?
