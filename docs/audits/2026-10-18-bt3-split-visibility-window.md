# BT-3 (candidate): the split rebuild→splice visibility window

**Date:** 2026-10-18
**Status:** **REPRODUCED (deterministic, verbatim in §11) and FIXED
(2026-10-18, same arc)** — the fail-first repro ran BEFORE the fix per
rule 1; the fix is the two-mechanism package of §7 (copy-out split +
root revalidation), controls bt3 A–E in the stress suite
(`CKV_ONLY_BT3=1`), mutant matrix with exact co-flip isolation in §11.
The originally-proposed 0.28.2 version stamp is SUPERSEDED by the v29
arc's observed pattern (M1–M3 shipped under the static 0.28.1 constant
with `## Unreleased` changelog entries; BT-3 rides the same pattern —
the version stamp belongs to the arc's release boundary, not to this
remediation).
**Found by:** M4 pre-work design reading (the merge protocol is the split
protocol's mirror; enumerating the split's reader-visible windows found this
one). This is an independent-of-the-fixer finding channel per rule 12 — the
repro harness should be built and run by whoever picks it up, and the defect
stands or falls on the repro, not on this document.
**Scope:** `chronokv_btree::BTree` split protocol and every point-read path
that descends through it. NOT in scope: the merge algorithms themselves
(`2026-10-18-m4-merge-rebalance-decision.md`), cursor/scan semantics (immune,
§4), and the WAL/durability layer (untouched).
**Code basis:** every anchor below was read at 0.28.1, commit `1ab7c8c`
(v29 M3 slice 3/3). Citations use function-scope anchors — line numbers rot
in this repo; names do not.

---

## 1. The claim

`BTree::get` (and therefore `ChronoKV::find_index`, `Database::get`,
`Transaction::get`, `get_async`) can return **false-negative "absent" for a
key that exists in the tree** — including keys whose writes were
acknowledged arbitrarily long before the read began — while a concurrent
split is between its destructive leaf rebuild and its parent splice.

This contradicts the flagship acknowledgment property (README §Safety
properties, v27 M1): "if a write is acknowledged before a transaction
begins, that transaction's snapshot — and every read in it — includes the
write." A false-absent read of an acked key is a real-time-order violation,
i.e. a strict-serializability break of the same class as TXN-1 (which was
graded Critical: "a soundness hole in the flagship claim").

## 2. Anchors

* `BTree::split_leaf` — the v28 CKV-002 plan-before-mutate rebuild. Its own
  comment: *"Pages 1..k-1 are unreachable until the parent splice, and
  page 0 is under the caller's exclusive latch until the split returns —
  the chain is only observable once fully built."* The MUTATE loop memsets
  **every** group page including page 0 (`pages[0] == page_id`) and
  reinserts only `groups[0]` (the LOW group) into it.
* `BTree::put_recursive` — on `r.split`: `leaf_excl.unlock()` **first**,
  then `insert_into_parent_multi(cur_root, leaf, r.promotions,
  r.last_new_page)`. The splice into the parent happens AFTER the split
  leaf's exclusive latch is released.
* `BTree::put` / `BTree::put_with_old` — root-split case:
  `install_new_root(r.promotions, r.last_new_page)` runs after
  `put_recursive` returned, i.e. after the leaf rebuild AND its latch
  release; `root_id_.store(new_root, release)` publishes last.
* `BTree::insert_into_parent_multi` → `interior_install` →
  `split_interior_planned` — the interior-level split rebuilds the parent
  page the same destructive way and recurses upward, so the window exists at
  **every level** of a cascading split, not only the leaf.
* `BTree::get_recursive` — shared-latch crabbing descent
  (`find_leaf_crabbing`), single linear slot scan, `return false` on miss.
  No fence, no epoch, no generation check, no sibling fallback, no retry.
  (Contrast the writer paths: `put_recursive`/`erase_recursive` capture a
  `LeafFence` and re-check under the exclusive latch.)
* `ChronoKV::find_index` — *"no nm_ — tree_->get uses shared latch crabbing
  internally."* `ChronoKV::get` → `find_index` → `entry == nullptr` →
  `return false`. No retry at engine level.

## 3. Reachability walkthrough (leaf level, k = 2)

Precondition: key `K` is committed, acked, and lives in leaf `L`
(`page 0`); a concurrent writer inserts a new key that overflows `L` at
separator `F`, with `K >= F` (K lands in the HIGH group).

1. Writer (under `write_mu_`, and under `nm_` exclusive when the engine's
   `ensure_index` slow path is the caller): descends, takes `L` exclusive,
   fence verified, `split_leaf` plans, allocates `R` (`pages[1]`), then
   MUTATES: `L` is memset and rebuilt holding only `[min(L), F)`; `R`
   holds `[F, max(L)]` including `K`; `L.next := R`; `R.next := old_next`.
2. Writer releases `L`'s exclusive latch (`put_recursive`'s
   `leaf_excl.unlock()`).
3. **The window is open.** The parent still holds slot
   `(key_j, left child = L)` — the promotion `(F, L)` and the repoint of
   `key_j`'s left child to `R` have not been installed.
4. Reader thread calls `get(K)`: `root_id_` load → shared crabbing → parent
   routes `K` (first separator `> K` is `key_j`, its left child is `L`) →
   latches `L` shared → scans `L` → `K` is not there (it is in `R`, which
   is descent-unreachable: the parent does not point at it) → `false`.
5. Engine: `find_index` returns `nullptr`; `Database::get` returns
   `nullopt`; a `Transaction::get` reads absent and can COMMIT with that
   read — nothing conflicts: `K` itself is not being written concurrently
   (only its slot moved), so neither the phantom tracker (TXN-1's
   committed-write recording) nor write-set validation sees an anomaly.
6. Writer completes `interior_install`: the parent now routes `[F, key_j)`
   to `R`. Later `get(K)` succeeds. The false-absent is transient — and
   already acknowledged to the caller.

The interior-level variant is the same shape one level up: after
`interior_install` rebuilds parent `P` (group 0 only) and releases `P`'s
exclusive, and before the recursion splices `P`'s parent, keys routing into
`P`'s upper-group subtrees descend through `P` to its group-0 rightmost
child and miss.

The root-split variant adds an **unbounded** window: a reader loads
`root_id_ == L` (the old root leaf) and is preempted; the split rebuilds
`L`, `install_new_root` publishes a new interior root; the reader resumes,
latches `L` — a leaf — and scans it. `L` permanently lacks the high group
now, so the reader misses `K` no matter how the splice timing falls. The
writer-side stale-root hazard is already handled (v25.7 review note 3:
`put_recursive`/`erase_recursive` reload `root_id_` per iteration); the
READER side has no equivalent, and a fence could not supply one — the
reader never captured anything to re-check against.

## 4. What is NOT affected (and why the suite is green)

* **Scans are immune.** `split_leaf` builds the chain (`L.next := R`) under
  `L`'s exclusive latch; `Cursor` reads `next_leaf_id` under its leaf latch
  and re-verifies slot generation on switch (`enter_leaf`, v29 M3 slice 2).
  A cursor either blocks on `L` until the rebuild completes (then sees the
  full chain) or enters after. S4 `tree_cursor_split` and `check_scans`
  exercise exactly this — chain-based — path, which is why they never saw
  the defect.
* **`ensure_index` self-heals.** A fast-path miss falls to the slow path,
  which blocks on `nm_` exclusive — held by the splitting writer for the
  whole `tree_->put` — and re-runs `tree_->get` after the splice. Winner /
  loser-delete semantics are unaffected.
* **Writers are immune.** All structural writers are serialized
  (`write_mu_` in the standalone tree; `nm_` exclusive around every
  engine-side `tree_->put`/`tree_->erase`), and writer descents carry the
  `LeafFence` (mut_epoch + slot generation) with re-descend on mismatch.
  With writers serialized, `fence_unchanged` currently has no engine-side
  producer — it is defense-in-depth and the machinery M4's merges will
  lean on.
* **No deterministic hook sits in the window.** The registered stress
  points bracket it but never open it: `tree_split_begin` fires inside
  `split_leaf` (latch held — readers are excluded, not exposed);
  `tree_leaf_latch_gap` fires in the descent BEFORE the leaf exclusive is
  taken. DST seeds cannot schedule an interleaving no point instruments, so
  150/150 DST and the lincheck runs are consistent with the defect being
  live: hitting it requires OS preemption inside a sub-microsecond
  in-memory window AND the lookup targeting a just-moved key.

## 5. Severity

* Class: **Critical candidate** by the TXN-1 precedent (breaks the
  flagship strict-serializable-ack claim; observable through the public
  API; silently — no throw, no health signal, no diagnostic).
* Probability per split: very low (µs window × moved-key targeting). It is
  NOT zero under load, it is deterministically reachable once a stress
  point exists in the window (§6), and M4's acceptance runs (7-day soak,
  DST ×100k, lincheck batteries over a merging tree) are exactly the
  regimes that would surface it as unattributable flakes — with merges as
  the default suspect.
* Data safety: none (in-memory index only; WAL/checkpoint/recovery
  untouched; no corruption — the tree converges to correct).

## 6. Fail-first repro plan (rule 1 — RED before any fix)

Instrumentation (test-hooks-gated seams, production-inert):

1. `stress_point("tree_split_splice_gap")` in `put_recursive` between
   `leaf_excl.unlock()` and `insert_into_parent_multi`.
2. `stress_point("tree_root_install_gap")` in `put`/`put_with_old` between
   `put_recursive`'s return (`r.split == true`) and `install_new_root`.

Control A (splice gap, leaf level): build a two-leaf-boundary tree; thread
W puts the key that splits leaf `L` and parks at
`tree_split_splice_gap`; thread R then `get(K)` for a pre-existing `K`
known to land in the high group. **Expected on current code: R observes
absent → control FAILS (the repro is the defect).** Post-fix: R observes
`K`.

Control B (root install gap): height-1 tree; W parks at
`tree_root_install_gap`; R `get(K)` for a high-group key. Expected same
shape.

Control C (stale-root descent): seam captures `root_id_` into a test hook
before W's split completes, then R descends through the captured stale id
after `install_new_root` + high-group mutation of `K` (a second put
updating `K`'s value). Expected on current code: absent (post-rebuild `L`)
— and after the copy-out fix WITHOUT root revalidation: a STALE VALUE
(§7's S-W3), which is why the fix is a package.

Each control runs old-RED/new-GREEN per the standing norm, plus the
reversion mutants of §8.

## 7. Proposed fix (two mechanisms, one package)

### 7.1 Copy-out split (non-destructive until the parent is spliced)

Reorder `split_leaf` + `put_recursive` so page 0 is a SUPERSET until the
splice is published:

* **Phase 1 — under page0 exclusive:** plan (unchanged CKV-002 planner);
  allocate pages 1..k−1; fill them with groups 1..k−1 and their internal
  chain (`page_j.next := page_{j+1}`; `page_{k−1}.next := old_next`);
  **leave page 0 byte-identical** (all entries, old fence, old epoch,
  `next := old_next`). The new pages are unreachable by descent (parent
  unspliced) and by chain (page0 bypasses them) — strictly less observable
  than today's rebuild.
* **Phase 2 — latch released:** splice the parent exactly as today
  (`insert_into_parent_multi` / `install_new_root`). After publication,
  descent routes group-j keys to page j, which is complete. Descent via a
  stale parent (or stale root) routes to page 0, which still CONTAINS
  every key — no miss is possible in the window.
* **Phase 3 — under page0 exclusive:** trim page 0 to group 0 (removal
  only — allocation-free, cannot fail), set `page0.next := page1`, bump
  mut_epoch, refence. Cursors inside page 0 block the trim (exclusive);
  cursors that captured `next = old_next` before the trim emitted the
  superset and skip pages 1..k−1 correctly (their contents were emitted
  from page 0); cursors entering after the trim follow the full chain. No
  emission can duplicate or drop (argued per interleaving class; the S4/S5
  order+uniqueness+legitimacy oracles bind it).

Why not hold the leaf latch across the splice (the obvious alternative):
upward latch acquisition while holding the leaf deadlocks against crabbing
readers — a reader holds the parent shared and waits for the leaf shared;
the writer holds the leaf exclusive and waits for the parent exclusive;
`std::shared_mutex` has no timeout. The release-then-splice ordering in
`put_recursive` is correct as a deadlock avoidance; the defect is the
destructive rebuild on the wrong side of the release, not the release.

Exception safety strictly improves: today a throw between rebuild and
splice leaves page 0 missing keys AND orphans the new pages; under
copy-out, a Phase-2 throw leaves every key reachable via page 0 (orphaned
new pages only — same leak class as today, minus the visibility hole).
BT-1's headroom reservation (`5*(height+1)+1`) is unchanged: Phase 1
allocates the same pages, Phase 3 allocates none.

### 7.2 Root revalidation after latch (all descents)

In every descent entry (`find_leaf_crabbing`,
`find_leaf_crabbing_with_fence`, `find_parent_crabbing`, the Cursor's
initial descent), after latching the root slot and **before dereferencing
any byte of it**: re-load `root_id_` (acquire); if the latched id is no
longer the root, release and retry from the fresh load. One relaxed-path
atomic load; no byte of a stale root is ever read, so a freed-and-reused
root slot (M4's root collapse) cannot alias into a content read — the
check fails first. ABA is benign by construction: if the slot IS the
current root again (re-allocated as a new root), its contents are the
valid current root and the descent is correct against the current tree.

This is also the exact mechanism M4's root collapse needs (freeing the old
root makes stale-root descent a use-after-free candidate, not just a stale
read) — one mechanism discharges both.

### 7.3 Windows the package must cover (checklist)

* **S-W1** splice-gap point-read miss (leaf + every interior level of a
  cascade) → closed by copy-out (page 0 superset until splice published;
  interior rebuild gets the same copy-out treatment via
  `split_interior_planned`: group-0 page keeps all slots until the
  grandparent splice, trim after).
* **S-W2** stale-root descent (unbounded under preemption) → closed by
  root revalidation.
* **S-W3** NEW, introduced by copy-out alone: between splice and trim,
  page 0 holds COPIES of the high group; a stale-root reader could read
  them after the trim made them non-current → closed by root revalidation
  (the reader never descends through a non-current root). **Prose
  correction (2026-10-18, post-repro):** the value-STALENESS horn as
  originally worded ("a since-updated value") is UNREACHABLE under
  `write_mu_` — the splitting writer owns structural exclusivity from
  phase 1 through the trim, so no writer can update the new pages' copies
  while the superset still exists (and engine-level value updates never
  touch tree slots at all — version chains only). The real S-W3 content
  is: (i) post-trim, the stale-root descent MISSES the high group again
  (the superset is gone — S-W2's shape, which revalidation closes), and
  (ii) M4's root collapse turns the same stale-root descent into a
  freed-slot use-after-free candidate — revalidation's check-before-byte-
  read ordering is what makes collapse safe. The package conclusion is
  unchanged (copy-out and revalidation ship together or not at all); the
  argument for the second mechanism is miss-based and collapse-based, not
  staleness-based.
* **S-W4** cursor emission across phases → argued above; bound by S4/S5
  oracles plus a dedicated parked-cursor control at the
  `tree_leaf_trim` seam.

## 8. Mutants (exact-isolation, house norm)

* **MUT-REBUILD** (restore destructive page-0 rebuild in Phase 1):
  Controls A/B go RED alone; C stays green (revalidation independent).
* **MUT-NOREVALIDATE** (delete the root check): Control C goes RED alone;
  A/B stay green (copy-out independent). Exact co-flip isolation between
  the two mechanisms.
* **MUT-NOTRIM** (skip Phase 3): differential/size oracles go RED
  (duplicate chain entries after the next split touching page 0; page 0
  over budget) — pins the trim's necessity, anti-vacuity for Phase 3.

## 9. Disposition and sequencing (rules 8, 11, 14)

* **ID:** BT-3 (continues the BT-1/BT-2 series; cited by this document
  until the repro confirms or refutes — if refuted, the refuted probe
  stays in the suite per rule 8's "refuted probes" clause).
* **Sequencing:** remediation BEFORE M4 slices land merges (rule 11:
  Critical/High findings loop back before rewrites proceed). Root
  revalidation is a hard dependency of M4's root collapse; copy-out is a
  hard dependency of honest M4 acceptance (soak/DST/lincheck flakes would
  otherwise be misattributed to merge code).
* **Version:** ~~propose 0.28.2 patch~~ **SUPERSEDED (2026-10-18, at
  landing):** the v29 arc's observed pattern ships main-branch work under
  the static 0.28.1 constant with `## Unreleased` CHANGELOG entries (M1–M3
  all rode it); BT-3 follows — CHANGELOG entry + README safety-properties
  bullet + INVARIANTS row landed in the same commit as the fix (rule 3
  satisfied at the arc's granularity), and the version stamp + tag belong
  to the arc's release boundary (rule 2), not to this remediation.
* **Known-limitation entry:** none needed if fixed before merge; if the
  maintainer defers the fix, README §Known limitations MUST carry the
  window before M4 ships (rule 7: known-limitation entries before the fix
  exists).

## 10. Honest boundaries

* This document is a reading result. No build was run (maintainer
  instruction for this work session: do not build). The claim's truth
  value is set by §6's controls, not by this prose.
* The interior-level and root-split variants are argued from the same code
  shapes; the repro plan instruments the leaf level (highest traffic) and
  the root level (unbounded window). An interior-level control should join
  the battery if the leaf repro confirms.
* Whether any historical lincheck/soak flake was ever a BT-3 hit is
  unknowable from the record; none is claimed.

---

## 11. REPRO LOG (2026-10-18) — executed, verbatim

The maintainer lifted the no-build constraint for the repro. Vehicle: the
project's own recorded 1 GiB-container shape (`-O0 --param
ggc-min-expand=5`, g++ 12.2.0, 2 cores). Baseline BEFORE any edit: full
release suite at `1ab7c8c` — **ALL TESTS PASSED** (so every red below is
attributable). Then the two seams + the direct root-descend hook landed
(neutral instrumentation), and the bt3 controls A–D ran against the
**pre-fix** header:

```
stress seed: 24301 (0x5eed)
   bt3 A: splice-gap seam fired (anti-vacuity):  PASS
   bt3 A: point reads inside the splice gap see every acked key:  FAIL
      (pre_missing=0 missing_in_gap=32 first=k00368 post_missing=0)
   bt3 B: root-install seam fired on a height-1 tree (anti-vacuity):  PASS
   bt3 B: point reads inside the root-install gap see every acked key:  FAIL
      (depth1=1 missing_in_gap=92 first=k00092 post_missing=0)
   bt3 C: stale-root descent still finds the acked key (root revalidated under the latch):  FAIL
      (fired=1 found_in_stale_descent=0 found_after=1)
   bt3 D: a concurrent reader inside the commit's root-install window sees the acked key:  FAIL
      (timed_out=0 reader_found=0 reader_val=<absent>)
   bt3 D: post-window convergence:  PASS
exit=1
```

**The claim is CONFIRMED, deterministically.** Reading the numbers: A's
32 missing keys are exactly the high group of the split leaf (`k00368`
onward — the byte-balanced split point of a 432-key frontier leaf); B's
92 of 150+ are exactly the balanced upper half of the root-leaf split
(`k00092` onward); C shows the stale-root descent missing a key that is
found one call later (`found_after=1`); D is the engine-level
strict-serializability violation through the public API — a reader inside
a commit's root-install window gets `<absent>` for a key acked 140
commits earlier. Every `post_missing=0`: transient, convergent, silent —
exactly the §5 severity shape.

A fifth control, **E** (trim-shape anti-vacuity: exact `size()` and exact
chain scan after a root split — green pre-fix by construction, the
destructive rebuild never duplicates), was added with the fix to give
MUT-NOTRIM an oracle; §6's A/B/C plan became A–E.

**Post-fix** (copy-out split + deferred trim + root revalidation, §7):
bt3 **8/8 PASS, exit=0**. Mutant matrix on scratch reverts of the fixed
header, each rebuild + `CKV_ONLY_BT3=1` run — exact co-flip isolation:

| scratch mutant | red | green |
| --- | --- | --- |
| (pre-fix, seams only) | A, B, C, D | E (anti-vacuity, green by design) |
| MUT-REBUILD (restore destructive page-0 rebuild in phase 1) | A, B, D — at the IDENTICAL pre-fix counts (`32/k00368`, `92/k00092`) | C, E |
| MUT-NOREVALIDATE (delete the root check) | C alone | A, B, D, E |
| MUT-NOTRIM (`execute_pending_trims` no-op) | E alone (`size=277 model=185` — the superset double-counted; `scan_rows=184` — the chain bypasses the new page) | A, B, C, D |

Full suites on the fixed header (same vehicle): **stress suite ALL TESTS
PASSED** (bt3 battery included in-flow, rc=0, zero FAIL lines) and
**release suite ALL TESTS PASSED** (rc=0). Quiescent post-split states
are byte-identical to the pre-fix protocol's (same groups, same final
mut_epoch, same chain), which is why the 562-check battery — btree fuzz,
concurrent cursors, checkpoint/recovery, crash-fuzz, DST — needed no
expectation changes: no test flipped (rule 14 not triggered; the fix
changes window behavior only, and the windows had no pre-existing
oracles — that WAS the defect).

**Rule-12 note:** repro and fix were executed in the same work session
(the maintainer's standing instruction for this arc); the controls are
the permanent independent harness — they live in the suite, run on every
stress build, and any regression of either mechanism reddens its exact
co-flip set per the matrix above.

