# M3 reclamation: the HP4 / page-reuse ABA decision

**Date:** 2026-10-12
**Status:** DECIDED — this is the "made **before** implementation" decision the
v29 M3 milestone requires (ROADMAP §M3: "retire pages through the epoch
reclaimer or generation-tagged ids with the ABA (HP4) decision made before
implementation").
**Scope:** reuse-safety mechanism selection for v29 M3 (page reclamation) and
its consumer M4 (leaf merge/rebalance). NOT in scope: the purge policy that
decides *when* a leaf becomes dead (an M3 implementation design point, §10),
M4's merge algorithms, and the pool-sizing/health thresholds (already fixed by
the M3 roadmap text).
**Code basis:** every anchor below was read at 0.28.1, commit `33aab1b`
(2026-10-12). Citations use function-scope anchors per the reading rules —
line numbers rot in this repo; names do not.

---

## 1. The decision

1. **Page-slot reuse safety = generation-tagged slots.** `PagePool` gains a
   pool-side per-slot generation table (`std::vector<std::atomic<uint32_t>>`,
   sized `capacity_ / PAGE_SIZE` at construction — 64K entries / 256 KiB for
   the 256 MiB default, fixed, never grown). `alloc()` bumps the slot's
   generation (release) under the pool mutex **before** the memset that makes
   the slot reachable. Every code path that obtained a `PageId` under one
   latch and dereferences it under another (the handoff windows, §3) must
   capture the slot generation together with the id (under the source latch)
   and re-verify it **after acquiring the new latch and before trusting any
   content**; mismatch forces the existing re-descend/restart path — it is a
   normal control-flow event, never an error, never a health signal.
2. **`PagePool::free()` returns slots to the free list immediately.** No
   epoch-delayed reuse for pages: with (1), a reused slot is *detectable* at
   every trust point, so deferral buys nothing and would re-couple page
   reuse latency to the slowest reader pin — the coupling that would let a
   long-lived `RangeScanStream` ratchet the pool high-water, against M3's
   P1-adjacent acceptance ("high-water mark stabilizes instead of
   ratcheting").
3. **Heap-object lifetime stays on the epoch-pinned reclaimer** (v23 Phase C:
   `retire()` / `reclaim_retired()` / `ReaderSlot::pinned_epoch` /
   `min_active_pin_epoch()`). M3 extends it to `KeyEntry` retirement on
   physical purge ("deferred reclamation via retire() on erase, same pattern
   as Version nodes" — the `ChronoKV` member comment already specifies this);
   the safety argument is re-derived in §7.3, not assumed.
4. **The hazard-pointer machinery is DELETED in M3, not wired.**
   `HazardPointer`, `HPRegistry`, `Cursor::hp_` and its
   publish/clear/register/release calls go away (the M5 culture: the code
   stops existing). Both audits already demanded this shape — "must be
   redesigned, not merely wired, before reclamation lands" (2026-09-23 audit,
   test-coverage findings) and "Redesigning it is a reclamation-project
   prerequisite" (remediation spec, beyond-CKV-018 section) — and §4.3 shows
   the redesign converges to (1) with less machinery. Suite inventory at
   `33aab1b`: no test touches the HP API (one historical comment in
   `main.cpp`'s H1 notes), so removal is header-internal.
5. **CKV-008's `mut_epoch` is untouched and orthogonal.** It answers "was
   this *live* page mutated in place since the fence was captured"; the slot
   generation answers "is this the *same page lifetime*". The fence carries
   both; the reuse-alias argument against `mut_epoch` ("a freed and
   re-allocated PageId restarts its epoch near zero", ROADMAP §M3) becomes
   moot because the generation mismatch fires first — `alloc()`'s memset
   zeroes `mut_epoch` with the rest of the header, and the bump precedes it.
6. **No on-disk or format change anywhere.** Page images are never persisted
   (`checkpoint_locked` serializes logical entries via `tree_scan*`; the WAL
   is logical records; `PageHeader`'s CKV-008 comment already records
   "in-memory format only"). The generation table dies with the process;
   recovery rebuilds the pool fresh, so no reference can straddle an open
   and no migration rule is owed (v29's first format change remains M6's
   PITR deltas, as planned).

**The principle in one sentence:** *value-identity references (PageIds) are
made safe by verification — generations checked at every latch handoff;
address-identity references (`KeyEntry*`, `Version*`) are made safe by
deferral — the epoch-pinned reclaimer.* Pages cannot be epoch-pinned cheaply
(their holders include unpinned writer descents, §4.1) and heap pointers
cannot be generation-tagged (a dangling `KeyEntry*` dereference is UB before
any check can run); each class gets the mechanism that actually fits it.

---

## 2. Why a decision was required (the problem, precisely)

`PagePool` is a fixed arena: `get(id)` is pointer arithmetic
(`(id - 1) * PAGE_SIZE` into one `aligned_alloc` block, no bounds check on
the hot path). Consequences, all read at `33aab1b`:

* **A freed page is never unmapped.** "Use-after-free" of a page is not a
  SEGV and not an ASan report — the pool is one large allocation, and
  ASan's intra-object checks are off by default. A stale `PageId`
  dereference reads *plausible memory*: after reuse, a valid `PageHeader`
  and slots belonging to somebody else's page. This is the silent-corruption
  class, invisible to every sanitizer leg CI runs. Any reuse mechanism whose
  failure mode is "the check usually catches it" is unacceptable; detection
  must be exact by construction.
* **The ABA is concrete, not theoretical.** `alloc()` memsets the slot, so
  `mut_epoch` restarts at 0. A fence captured before a free carries
  `(min_key, max_key, key_count, mut_epoch=E)`; after free+reuse, the slot
  can present the same quadruple — epoch `E` again after `E` mutations of
  the new occupant, and min/max/count coinciding is a birthday problem, not
  an impossibility, under merge churn. `fence_unchanged()` then validates a
  page that is not the page. This is exactly the ROADMAP §M3 sentence:
  "a pre-free captured fence can alias, which is exactly the ABA the HP4
  decision exists for."
* **The v25.1 note in `PagePool::free()` itself defers here:** "the ABA
  interaction with hazard pointers is specified in v25.1 §6 (HP4 — the
  free-list reuse is tagged or Version allocations are never reused)". One
  horn of that disjunction ("never reused") is the pool's behavior today and
  is precisely what M3 exists to end; this document picks the other horn
  (tagged) and generalizes it from "Version allocations" to all slots.

---

## 3. Reference-window inventory (what holds a PageId across what)

The complete set of windows in which a `PageId` (or a `Page*`) is held
across a latch boundary, at `33aab1b`. Merge/rebalance (M4) is the first
producer of frees; every window below is a consumer the mechanism must
cover. Anchors are function names.

| # | Window | Identity held | Guard today | Under free+reuse |
|---|--------|---------------|-------------|------------------|
| W1 | **Cursor leaf-switch gap** — `Cursor::advance_to_valid` reads `next_leaf_id` under the current leaf's shared latch, calls `leave_leaf()` (latch released, HP cleared), stress point `tree_cursor_leaf_switch`, then `enter_leaf()` latches the successor | bare `leaf_id_` | **nothing** between leave and enter — the HP is cleared *before* the gap, so the HP machinery does not even cover its own headline window | successor freed+reused in the gap ⇒ cursor resumes iteration inside a foreign page; the debug-only `verify_fence_consistency` assert is the sole (NDEBUG-compiled-out) tripwire |
| W2 | **Writer leaf-upgrade gap** — `put_recursive` / `erase_recursive`: crabbing descent returns the leaf latched shared (`find_leaf_crabbing_with_fence`), the descent latch is released, stress point `tree_leaf_latch_gap`, then the leaf is re-acquired EXCLUSIVE and the fence re-checked | `leaf` id + `LeafFence{min,max,count,mut_epoch}` | fence re-check (`fence_unchanged`) → re-descend | reuse can alias the full fence quadruple (§2) ⇒ the writer mutates a foreign page believing it is the target |
| W3 | **Fence re-descend loop** — on mismatch, both paths reload `root_id_` and re-walk; each iteration re-enters W2 | root/child ids per iteration | crabbing is latch-coupled *within* a descent (parent held until child latched), so no latch-free window inside one walk | covered by W2's check at every exclusive acquisition; the loop itself is safe |
| W4 | **GC / checkpoint tree walks** — `gc_once` and `checkpoint_locked` collect `KeyEntry*` via `tree_scan_all()` (a Cursor) then dereference after the walk | `KeyEntry*` (heap, address identity) | reader-slot epoch pins: checkpoint registers `ckpt_slot`; GC reclamation runs only inside `gc_once` under `gc_active_mu_` (same thread as the walk) | unaffected by *page* reuse (entries are heap); the page layer under the walk is W1; the heap layer is §7.3 |
| W5 | **Cursor residency** — a latched leaf while emitting pairs | `leaf_id_` + shared latch | the latch itself: any future freer (M4 merge) must take the leaf EXCLUSIVE to unlink it, so residency blocks the free | safe by latch discipline; the HP publish during residency is belt-and-braces the freer never needed |
| W6 | **`RangeScanStream`** — long-lived `ChronoKVRangeScanCursorState` holding a Cursor + SnapshotGuard across user-code gaps | as W1, repeatedly | as W1 | as W1; also the strongest argument against epoch-deferred reuse (§4.1): a stream's pin would stall the free list for its whole lifetime |

The generation check therefore has exactly **two verification sites** —
`enter_leaf` (W1/W6) and the exclusive re-acquisition fence check
(W2/W3) — plus one capture extension at each site's source latch
(read the slot generation where the id is read: under the predecessor leaf
latch for `next_leaf_id`, inside `LeafFence` for descents).

**Why capture-under-latch is sound** (the ordering argument M3's
memory-ordering audit section must carry): a page's generation is constant
while the page is linked in the tree — a free requires an EXCLUSIVE latch on
the page (M4 unlink discipline) which the capturer's shared latch on the
*linking* page (predecessor leaf / parent) excludes for the chain field
being read. The bump itself happens under the pool mutex at alloc, before
the memset, before the slot is re-linked anywhere; the verifier's latch
acquisition is the acquire edge; a release store on the bump completes the
pair. Generation wrap: 32 bits, one bump per reuse of one slot; the
capture→verify window is a single latch acquisition (µs), so a wrap inside
the window needs 2^32 free+alloc cycles of the *same slot* in microseconds —
the same "cannot realistically wrap" argument CKV-008's 32-bit `mut_epoch`
already carries, with eleven more orders of magnitude of headroom (epochs
bump per mutation; generations per reuse).

---

## 4. Options considered

### 4.1 Extend the epoch-pinned reclaimer to pages (REJECTED as the page mechanism)

The v23 Phase C protocol works and is now gate-covered (DST S6's
pinned-block witness + drain oracles; the v17 boundedness accounting). Pages
could ride it: `retire_page(id)` tags with `reclaim_epoch_`, the free list
gains entries only when `retired_epoch < min_active_pin_epoch()`.

Why it does not fit pages:

* **Writer descents do not pin.** `commit_txn`'s tree work (W2/W3) holds
  latches and fences, never a `ReaderSlot`. Epoch-deferred reuse is only as
  safe as the pin coverage of *every* raw-id holder, so either (a) every
  commit acquires a reader slot for its descent — a `reader_mu_` round-trip
  on the write hot path, and `min_active_pin_epoch` semantics now conflate
  read snapshots with write descents — or (b) a second barrier tracks
  in-flight descents, which is a new protocol with the same shape as the one
  being avoided. Neither is "reuse the existing reclaimer" any more.
* **Reuse latency couples to the slowest pin.** W6 streams pin for their
  whole (user-paced) lifetime; every page retired under a stream waits for
  it. Under delete-churn with one long stream open, the high-water ratchets
  for the stream's duration — behaviorally indistinguishable from the
  defect M3 exists to fix, and hostile to the P1 soak acceptance.
* **Deferral does not remove the need for detection.** Even with epochs,
  the moment a page IS reused, W1/W2 holders from before the retire cycle
  are exactly the alias case — the epoch argument says they are gone, but
  the argument's premise (complete pin coverage) is the thing (a)/(b) above
  would have to build. Generations make the same window *checked* instead of
  *argued*, at one relaxed load per handoff.

The epoch reclaimer remains the right mechanism where its premise already
holds — heap objects dereferenced by slot-pinned readers (§7.3).

### 4.2 Generation-tagged slots (CHOSEN)

As specified in §1. Cost: one fixed-size atomic array; one release bump per
alloc (already a mutex-serialized, memset-paying path); one acquire load +
compare per latch handoff (two sites). Failure mode of a *missed* check is
the status quo ante (aliasing possible under merge churn), so the
implementation checklist (§6) enumerates the sites exhaustively and the
fail-first plan (§9) mutates each one. No format change, no protocol
additions, no hot-path mutexes, immediate reuse (W6-immune), and the fence
machinery CKV-008 built is extended with one field rather than duplicated.

The pool-side table (not a `PageHeader` field) is deliberate: the header is
a 32-byte `static_assert`-guarded packed layout whose reserved bytes are
already spent (`mut_epoch` took both halves in CKV-008); a pool-side table
touches no layout, survives the `sizeof(PageHeader) == 32` invariant
untouched, and keeps the generation where its mutator already is (the pool
mutex in `alloc()`).

### 4.3 Wire/redesign the hazard pointers (REJECTED — machinery deleted instead)

The HP registry (per-BTree, slot-recycled since the H1 leak fix) protects
only *residency* (W5) — which the latch already protects, since a freer
must take the page exclusive. Extending HPs to W1/W2 means publishing at
every handoff with the classic publish-then-revalidate protocol (a bare
publish races the free between "read id" and "publish"; the revalidate loop
is the part HP implementations get wrong), on seq_cst stores, in the descent
hot path. That is strictly more protocol surface than §4.2 for the same
coverage, and both audits independently concluded the machinery needs a
redesign rather than consumers. The redesign that survives review is the
generation check: same question ("is this slot still the page I think it
is?"), answered by a value comparison instead of a distributed publication
protocol. Cursor residency keeps its latch; `HPRegistry`'s diagnostics
(`slot_count`/`in_use`) are replaced by pool-side generation/free-list
counters feeding M3's `stats()` additions.

### 4.4 Never reuse (REJECTED — the status quo)

The other HP4 horn: monotonic bump allocation, `free()` dead, the pool a
ceiling. That is the documented limitation M3 exists to remove ("the pool
is a monotonic ceiling", README limitation #1 and the audit's GC row);
choosing it would delete the milestone, not satisfy it.

---

## 5. What the decision does NOT decide

* **The purge policy** — when GC concludes a key is fully dead (tombstone
  head, all versions below the safe threshold) and physically erases it
  from the leaf, and when an emptied leaf becomes free-able without M4's
  rebalance (root-collapse-only? tolerate underflow until M4?). M3's
  acceptance ("delete-then-reinsert high-water stabilizes") requires *some*
  shrink path to exist in M3; its shape is an M3 implementation design
  point with its own measure-first duty (the arena's delete-churn table is
  the instrument).
* **M4's merge algorithms** — borrow vs merge, crabbing-with-exclusive on
  the delete path, interior key deletion, cascade, root collapse. They
  inherit this decision: a merge's unlink-then-free must take the freed
  leaf's exclusive latch (W5's premise) and the parent's (chain-field
  consistency), then `pool_.free(id)`; nothing else is owed to the page
  layer.
* **The BT-1 headroom re-derivation** — ROADMAP §M4 already assigns "M3's
  reuse work and BT-1's `5*(height+1)+1` bound get re-derived together,
  with the split planner as the single source of capacity truth".

---

## 6. M3 implementation checklist (mechanism only; each item testable in isolation)

1. `PagePool`: `std::vector<std::atomic<uint32_t>> slot_gen_` sized at
   construction; `alloc()` bumps (release) under `mu_` before the memset;
   `slot_generation(PageId)` public accessor (acquire); free-list counters
   for `stats()`. `free()` itself is unchanged (immediate push) — and
   finally gets its engine callers.
2. `BTree::slot_generation(id)` forwarder (BTree owns the pool reference
   discipline; Cursor and descents go through the tree face).
3. `LeafFence` gains `uint32_t gen`; captured in
   `find_leaf_crabbing_with_fence` under the leaf latch; checked in
   `fence_unchanged` (W2/W3 closed — mismatch keeps the existing
   `continue`-to-re-descend path, `fence_recheck_retries` counter
   unchanged in meaning).
4. `Cursor`: stores `leaf_gen_` alongside `leaf_id_` at every enter
   (source: read under the predecessor latch in `advance_to_valid`'s
   switch, and under the descent latch in `enter_leaf_from_crabbing`);
   `enter_leaf` verifies after latch acquisition, before any header read;
   mismatch ⇒ restart: re-descend from the last emitted key (exclusive
   successor seek — keys are unique and ordered, so re-emission is
   impossible and no output is skipped) and set a `cursor_restarts`
   diagnostic. W1/W6 closed. `hp_` and its publish/clear/register/release
   calls removed in the same edit.
5. Delete `HazardPointer`, `HPRegistry`, `BTree::hps_`; repoint the H1
   historical comment; replace registry diagnostics with pool counters.
6. `KeyEntry` retirement on physical purge through the existing
   `retire()`/`reclaim_retired()` path (§7.3's argument goes into the
   header's memory-ordering audit section as E-series siblings, per that
   section's own rule: "Do not change an order without re-running TSan and
   updating this audit").
7. `stats()` gains pool utilization + high-water; `health()` degrades at
   80%, fails at 95% (roadmap-fixed thresholds); `LatchTable::erase` stays
   uncalled and is documented as such (entries recycle with slots; the
   table is bounded by peak live pages — pool capacity — not by cumulative
   frees) or deleted; the choice is free, both are bounded.
8. Memory-ordering audit section extension: the bump/before-memset/release
   and verify-after-latch/acquire pairing; the generation-wrap argument
   (§3) recorded beside CKV-008's epoch-wrap argument.

## 7. Safety arguments M3 must carry (not assume)

### 7.1 Pages: G1–G3 (proposed canonical IDs for the invariant matrix)

> **G1 (identity)** — every free-list→allocated transition of a slot bumps
> its generation exactly once, under the pool mutex, before the slot's
> contents or linkage become reachable.
> **G2 (verification)** — no code path trusts page *content* reached via an
> id that crossed a latch handoff without first verifying the captured
> generation against the slot's current generation under the newly acquired
> latch; mismatch routes to re-descend/restart, never to an error path and
> never to `health()`.
> **G3 (scope)** — heap-address references (`KeyEntry*`, `Version*`) are
> never generation-checked; their lifetime is governed exclusively by the
> epoch-pinned reclaimer (E-series). A design that mixes the two (tagging
> pointers, or epoch-deferring pages) reopens the analysis this document
> closes.

The window inventory (§3) is the completeness argument for G2: two
verification sites cover W1–W6 because crabbing is latch-coupled within
descents (W3) and residency is latch-protected (W5).

### 7.2 Latch-entry ABA

A thread blocked acquiring the latch of a freed-then-reused slot wakes
holding the latch of the *new* occupant (the `shared_mutex` recycles with
the slot — `LatchTable` keys on the bare id). This is safe under G2's
ordering rule: the latch is acquired, *then* the generation is checked,
*then* content is trusted. The waking thread's check fails and it
re-descends. (This is why G2's ordering is "after latch, before content"
and not merely "somewhere".)

### 7.3 Heap layer: KeyEntry retirement under the existing pins

The E1–E16-shaped argument, re-derived for entries rather than assumed:
reclamation executes only inside `gc_once`/`reclaim_retired` under
`gc_active_mu_` (single-threaded with respect to its own walks — W4's
collected pointers cannot race the frees, which happen in the same pass
thread); external dereferencers of `KeyEntry*` are readers (slot-pinned:
`read_at_idx`/`range_scan` hold a `SnapshotGuard`) and checkpoints
(`ckpt_slot`); a retire tagged at epoch E is freed only when every active
pin is strictly newer than E, and a pin taken before the retire blocks it —
identical algebra to Version suffixes, with the same DST S6 witness now
covering the machinery. New obligation unique to entries: the *tree* slot
holding the encoded pointer must be unlinked (under the leaf's exclusive
latch, W2 discipline) **before** `retire(entry)`, so no post-unlink fetch
can hand out a pointer whose retirement epoch is already counting down.

## 8. Measure-first plan (rule 13; instruments already in the tree)

* **Handoff-check overhead:** arena fill/workload legs (v29 M1 vehicle) with
  the generation checks compiled in vs. a scratch `-D` bypass: expected at
  noise level (one relaxed/acquire load + compare per handoff); the number
  goes in the M3 closure report either way.
* **Reuse behavior:** the arena's delete-churn table (M3's acceptance
  instrument) — high-water curve flat vs. ratcheting across a
  delete-then-reinsert workload, with `cursor_restarts` and
  `fence_recheck_retries` published beside it (restart storms under churn
  are the cost side of the safety).
* **Latch-table bound:** peak live pages vs. table size under the churn
  workload (the §6.7 boundedness claim, measured once).
* **Stream immunity:** a long-lived `RangeScanStream` open across a churn
  phase must not move the high-water (the §4.1 rejection reason, verified
  as a property of the chosen mechanism, not just argued).

## 9. Fail-first plan for M3 (mutation gates at landing)

Each mutation must redden the named catcher on a scratch build, with the
clean tree green (the item-3 methodology: old/new side by side where a
legacy behavior exists):

* **alloc without the bump** (pure ABA): the reuse-churn differential
  (delete-churn workload + full-range scan differential + DST S5's pinned
  prober once purge lands) — silent corruption surfaces as
  order/legitimacy/differential violations, not crashes (§2: sanitizers
  stay green by construction; that blindness is why this mutant is
  mandatory).
* **Cursor switch verification removed** (W1 open): DST S5/S6-class pinned
  scans + the restart counter pinned at 0 on a run where the clean tree
  shows restarts > 0 (the witness that the window is real, per the item-4
  fired-witness pattern).
* **`fence.gen` removed from `fence_unchanged`** (W2 open): the concurrent
  putters battery under merge-active builds (M4's landing gate re-runs it);
  at M3 (merge-free) this mutant is UNREACHABLE — recorded as such rather
  than faked, and re-run when M4 lands, the S5-grammar lesson applied to
  mutation proofs.
* **TSan + ASan legs** with reuse enabled: heap-layer regressions
  (KeyEntry retire ordering, §7.3's unlink-before-retire) are theirs to
  catch; the page layer's aliasing is the mutants' job (§2).

## 10. Explicitly deferred to M3 implementation

* Purge-trigger shape (inside `gc_once`'s sweep vs. a dedicated sweeper
  pass) and the dead-key criterion's interaction with PITR windows
  (a tombstoned key whose versions a PITR as-of could still read is NOT
  purge-eligible below the PITR horizon — v26.1's delta-filtering lesson
  applies to physical purge too).
* Cursor restart positioning cost under churn (measure before choosing
  between last-emitted-key seek and full re-descent).
* Whether descent-side verification reads the generation once at capture
  into the fence (chosen here) needs a second read at the exclusive
  acquire, or can reuse the fence's other fields' read (implementation
  detail; the invariant is G2's ordering).

## 11. Provenance

* Audit 2026-09-23, test-coverage findings: "`PagePool::free` … and
  `HPRegistry::is_hazardous` … have zero callers — the hazard-pointer
  machinery publishes with seq_cst but nothing consults it, so it protects
  nothing today (harmless while `free()` is dead; must be redesigned, not
  merely wired, before reclamation lands)."
* Audit 2026-09-23, invariant matrix row PAGE/OLC and the fence narrative:
  "The fence triple is the only validation, and it is not a version: two
  different tree states share it. The hazard pointers protect page
  lifetime, not logical staleness." (CKV-008 later added `mut_epoch` for
  the staleness half; this document supplies the identity half.)
* Remediation spec 2026-09-24, CKV-008 notes: "(4) Do NOT wire tree_->erase
  or hazard-pointer reclamation as part of this fix — those are separate
  roadmap items"; beyond-CKV-018 section: "Hazard-pointer machinery having
  no consumers … Redesigning it is a reclamation-project prerequisite."
* ROADMAP v29 §M3 (the mandate, the CKV-008-is-not-reuse-safety argument,
  the stats/health thresholds) and §M4 (the consumer, the BT-1
  re-derivation, P1).
* v23 Phase C epoch-pinned reclamation (the heap-layer mechanism this
  decision keeps) and its 2026-10 gates: DST S6 `reclaim_retire_pin`
  (pinned-block witness + drain + controls), the de-vacuated v17
  boundedness accounting, and the v18.2-filter regression's structural-
  closure methodology (read the locks, then measure the claim) — the same
  methodology this document's §3/§4.1 follow.
* `PagePool::free()`'s own v25.1 HP4 deferral note (the disjunction this
  document resolves) and the H1 registry-leak fix (the machinery's last
  repair before its scheduled deletion).
