# ChronoKV roadmap — v29 → v30: challenge the real databases

Written 2026-10-07 against `main @ e9eb787` (**0.28.1** — the Audit-2
remediation arc, shipped as PR #2, plus the arena-ledger vehicle split).
This document **rebaselines and supersedes** the 2026-09-27 plan (v29 → v30,
revision 3, written against `3161c77` / 0.28.0), which is preserved in full
as the appendix — including its own appendix (the superseded 2026-09-17
plan), per the disposition rule. Every item of the 2026-09-27 plan is
accounted for in the disposition table below; nothing was silently dropped.

**Why a rebase, not a revision 4.** The delta since revision 3 is not
citation polish — an entire independent audit round ran, found thirteen
defects including a genuine SSI soundness hole the first round had graded
"verified sound", and all of it shipped as 0.28.1 with fail-first tests.
That changes this plan's own gate status (M0 executed, closure pending),
its evidence base (two rounds, 34 findings, both remediated), and its
process rules (three new ones, each earned the hard way). By this
document's own adoption mechanics, that is a new plan with a disposition
table, not a revision block.

---

## What changed since the last plan was written

Nine days between plans (2026-09-27 → 2026-10-06). In order, with the
evidence in-tree or in the CI ledger:

1. **v29 M0 executed — the independent re-audit ran** (the "Audit-2" round,
   commissioned per M0's work statement: the 0.28.0 tag, the original audit
   and the remediation specification's status table handed over as the map).
   It found **13 defects**: TXN-1/2/3 (transaction & SSI), WAL-1/2/3/4
   (recovery & durability), BT-1/2 (tree exhaustion & standalone writer
   safety), EXTRA-1/2 (latch window, uint16 slab-offset wrap), API-1/3
   (single-header ODR, observer-move lifetime). The most consequential:
   **TXN-1 — the phantom tracker recorded only existence transitions, so a
   value UPDATE inside a concurrently scanned range did not conflict the
   scanner.** A standalone PoC on 0.28.0 committed the resulting
   value-based write skew. The first audit's §7 had graded phantom
   detection "(verified sound)". Both statements were true — of their
   round. Process rule 11 exists because of this sentence.
2. **0.28.1 shipped the full remediation** (PR #2, `5cd74b5`, merged
   `b6fb181`): every finding fixed fail-first — each PoC was reproduced
   against pre-fix 0.28.0 first (TXN-2: a 1 MiB pool exhausted at absent-
   read txn 16,554; EXTRA-2: a 2,500-byte update corrupted 4 neighboring
   keys; API-3: observers fired 0 times after a Database move) — with a
   15-check Audit-2 battery appended to `CKV_ONLY_REMEDIATION`, one
   deliberate semantic change shipped as a *flipped, renamed, cited* test
   (v17 guard test 4: update-in-scanned-range now CONFLICTS), and one
   soundness refinement over the audit's own fix (no-op deletes of absent
   keys stay unrecorded — a tombstone above every concurrent snapshot
   cannot change any visible scan output, so recording it is pure false
   conflict; the v17 no-op-delete contract stays green by soundness, not
   by accommodation).
3. **Cross-verification caught the fixer's gap** (process rule 12): the
   reviewing harness probed BT-1's in-place-update exemption and found it
   missing from `put_with_old` while the changelog claimed it generally.
   Fixed fail-first (extended exhaustion test verified throwing bad_alloc
   on `put_with_old` pre-fix, green post-fix). The fixer did not have the
   last word on the fixer's patch — and that is the only reason the claim
   and the code agree today.
4. **v29 CI hardening landed in the same arc**: the `lint` job (-Werror
   syntax gates over header+suite hooks-on/off in 40 s; a **-Wconversion
   count ratchet** — baseline 33 — making the uint16-narrowing class behind
   CKV-001/002 *and* EXTRA-2 a permanently gated property; header hygiene
   now GATING: double-include + two-TU link, both green since API-1);
   `build-shared` (the one big TU compiles once per run and rides to
   dst/crashfuzz as artifacts — crashfuzz 4m42s → 3m13s); four
   **experimental sanitizer legs** (asan/tsan × io_uring-disabled — the
   only config where the WAL write-path fault legs execute — and clang).
   All four experimental legs green three runs straight (#64, #65, #66):
   the promotion condition ("green once → delete the flag and it gates")
   is met; flags deleted in `0027fe0` (2026-10-08) — all 12 legs now
   gate `ci-passed`.
5. **The arena ledger vehicle was repaired** (`e9eb787`): two consecutive
   nightlies (#62 on `8863bc8`, #65 on `b6fb181` — identical deaths on both
   sides of the merge, so it was the vehicle's budget, not 0.28.1's delta)
   proved the single 60-minute job never fit: the ChronoKV group+async legs
   measured **44m01s**, leaving the five baselines a ≥15m31s lower bound
   before the cap killed them mid-leg, every night. The ledger never
   completed a pass, so M1's "one green week" acceptance clock could not
   start. Now two parallel jobs (`arena-chronokv` 90 min from measurement;
   `arena-baselines` 180 min generous-on-purpose with the rule-13 tighten-
   after-measuring follow-up written into the job comment). Geometry
   untouched — geometry changes are ledger-format changes (rule 9).
6. **The cost of correctness, measured** (rule 9; sandbox floors — 2-core
   Xeon 2.5 GHz, 1 GiB, 4.19 kernel io_uring-blocked, overlayfs, -O1,
   group durability, 200k keys / 100 B / 4 threads / seed 42): 0.28.1 costs
   **~10–13% on write-heavy mixes** (fillseq 30,077→26,056; fillrandom
   39,530→35,067; ycsb_a ~40k→~36k ops/s both directions), reads unchanged
   (130,129→131,353), ycsb_a RSS +10.7% (210.9→233.5 MiB — the tracker now
   holds every written key until pruned). Accepted: SSI soundness and
   exhaustion-safety outrank the delta; the ledger records it, and M5's
   writer-thread rewrite is where the throughput gets bought back.
7. **The build-memory wall rose again**: the post-Audit-2 TU (28,016 lines:
   `chronokv.hpp` 13,445 + `main.cpp` 14,571) OOMs a 1 GiB container at
   `-O1`, where 0.28.0 fit; ASan does not build there at any -O level.
   `-O0` builds in ~25 s and the full suite (495 checks) passes. This is
   the third arc in a row that raised the wall — v29 M2 gains a
   pressure-relief item (5), unlocked by API-1: the header is now ODR-safe,
   so the *test suite* can split into linked TUs without touching the
   engine.

---

## The challenge, stated honestly

ChronoKV is a zero-dependency, single-header, single-process embedded
key-value store. That niche has incumbents, and they are not soft targets:

| | SQLite | LMDB | RocksDB | ChronoKV today (0.28.1) |
| --- | --- | --- | --- | --- |
| Deployment scale | billions | hundreds of millions | industry standard server-side | hobby/evaluation |
| Correctness proof | TH3 + OSS-Fuzz + astronomical field exposure | long field history, simple core | huge field history, battle-tested | **two independent adversarial rounds (21 + 13 findings), every fix fail-first; machine-checked gates (4-config sanitizer matrix, DST, lincheck, crash-fuzz, lint ratchets)** — deep, young, and now *iterated* |
| Transactions | WAL mode: snapshot isolation; writers serialized | MVCC, single writer at a time | write-batch atomicity; SI via pessimistic/optimistic `TransactionDB` — not serializable | **SSI — serializable, multi-writer; value-antidependencies inside scanned ranges tracked since 0.28.1 (TXN-1)** |
| Durability I/O | pwrite + fsync (portable) | mmap + msync | pwrite + fsync (pluggable env; io_uring on read paths) | **default-path io_uring write→fdatasync chains** — kernel deadline (`LINK_TIMEOUT`), registered-buffer/file ladder, 3-strike pwrite fallback; segment-creation dir-fsync and rotation-path dir-fsync correctness since 0.28.1 (WAL-2/WAL-3) |
| Recoverable to a point in time | no (backup only) | no | yes (WAL + backups) | **yes — PITR is a first-class API**; recovery loops bounded and interior corruption loud (WAL-1/WAL-4) |
| Memory at scale | page cache, transparent | virtual, sparse | block cache + memtables, tuned | **256 MiB monotonic pool — still the credibility gap**; exhaustion is now pre-mutation and loud (BT-1) but nothing is reclaimed yet |
| Form factor | amalgamation, ubiquitous | small C lib | large library suite | **one header, zero dependencies — double-include and two-TU-link CI-gated (API-1)** |

Read that table twice and the strategy writes itself. ChronoKV will not
out-test SQLite's field exposure in a decade, will not out-feature RocksDB's
tuning surface in two, and should not try. What it can do — what this arc
does — is close the one front where today it is not credible (memory and
steady-state behavior at real scale), press the two fronts where it is
already ahead of every incumbent in its class (transactional semantics and
durability I/O), and convert the front nobody else publishes on (adversarial
verification) from an event into a permanent, machine-checked property.

Four fronts, four claims — carried from the 2026-09-27 plan, claims 1 and 2
strengthened by what Audit-2 proved:

1. **Trust, by construction — and by iteration.** SQLite proves correctness
   by volume of testing and exposure. ChronoKV proves it by *construction*:
   every canonical invariant machine-checked in CI, every audit published
   in-tree, every fix born fail-first, power-loss semantics tested against
   a block layer that actually lies. Audit-2 added the second half of the
   claim: a construction can pass one independent round and still hide a
   soundness hole (TXN-1 under a "verified sound" grade), so trust here
   means *surviving repeated rounds* — the 1.0.0 gate is a round, not a
   ceremony, and it now has a precedent with teeth.
2. **Serializable by default.** Every incumbent in the class stops short of
   serializable: SQLite's WAL mode and LMDB serialize their writers, and
   RocksDB's `TransactionDB` (pessimistic or optimistic) tops out at
   snapshot isolation — SI, not SSI; write skew is not prevented. ChronoKV
   runs concurrent serializable (SSI) transactions — since 0.28.1 including
   the value-antidependency half (an update inside a scanned range
   conflicts the scanner) — and by v29 M2 proves them with a write-skew
   checker in CI. That checker is no longer optional polish: TXN-1 was
   found by an audit, not by lincheck, which is the remediation spec's
   predicted SSI-blindness demonstrated in production.
3. **fsync-per-commit at group-commit prices.** io_uring is not unique in
   the class — RocksDB's POSIX env uses it on read paths — but no embedded
   KV makes it the **default** WAL durability path. ChronoKV's is a
   hard-linked write→fdatasync chain with a kernel-enforced deadline
   (`LINK_TIMEOUT`), a registered-buffer/file ladder, and a
   runtime-degrading offset-pinned pwrite fallback (0.28.0: deep
   availability probe + 3-strike CQE degradation). With the v29
   writer-thread rewrite, chained write+fsync plus adaptive group commit is
   the throughput story: durable writes that don't serialize the
   committers — and reclaim the ~10% the soundness fixes cost.
4. **Real scale, bounded memory.** 100 million keys in bounded *memory* at
   steady state — tree pool, version heap and reader slack, all three
   budgeted: `page_pool_bytes` alone bounds only the B+ tree, while values
   live in `Version` chains on the regular heap — with recovery in tens of
   seconds and numbers published — or the other three claims don't matter,
   because nobody who needs a *database* can deploy a monotonic memory
   ceiling.

What this roadmap does **not** claim, equally on purpose: no SQL, no
multi-process, no replication, no cross-platform, no third-party
compression, no bindings. The deferral table survives this rebase verbatim;
"single header, zero dependencies, verifiably correct" is the product. The
challenge is to make that product *competitive*, not to make it *bigger*.

---

## Where the project actually stands (0.28.1)

Two adversarial rounds are closed and remediated: the 2026-09-23 audit's
21 findings (CKV-001…021) plus platform defects F1/F2, shipped as 0.28.0;
and Audit-2's 13 findings over the 0.28.0 tag, shipped as 0.28.1 with the
15-check battery, the flipped v17 guard test, and the CI hardening. The
full suite stands at 495 checks green (sandbox, -O0), including crash-fuzz
(624 iterations, 316 armed crash points reached, 0 violations, every point
reachable), DST (fork-isolated, seeded), and lincheck with all 8 mutation
detectors proving the checker non-vacuous.

Worth recording because it shapes the next arc — the Audit-2 implementer,
like the v28 one, did not blindly transcribe the specification:

- TXN-1's fix as specified would have made **no-op deletes of absent keys**
  conflict with every overlapping scanner. The shipped version records
  every write *except* the provably-unobservable one (`!old_exists &&
  !new_exists` — the tombstone's cts is above every concurrent snapshot),
  keeping the v17 contract green by soundness instead of flipping two more
  tests to accept false conflicts.
- TXN-2 replaced the audit's "materialize less" with "materialize
  nothing": absent read-set keys become `[k,k]` point range reads through
  the phantom tracker, which TXN-1's completeness is exactly what makes
  sufficient — the two fixes are load-bearing for each other, and the
  battery tests the composition (check-then-insert still conflicts).
- WAL-3 was found by reading `fsync_dir`'s actual semantics against its
  call sites (it syncs the *parent* of the path it is given): both
  rotation-path calls passed the WAL directory itself and had therefore
  been syncing the wrong directory — the MANIFEST rename was never made
  durable on those paths. The audit-1 round walked past this; the v25.7
  manifest-brick history made the second round look.

What 0.28.1 did *not* do, and v29 inherits:

- **M0's closure is pending.** The Audit-2 *report itself is not in-tree*
  (rule 8 violation-in-waiting: the finding IDs TXN-1…API-3 are cited by
  the battery's comments but the report they came from lives outside the
  repository), the **delta confirmation round** over 0.28.1 has not run,
  and **tag `v0.28.1` does not exist** (rule 2: only `v0.28.0` is tagged).
  The rewrite milestones (M3+) stay gated until M0 closes — the gate did
  its job once already this arc; it does not get waved through now.
- **The suite still carries the vacuous tests** the first audit enumerated
  beyond the arc's scope (v17 GC-boundedness, v18 LSN-contiguity,
  `m2_phase1` size-rotation admission, the 30 s soak's missing oracle).
  v29 M2.
- **The structural debt is untouched except where Audit-2 hardened its
  edges**: the monotonic page pool remains (exhaustion is pre-mutation and
  loud via BT-1's headroom check, feeding the same D4 latch — one health
  model, reinforced — but `PagePool::free()` still has no *engine* callers
  and `is_hazardous()` still has none at all); no merge/rebalance; GC
  sweeps at O(N²/256); the leader-election handshake in the WAL committer
  (M5 deletes it); `nm_` still serializes every new-key insert. The TU
  grew to 28,016 lines and now OOMs 1 GiB at `-O1` where 0.28.0 fit; the
  README's and Makefile's "~17k" counts remain stale (v30 M2 fixes all
  three). API-1 is the one structural item that *closed*: the header is
  genuinely multi-TU-safe and CI-gated — which is also what makes the
  test-suite split in M2 item 5 mechanical rather than brave.
- **The README still publishes exactly one performance number** (async at
  0.256× sync — the indictment stands). The arena now exists with rule-9
  methodology headers and repaired nightly vehicle; the first *completed*
  ledger night starts the green-week clock, and the README performance
  section lands with M1's acceptance, not before.

### Progress

| Milestone | Status | Shipped as |
| --- | --- | --- |
| Adversarial audit round 1 (21 findings, verdict D) | **DONE** | 2026-09-23 report, in-tree |
| Remediation specification + waves 1–5 | **DONE** | 0.28.0, tag `v0.28.0` |
| v29 M0 deliverable zero (audit docs in-tree) | **DONE** | `docs/audits/` (2026-09-23, 2026-09-24) |
| v29 M0 re-audit commission (Audit-2 over 0.28.0) | **EXECUTED** — 13 findings, all remediated fail-first | PR #2 (`5cd74b5`, merged `b6fb181`) = 0.28.1 |
| v29 M0 closure | **DONE 2026-10-08** — reconstructed report + tag `v0.28.1` + delta confirmation round, verdict **B+ (clean)**: 20/20 probes, one Low doc-truth finding fixed in the closure commit; **M3+ gate lifted** | `docs/audits/2026-10-delta-confirmation-0.28.1.md` (+ probes) |
| v29 M1 — benchmark arena | **STARTED** — steps 1–3 COMPLETE (D/E/F adapters + version pins with sha256/fetch recipe); step-4 vehicle: split (#68–#75 green) then RE-MERGED to one same-hardware job after #75's headers caught the two-runner CPU split — green-week clock restarts on the merged vehicle (rule 9 over sunk nights); soak vehicle NAMED (rolling nightly chain). Remaining: green week (7 nights), noise bands + gate enablement, ledger-promotion decision, CI build cache | `bench/`, `bench/third_party/versions.txt`, ci.yml `arena` |
| v29 CI hardening (adjacent, unplanned-in-9/27) | **SHIPPED + PROMOTED** — lint/ratchet/hygiene gates, build-shared; the 4 experimental sanitizer legs went green ×3 (#64/#65/#66) → flags deleted, all 12 legs gating | `5cd74b5`; promotion `0027fe0` |
| v29 M2 — catcher hardening | **STARTED** — item 1 (write-skew checker, 2026-10-08, live pre-fix-flag/post-fix-pass proof) + item 5 (test-suite TU split, 2026-10-10: 1 GiB release/stress `-O1` bare, ASan `-O0` possible, 503/0 full suite; found+fixed the `dst::my_tid` non-inline thread_local, now stress-gated) shipped; items 2–4 pending | `check_write_skew`; `tests/`, Makefile template |
| v29 M3–M7 — the overhaul | not started — M0 CLOSED 2026-10-08; order still governed by constraints 2–3 (arena green week, catchers first) | — |
| v30 M0–M5 — the challenge, 1.0.0 | not started | — |

---

## Reading rules (kept verbatim, one addition)

The 2026-09-27 plan's reading rules survive unchanged — no speculative
work; fail-first precision (defect-asserting checks verified failing,
controls and contract tests passing by design); canonical IDs for new
invariants (R/I/D/E/P/B/T, P1 pending with M4); documented migration rule
before any on-disk format change (M6 carries the arc's first); measured
claims only — numbers are budgets, not trophies. Sizes remain T-shirt
guesses (**S** ≈ days, **M** ≈ 1–2 weeks, **L** ≈ 3–6 weeks, **XL** ≈ a
quarter). The pace assumption survives contact with reality: a full
audit → remediate → cross-verify → merge round took **nine days**
(2026-09-27 → 2026-10-06), faster than the five-week arc it was planned
against; if the pace halves, the plan re-sequences milestones, it does not
renegotiate gates. Citations prefer function-scope anchors and carry
"as of" tags, because this repo keeps proving line numbers rot.

**NEW — verdicts are round-scoped.** A "verified sound" grade from any
audit round means *no defect found by that round's reading*. TXN-1 lived
for at least one full round under exactly that phrase. No section of any
document in this repo may cite a soundness verdict without its round.

### Load-bearing constraints (carried, with status)

1. **The re-audit (v29 M0) lands before any rewrite.** STATUS:
   **SATISFIED — M0 closed 2026-10-08.** Audit-2 ran, found
   Critical/High-class defects, the loop-back ran exactly as designed
   (0.28.1 shipped before any M3+ line was written), and the delta
   confirmation round verified the closures at depth (verdict B+, probes
   committed). The foundation under the rewrite milestones is now
   independently verified, not self-verified.
2. **The arena (v29 M1) lands before the rewrites it measures.** STATUS:
   in progress — vehicle repaired, first completed nightly pending.
   *Measure before predict* has now also been applied to the vehicle
   itself (rule 13 was earned by two dead nightlies).
3. **Catcher hardening (v29 M2) lands before the rewrites it guards.**
   Unchanged — and Audit-2 upgraded item 1 from "spec says the checker is
   blind" to "the blindness shipped a real anomaly past round one".
4. **The 1.0.0 gate is not negotiable.** Unchanged, with a track record:
   the second independent round found a soundness hole in a
   "remediation-complete" engine nine days after the first declared
   closure. The gate is the reason, not the obstacle.
5. **Composition over duration.** Unchanged — the Audit-2 battery composed
   into the existing remediation gate; the arena split reused the existing
   ledger format; the lint ratchet reused the existing WARN set.

---

# v29 — The overhaul (0.29.0)

Theme unchanged: close the credibility gap. Every structural weakness a
real database would laugh at — the monotonic pool, the missing merge, the
leader handshake, the O(N²) GC, the invisible benchmarks — gets closed or
measured, in an order where each rewrite is guarded by catchers written
before it and measured by an arena built before it. Three of the four
subsystems are touched (page lifecycle, delete path, WAL committer). The
2026-09-27 plan's "verified-sound list" is preserved but read under the
new rule: those grades are round-1-scoped; TXN-1 revised one of them
(phantom detection) in round 2, and the delta confirmation round is what
re-grades the rest.

## M0 — Independent re-audit  **[S] [GATE] — CLOSED 2026-10-08**

**What happened.** Deliverable zero shipped (both round-1 documents are
in-tree under `docs/audits/`). The re-audit was commissioned per this
milestone's own work statement and ran over the 0.28.0 tag: 13 findings
(TXN-1/2/3, WAL-1/2/3/4, BT-1/2, EXTRA-1/2, API-1/3), each reproduced
fail-first, each remediated with a battery test, shipped as 0.28.1 in
nine days including cross-verification of the fixes by a second
independent harness (which caught the `put_with_old` parity gap — rule 12
earned).

**Acceptance, honestly stated.** The original bar — "zero Critical/High
findings, verdict B or better" — was **not met at audit time**: TXN-1
(SSI soundness), EXTRA-2 (memory corruption) and WAL-3 (durability) are
Critical/High class. The gate's loop-back arm executed instead, which is
also what the milestone text prescribes. M0 therefore closes only when:

1. **The Audit-2 report is committed under `docs/audits/`** (rule 8: the
   finding IDs are cited from the battery's comments; the report they came
   from must be checkable from the tree — the round-1 lesson, not
   repeated). **DONE 2026-10-08 — as a labeled reconstruction**
   (`docs/audits/2026-10-audit-2-reconstructed.md`): the original audit
   session was not preserved, so the committed record consolidates the
   in-tree remediation artifacts (fix rationale comments, the battery, the
   0.28.1 changelog) and the independent cross-verification log, with
   severities assigned-in-reconstruction and an explicit statement of what
   is unrecoverable. Nothing is invented to fill the gap.
2. **A delta confirmation round runs over 0.28.1** — verify all 13
   closures at depth (not checklist), and hunt specifically in the classes
   the *fixes* could have introduced: the phantom tracker's new
   completeness (memory growth, pruning liveness under long readers), the
   `[k,k]` point-range path (TXN-2) against the range-read machinery,
   WAL-1's offset-scan classifier against adversarial corruption, BT-1's
   headroom arithmetic against the split planner's real worst case.
   Verdict B or better, zero new Critical/High. **DONE 2026-10-08 —
   verdict B+ (clean)**: 20 mechanical probes across the four hunt
   classes all pass (committed, re-runnable:
   `docs/audits/2026-10-delta-confirmation-probes.cpp`), zero new
   Critical/High/Medium, one Low doc-truth finding (four comments still
   describing the pre-TXN-1 "existence transitions only" contract) fixed
   in the closure commit. Report:
   `docs/audits/2026-10-delta-confirmation-0.28.1.md` — it also records
   the BT-1 bound re-derivation against the real planner, two behavioral
   notes for M6/v30-M4 (GC-off tracker scale; long-reader validation
   cost), and the environmental classification of this sandbox's
   full-suite thread-EAGAIN anomaly (three independent
   non-reproductions; CI is the authoritative vehicle).
3. **Tag `v0.28.1`** exists (rule 2 — the version, the changelog and the
   tag ship together or the release identity is prose). **DONE** — the tag
   rides the reconstruction commit, completing the arc's in-tree record
   (the same convention as `v0.28.0` tagging the arc-completion commit
   `3161c77` rather than the release commit).

**Status: CLOSED 2026-10-08** — all three items done (reconstructed
report, confirmation round at verdict B+, tag `v0.28.1`). **The M3+
rewrite gate is lifted**; sequencing behind M1's green week and M2's
catchers (constraints 2–3) still governs the order of work.

Until then, M3+ stays gated. M1/M2 proceed.

## M1 — The benchmark arena  **[M] [MEASURE FIRST] — STARTED**

**Status.** Steps 1–3 shipped (engine-side workloads incl. the `nm_`
isolation fills; YCSB A–F over zipfian(0.99)-CDF with documented
deviations; SQLite/LMDB/RocksDB adapters under identical methodology with
the three fairness fixes — LMDB dbi priming, RocksDB parent-dir creation,
the SQLite WAL read-snapshot trap). Step 4's vehicle shipped, died twice
on its own budget (#62, #65 — identically, across the merge), and was
split into parallel jobs (`e9eb787`): `arena-chronokv` (90 min cap from a
measured 44m01s) and `arena-baselines` (180 min cap, generous-on-purpose,
with the tighten-after-measuring follow-up in the job comment).

**Remaining, in order.**
1. **First completed nightly — DONE, night 1/7 banked** (run #68,
   2026-10-08, on `b5a7eee`): `arena-chronokv` 31m55s and
   `arena-baselines` 15m33s, both artifacts uploaded
   (`arena-ledger-{chronokv,baselines}-37764953712`), whole nightly
   green. The measured 15m33s proves the pre-split deaths were budget
   allocation, not baseline slowness (the killed lower bound was
   ≥15.5 min — nearly the entire real cost). Caps right-sized in the
   same breath per rule 13. Nights 2–3 (#74, #75) also completed green.
   **THEN the split's own fairness defect surfaced (2026-10-10):** #75's
   methodology headers record the two jobs on DIFFERENT CPUs (baselines
   AMD EPYC 7763, ChronoKV Intel Xeon 6973P-C) — parallel jobs mean
   parallel runners, and the fairness protocol's "same harness, same
   HARDWARE" is the ledger's entire product. Vehicle re-merged to ONE
   job (same machine by construction), timeout 120 min from the measured
   sequential need (#65: ~60 min killed-at-budget; split legs sum ~48
   min; ≥2x headroom), artifact name reverted to `arena-ledger-<run_id>`.
   **The green-week clock RESTARTS at the first completed night on the
   re-merged vehicle**: nights 1–3 ran cross-machine — their per-engine
   rows remain valid for same-engine trend (every row carries its own
   hardware header) but their cross-engine comparisons are not
   same-hardware ledger material. Rule 9 over sunk nights; the headers
   catching this is rule 9 working as designed.
2. **D/E/F baseline adapters — DONE (2026-10-10, step 3 complete).**
   `Engine::scan` primitive added (materializes rows — the arena's
   `range_scan` returns pairs, so a count-only baseline scan would do
   strictly less work; the SQLite adapter resets the scan statement
   before returning, the WAL read-snapshot trap the step-3 validation
   already paid for once, now live in E's scan+insert alternation on the
   same connection). `w_ycsb_def` mirrors the arena's semantics
   operation-for-operation: D's read-latest is the geometric(0.001) tail
   over the shared insert counter, E scans zipfian starts with len
   1+rng()%100, F records combined read+write latency and rewrites the
   value it read. Validated on sqlite-full/sqlite-normal/lmdb (identical
   op-mix counts across engines at equal seeds — the fairness property;
   `rows/scans=51` matches the arena's #75 E rows exactly; zero
   FAILED-OPS); the rocksdb leg is stub-syntax-checked locally and gets
   its first full compile in CI (distro 8.9.1).
3. **Version pinning — DONE (recipe shape, 2026-10-10).**
   `bench/third_party/versions.txt`: sqlite 3.45.1, lmdb 0.9.31,
   rocksdb 8.9.1 — the exact versions the CI ledger builds against,
   cross-checked against run #75's methodology headers, with sha256 of
   each release artifact (fetched over TLS from canonical origins) and
   the GitHub-archive re-record rule; `bench/third_party/fetch.sh`
   downloads + verifies every pin (mismatch is fatal, never
   blind-accepted). Distro packages remain the calibration-stage CI
   install path (per-run versions are recorded in the headers and
   cross-checkable against the pins); in-tree vendoring stays deferred
   per the policy README — recipes are its sanctioned alternative; CI
   build caching (policy item 3) lands with gate enablement.
4. Noise-band calibration from the green week: per-metric bands,
   N-iteration medians, explicit re-run policy — then **gate enablement**
   (add `arena` to `ci-passed` needs + commit the bands). The first
   recorded regression-vs-baseline event is already known: 0.28.1's
   ~10–13% write-mix delta (sandbox floors; the ledger re-measures it on
   fixed-spec runners). The ledger's first real comparison rows (#75)
   also give M5 its honest starting picture: ChronoKV group-durability
   fills/updates sit at 2.3k–5.4k ops/s against power-loss-class
   baselines at 3.9k–9.9k (sqlite-full fillseq 8.3k, rocksdb-sync
   ycsb_a.update 9.9k) with reads competitive (ycsb_c 943k vs rocksdb
   915k, sqlite 436k; lmdb's mmap owns readrandom at 4.0M) — the
   writer-thread rewrite's before-column, exactly as claim 3 predicted.
5. The ledger-promotion decision (committed `bench/results/` needs
   contents:write or a bot commit) — and the **soak vehicle is NAMED
   (2026-10-10): the rolling nightly chain with persisted state +
   aggregate verdict.** Rationale: hosted jobs die at 6 h, so a literal
   24 h/7-day single job cannot exist; the project has no self-hosted
   machine and a milestone must not depend on acquiring one; the ledger
   artifact machinery already ships (TSV + methodology headers ARE the
   persisted state) and the noise-band calibration of item 4 IS the
   aggregate-verdict mechanism. Shape: each nightly runs a deterministic
   seeded segment; the 24 h soak = chained segments, the 7-day
   steady-state variant (P1's acceptance) = a 7-night chain; segments
   rebuild from pinned geometry, so runner variance lands in the bands,
   not in the state. If a self-hosted runner is ever acquired, the
   vehicle upgrades to a true continuous-db soak before v30 M1 — the
   acceptances (P1 under the 7-day soak, p99 drift beyond noise fails)
   are vehicle-independent.

**Acceptance** (unchanged): `make arena` produces the six-table
comparison; the README performance section exists and links the
methodology; the nightly ledger shows one full green week. No engine
changes in this milestone.

## M2 — Catcher hardening  **[M] — STARTED (items 1 + 5 shipped 2026-10-08/10)**

**Anchor** (upgraded by Audit-2): remediation spec §11 predicted lincheck's
SSI blindness; TXN-1 *demonstrated* it — a real write-skew-class anomaly
found by an audit round because no machine checker covered the class. Plus
the remaining audit-enumerated vacuous tests, plus the build-memory wall.

**Work.**
1. lincheck gains the SSI anti-dependency (write-skew) checker and a
   synthetic write-skew anomaly battery — the checker that proves claim 2,
   now with a production miss to validate against: the TXN-1 PoC history,
   replayed through the checker, must flag the pre-fix engine and pass the
   post-fix one. **DONE 2026-10-08**: `lincheck::check_write_skew` — the
   multiversion dependency graph (ww version chains, wr observed-version
   edges, and the rw ANTI-dependency edges from point reads AND scans that
   SI-level checking cannot see) with cycle = anomaly; scope-disciplined
   against check_cts_order (snapshot-soundness stays there). Ships with a
   4-anomaly + 3-control synthetic battery (W2 pins the TXN-1 PoC shape
   verbatim; C3 pins the post-fix aborted shape), wiring into BOTH engine
   workload aggregates (list-append + mixed-API, every seed), and an
   engine-shaped mutation (a fabricated missed-anti-dependency pair on
   real recorded txns). Live acceptance demonstrated: the same driver run
   against tag `v0.28.0` records the anomaly committing and the checker
   flags the exact cycle (T2 -[rw 'x' missed write@4]-> T1 -[rw scan[k,k]
   missed 'k'@3]-> T2); post-fix the engine refuses T1 and the committed
   history is CLEAN. Suite grows 495 → 503 checks. CI-hardened in
   follow-up (run #72 taught it): the engine-shaped mutation's first
   pair predicate starved on faster runners where SSI refused every
   committed scanning writer — it now searches pairs exhaustively and
   carries a non-vacuous fabricated-scan fallback over two REAL
   committed writers, so the mutation cannot silently skip while the
   workload's nonvacuity gate holds.
2. The DST plan grammar grows merge/rebalance/cascade and
   reclamation-retirement scenarios *before that code exists* — scenarios
   written against the design; the implementation must survive them.
3. De-vacuate the remainder: v17 GC-boundedness, v18 LSN-contiguity,
   `m2_phase1` size-rotation, the 30 s soak's oracle. Each upgraded test
   proven once by temporary reversion in a scratch build.
4. The crash-fuzz plan grammar composes GC sweeps, checkpoint, rotation
   and merge windows — and now also the Audit-2 surfaces: segment-creation
   dir-fsync windows (WAL-2), the recovery offset-scan classifier
   (WAL-1), reservation-window throws under batch pressure (EXTRA-1).
5. **NEW — TU pressure relief (test-side split).** Anchor: the 28,016-line
   TU OOMs 1 GiB at `-O1` where 0.28.0 fit, and ASan doesn't build there
   at all; every arc raises the wall; the experimental-leg policy (sanitizer
   coverage for the write-path fault legs) depends on builds small machines
   can make. API-1 (0.28.1) removed the blocker: the header is ODR-safe,
   so `main.cpp`'s batteries split into linked TUs each including the
   header — no engine change, mechanical by construction, `make -j`
   parallel. The engine-side split + amalgamated release artifact stays
   v30 M2; this item is the pressure relief until then. Acceptance: a
   1 GiB container builds the full suite (release) at `-O1` and the
   sanitizer matrix at `-O0`, all 495 checks green, CI wall-time not
   worse. **DONE 2026-10-10** — `main.cpp` (14,921 → 10,587 lines) plus
   `tests/tests_{lincheck,remediation,dst}.cpp` + `tests/test_decls.hpp`;
   content byte-identical in the move except the three entry points lose
   `static`. Measured on the 1 GiB confirmation sandbox: release `-O1`
   bare (68 s — the monolith was OOM-killed here), stress `-O1` bare,
   ASan+UBSan `-O0` with `--param ggc-min-expand=5` (battery suite green
   under it — first sanitizer run this box ever had; TSan/UBSan matrix
   legs ride CI), full suite **503/0 bare at `-O1`** (and the
   environmental `-O0` abort did not reproduce at `-O1`), CI builds gain
   `-j4`. **The split immediately paid for itself**: the 4-TU stress
   link failed on `dst::my_tid` — a namespace-scope `thread_local` in the
   header WITHOUT `inline` (per-TU definitions; the monolith hid it, and
   the hygiene gate missed it because it never compiled STRESS). Fixed
   (`inline thread_local` — single identity is also the semantically
   correct baton state) and the class is now gated: `lint-header-2tu`
   gained a `-DCHRONOKV_STRESS` 2-TU link leg. API-1's "true single
   header" claim is now true under STRESS too.

**Acceptance** (carried): a synthetic write-skew history fails the new
checker; every de-vacuated test fails against a reintroduction of its
defect class; the merge/reclaim DST scenarios run green against the
current (merge-free) tree; plus item 5's build acceptance.

## M3 — Page reclamation  **[L]**  *(M0 gate lifted 2026-10-08)*

Carried unchanged from the 2026-09-27 plan (appendix §M3): wire
`PagePool::free()`, give the hazard-pointer machinery its consumers,
retire pages through the epoch reclaimer or generation-tagged ids with the
ABA (HP4) decision made **before** implementation; CKV-008's mutation epoch
is the stale-descent net for live pages, not a reuse-safety mechanism.
Two updates from 0.28.1: pool exhaustion now surfaces *pre-mutation*
(BT-1's headroom check throws before any page is touched, and the engine's
catch latches D4) — the "one health model, not two" requirement is
already half-built; and TXN-2 removed the negative-lookup leak that would
otherwise have polluted the reclamation numbers (absent reads no longer
materialize index entries at all). `stats()` gains pool utilization and
high-water mark; `health()` degrades at 80%, fails at 95%.

**Acceptance** (unchanged): delete-then-reinsert workload's high-water mark
stabilizes instead of ratcheting; TSan-clean; no use-after-free under ASan
with reuse enabled; arena delete-churn table shows bounded memory.

## M4 — Leaf merge and rebalance  **[XL]**  *(M0 gate lifted 2026-10-08)*

Carried unchanged (appendix §M4): feasible on 0.28.0's plan-before-mutate
byte-aware split planner — merges are its mirror image, sharing the
capacity model (a model BT-1's headroom arithmetic now also leans on; M3's
reuse work and BT-1's `5*(height+1)+1` bound get re-derived together, with
the split planner as the single source of capacity truth). Crabbing with
exclusive latches on the delete path; sibling borrow vs. merge; interior
key deletion; root collapse; the fence-recheck machinery exists.

**New invariant (unchanged).**
> **P1** — under a sustained delete-heavy workload the page pool reaches
> steady state with bounded utilization, with no use-after-free and no ABA.

**Acceptance** (unchanged): P1 under a 7-day soak (ASan/TSan interleaved)
on the soak vehicle named in M1; concurrent cursor + concurrent merge
clean under TSan *and* M2's DST scenarios; differential-vs-`std::map`
passes with merges active; the arena's delete-churn and space-amplification
tables move.

## M5 — WAL writer thread  **[L]**  *(M0 gate lifted 2026-10-08)*

Carried unchanged (appendix §M5): the arena baseline is the "before"
column — which now includes 0.28.1's ~10% write-path delta as part of the
honest starting point, not a regression to hide. Dedicated writer thread +
bounded MPSC queue; **deletes** `leader_active_`, the shared `cur_batch_`
mutation, the leader/follower handshake and the `pending_` FIFO — the code
stops existing; it does not get rewritten more carefully. Unlocks
`SINGLE_ISSUER`, `DEFER_TASKRUN`, `SQPOLL` worth its kernel thread, queue
depth > 1. Adaptive group-commit linger. Old path one release behind a
compile flag, then deleted, with the full DST + crash-fuzz + fault matrix
re-run first. One 0.28.1 interaction to respect: EXTRA-1's fail-stop latch
now fires *inside* `group_append` under `batch_mu_` — the writer-thread
design inherits that latch point (the reservation window moves into the
queue producer; the latch semantics — no committer past a burned cts —
must survive the move verbatim).

**Acceptance** (unchanged, plus one): arena "after" tables show no
regression at any concurrency and a measurable win at 8+ committers;
C1/H3-class reversion probes fail loudly; crash-fuzz green; dead code
gone; the triple identity **cts-order == WAL-order == phantom-order**
re-proven by the full matrix — and since TXN-1, phantom-order carries
*every* write, so the identity has strictly more content to preserve.

## M6 — Space and steady state  **[M]**

Carried unchanged (appendix §M6): persistent incremental GC cursor
(O(N/256) sweeps); slab-compaction reuse against a published
space-amplification budget; WAL delta-encoding only if it pays;
**multi-version PITR deltas** — the arc's first deliberate on-disk format
change, migration rule before implementation, GC-watermark coupling in
scope from day one; streaming recovery parse (recover_all's 64 MiB slurp);
checkpoint-stall measure-then-decide; observer notification thread; async
API worker-pool-or-removal decided by an arena table. Two 0.28.1
interactions: the observer thread builds on API-3's `ObserverState`
shared_ptr (its lifetime semantics are already move-safe and
raw-pointer-free — the notification thread becomes the state's only
locker); and TXN-1's tracker growth (every written key, until pruned —
ycsb_a RSS +10.7% at sandbox scale) belongs to the same memory accounting
as the version heap: the GC watermark and the phantom-tracker prune share
a "oldest live reader" notion, and M6 makes that sharing explicit and
budgeted rather than two independent prunes racing.

**Acceptance** (unchanged): sweep cost linear at 10M keys; space-amp
published and within budget; the PITR format change carries its migration
test; the async decision is an executed commit, not a discussion.

## M7 — Release 0.29.0  **[S]**

**Gate** (carried, extended): delta re-audit (reclamation, merge, writer
thread, format change) clean; P1 proven; arena tables re-run with no
unexplained regressions; changelog complete; CHANGELOG/README/version in
the same commits they describe — **and the tag with them** (0.28.1's
pending tag is the incident this extension encodes, per rule 2's own
precedent of owning its violations).

---

# v30 — The challenge (1.0.0)

Theme unchanged: convert the overhauled engine into a *defensible* product —
every property the audits verified by hand becomes machine-checked; every
performance claim becomes a published, regression-gated table; the scale
story gets a number a database person respects; the version number means
what it says.

## M0 — Invariant machine-checking  **[M]**

Carried unchanged (appendix, v30 §M0): every canonical invariant ID gets a
runtime checker, a CI job, or a reviewed checked-by-construction argument;
the 14-row matrix becomes a living test-plan document; the untested-error-
lines ledger driven to zero for the durability path; docs gain the
invariant→checker→job mapping. One addition from this arc: the matrix gains
Audit-2's rows (the TXN-1 recording-completeness property, the `[k,k]`
point-range equivalence, WAL-1's classification contract, BT-1's headroom
bound) — the canonical ID set extends, per the reading rules, when new
invariants land. The lint ratchet (0.28.1) is the first invariant of this
kind already machine-checked in CI; v30 M0 generalizes the pattern.

**Acceptance** (unchanged): intentionally breaking any invariant in a
scratch build fails at least one CI gate — proven once per invariant,
recorded in the audit trail.

## M1 — The scale soak  **[L]**

Carried unchanged (appendix, v30 §M1): 100M keys, RSS-level memory budgets
(tree pool + version heap + reader slack), pool-high-water curve, cold
recovery ≤ 30 s at 100M keys **with the bulk-load/sorted-build path as a
named prerequisite** (the `nm_` per-key mutex makes it unreachable
otherwise; the `nm_` sharding decision is measured by v29 M1's fills and
taken before this milestone ends); 24-hour arena soak + 7-day steady-state
variant on the soak vehicle; read-side budgets against LMDB/SQLite with
"published, regressing loudly when touched" as the gate. One 0.28.1
interaction: TXN-2 closed the negative-lookup materialization leak that
would otherwise have made a read-heavy soak's memory curve lie — the soak
measures a pool whose growth is real workload growth, not phantom index
entries.

## M2 — Build and release engineering  **[M]**

Carried (appendix, v30 §M2) with two updates. The anchor grew: 28,016 lines
in one TU (13,445 + 14,571 as of 0.28.1), `-O1` OOMs 1 GiB, ASan doesn't
build there at all; the stale "~17k" README/Makefile counts are now stale
by ~11k, and this milestone still corrects all three. The prerequisite
shipped early: **API-1 (0.28.1) made the header ODR-safe** — double-include
and two-TU link are CI-gated — so the amalgamated single header becomes a
*generated release artifact* assembled from sources that are already
multi-TU-correct, and the test-side split (v29 M2 item 5) will have proven
the build system half. Reproducible builds (`sha256`-stable per commit),
signed tags, release tarballs, vendored baselines stay out of the artifact.

**Acceptance** (unchanged): a 1 GiB container builds the split repo at
`-O2`; the amalgamated artifact passes the full suite including the arena
smoke leg.

## M3 — Power-loss truth  **[L]**

Carried unchanged (appendix, v30 §M3): `dm-flakey` fault-injecting block
layer; crash-fuzz grammar extended with fsync-lies and device-lose-flush;
plan B named in the milestone (`scsi-debug`, or documented local VM rig +
reduced CI leg); fail-first-at-the-block-layer acceptance. Audit-2 raised
the stakes with evidence: **WAL-2 and WAL-3 are exactly this bug class** —
directory-entry durability and a wrong-directory fsync are invisible to
`_exit()`-based crash fuzzing (the page cache survives; the directory
metadata was never tested against a lying device) and were found by
*reading*, not by running. A block layer that lies is the only harness
that could have caught them mechanically. The D-invariant set finally gets
tested against a layer that can violate it.

## M4 — Documentation overhaul  **[M]**

Carried unchanged (appendix, v30 §M4): user guide beyond the README;
failure-mode catalog (every `Status` and `health()` level with operator
action); safety-properties page updated to post-v29 truth; the honest
head-to-head page generated from the arena ledger. Addition: the
safety-properties page must document the TXN-1 semantic change (scanned-
range updates conflict; no-op deletes of absent keys don't, and why that
is sound) — a contract change shipped as a flipped test deserves a
prose contract too.

## M5 — The 1.0.0 gate  **[S]**

Carried unchanged (appendix, v30 §M5): final independent adversarial audit,
full depth, published in-tree; API/ABI freeze + LTS policy; semantic
versioning from here forward. Acceptance: verdict **A or B+**; benchmark
tables published and regression-gated; every invariant machine-checked;
failure-mode catalog complete; sign-off in the changelog. Audit-2 is the
precedent that gives this gate meaning: the last "final" audit of a
remediation-complete engine found thirteen more defects, one of them a
soundness hole in the flagship claim. The 1.0.0 round assumes nothing.

---

# Disposition of the 2026-09-27 plan

Every item of the superseded plan, accounted for (its own old-arc
disposition table travels with it in the appendix and still governs those
older items):

| 2026-09-27 item | Disposition here | Note |
| --- | --- | --- |
| Header + revisions 1–3 | **SUPERSEDED** by this document's writing line | The plan is preserved verbatim in the appendix |
| The challenge table | **UPDATED** to the 0.28.1 column | Same four fronts; claims 1–2 strengthened by Audit-2's evidence |
| Where the project stands (0.28.0) | **REWRITTEN** for 0.28.1 | Same honest-debt structure; API-1 closed, wall rose |
| Progress rows | **CARRIED + EXTENDED** | Audit rows, 0.28.1, CI hardening, M0 closure pending |
| Reading rules | **CARRIED VERBATIM** + one new | "Verdicts are round-scoped" |
| Load-bearing constraints 1–5 | **CARRIED** with status annotations | Constraint 1: executed, closure pending |
| v29 M0 | **STATUS CHANGE** | Executed → closure gate (report in-tree, delta confirmation, tag) |
| v29 M1 | **STATUS CHANGE** | Steps 1–3 done; step-4 vehicle split; remaining list reordered around the first completed nightly |
| v29 M2 | **EXTENDED** | Items 1–4 carried; item 5 added (TU pressure relief, API-1-unlocked) |
| v29 M3–M7 | **CARRIED** | With 0.28.1 interaction notes (D4/BT-1, EXTRA-1 latch point, ObserverState, tracker growth) |
| v30 M0–M5 | **CARRIED** | M2 gains the API-1-shipped note; M3 gains the WAL-2/3 evidence; M4 gains the TXN-1 contract-doc note |
| Old-arc disposition table (v28/v29-old/v30-old) | **CARRIED IN APPENDIX** | Still governs; PARKED items remain PARKED |
| Explicitly out of scope | **CARRIED VERBATIM** | Including the cts-monotonicity protection |
| Process rules 1–10 | **CARRIED VERBATIM** | Rules 11–14 added below |
| Suggested first commit | **SUPERSEDED** | By "The next five commits" below |
| External review disposition (rev 2) | **HISTORICAL** | Travels with the appendix |

# Explicitly out of scope (kept)

The deferrals survive this rebase because they were correct:
**Raft/replication** (a multi-version project of its own), **language
bindings** (conflicts with single-header purity), **encryption at rest**
(the embedder's filesystem layer), **multi-process / non-Linux**
(documented limitations with real architectural cost), **third-party
compression** (breaks the zero-dependency differentiator; delta-encoding
gets most of the win), **SQL** (a different product; the comparison page
says so plainly).

One genuine strength to keep protecting: `cts` is a monotonic counter, not
wall-clock — the entire clock-skew bug class does not apply. Do not
introduce a wall-clock dependency anywhere in the durability path; it would
open a bug class the project currently does not have. (Audit-2 agrees by
silence: thirteen findings, none clock-related.)

# Process overhaul (permanent rules)

Rules 1–10 carried verbatim from the 2026-09-27 plan (appendix): fail-first
or it didn't happen; version identity in the same commit; changelog at merge
time; every accepted external finding gets an ID; the reviewer checklist;
composition over duration; known-limitation entries before the fix exists;
refuted probes stay in the suite; measured claims only; baselines are
benchmarks, not dependencies. Four new rules, each paid for:

11. **Audit rounds repeat; verdicts are round-scoped.** "Verified sound"
    means *that round found nothing*. Every arc ends with an independent
    round over the shipped state, and Critical/High findings loop back to
    remediation before rewrites proceed. Earned by: TXN-1, a soundness hole
    in the flagship claim, shipped under round 1's "phantom detection
    (verified sound)" grade.
12. **Fixes are cross-verified independently of the fixer.** The harness
    that reviews a fix is not the harness that wrote it; differential PoCs
    run against pre-fix code for every claimed defect. Earned by: the
    `put_with_old` parity gap — a changelog claim contradicted by the
    shipped code, caught only because a second harness probed the claim
    instead of trusting it.
13. **CI vehicles must fit their own budgets — measured, then sized.** A
    new CI job ships with a budget derived from a measured run, or with an
    explicitly-generous cap and a written tighten-after-measuring follow-up
    in the job itself. Earned by: two consecutive nightlies (#62, #65)
    killed mid-baselines by a 60-minute cap set before anyone had ever
    measured the vehicle — the ledger's green-week clock lost nine nights
    to a number nobody checked.
14. **Contract changes flip tests; they never delete them.** When a fix
    deliberately changes observable semantics, the contradicting test is
    renamed, flipped, and commented with the finding ID at the test site —
    the change stays visible in the suite forever. Earned by: v17 guard
    test 4, whose old expectation ("should remain green after the
    redesign") encoded exactly the TXN-1 anomaly; it now reads "update of
    present key in scanned range CONFLICTS" with the citation inline.

# The next five commits

1. **This document's adoption** (rule 3 mechanics): replace the plan,
   CHANGELOG entry, README Roadmap paragraph updated — one commit.
   **DONE** (`b5a7eee`).
2. **Promote the experimental legs**: delete the four `experimental: true`
   flags (condition met: green ×3 in #64/#65/#66) — **DONE** (`0027fe0`).
   Still open: fix the four clang warnings (3× `-Wunused-lambda-capture`,
   1× `-Wunused-private-field`) so the clang *lint* leg loses its
   `continue-on-error` too — needs a clang-18 environment to fix verified
   (no unverified warning fixes); tracked here until landed.
3. **M0 closure, part 1**: publish the Audit-2 report under `docs/audits/`
   (rule 8) and tag `v0.28.1` (rule 2) — **DONE 2026-10-08**, with the
   report committed as a labeled reconstruction (the original session was
   not preserved; the document states exactly what is consolidated from
   where, and what is unrecoverable).
4. **First completed nightly** — **DONE** (#68, 2026-10-08: chronokv
   31m55s, baselines 15m33s, both artifacts up, nightly green); caps
   tightened from the measurement in the same commit as this status
   (baselines 180→60). The green-week clock is running — six more
   nights, then noise-band calibration and gate enablement (M1
   remaining item 4).
5. **M0 closure, part 2** — **DONE 2026-10-08**: delta confirmation
   round executed (20 probes, verdict B+, one Low doc-truth finding fixed
   in the closure commit); M3 unlocked. **M2 item 1 (the write-skew
   checker) landed the same day** — validated live against the TXN-1 PoC
   history on both engines, before any M3+ rewrite starts (constraint 3
   honored). Remaining catcher work: M2 items 2–4 (DST grammar growth,
   de-vacuation, crash-fuzz composition) — item 5 (TU pressure relief)
   shipped early on 2026-10-10 because the wall kept biting (bare `-O0`
   monolith builds were already dying on the confirmation sandbox).

---

# Appendix — the superseded 2026-09-27 plan (v29 → v30, revision 3), kept in full per the disposition rule

Written against `main @ 3161c77` (0.28.0). Superseded 2026-10-07 by the
rebaselined plan above; preserved verbatim below — including its own
appendix (the 2026-09-17 plan). Statuses inside are as of 0.28.0; the
disposition table above maps every item to its current home.

# ChronoKV roadmap — v29 → v30: challenge the real databases

Written 2026-09-27 against `main @ 3161c77` (**0.28.0**, the complete audit
remediation arc, tagged). This document **supersedes** the previous v28/v29/v30
sections of `docs/ROADMAP.md` (written 2026-09-17 against v25.3, *before* the
adversarial audit). The v26/v27 history from that plan is preserved in the
appendix at the end of this file. Every old-arc item is accounted for in the
disposition table — nothing was silently dropped, which is itself a rule now
(process overhaul, rule 3).

**Revision 3 (2026-09-27).** Post-review citation-precision pass (external
review round 2 — verdict *adopt after four one-line fixes*): the baseline
hardware is correctly attributed (external-review dev sandbox on an
io_uring-blocked 4.19 kernel, **not** a CI runner — rule 9); M3's pool-default
cite is relabeled to the constructor parameter it quotes, cross-referencing
the `Options` field; M5's handshake cite becomes a member-name anchor (the
document's own rot rule); the `docs/` inventory sentence is corrected (the
roadmap and changelog ARE in-tree); and the recovery re-insert estimate is
tightened to uncontended-path arithmetic (conclusion unchanged: the
sorted-build prerequisite stands). Adoption mechanics applied: the v26/v27
history of the 2026-09-17 plan is preserved as this file's appendix. No
structural changes; the revision-2 review-disposition table stands.

**Revision 2 (2026-09-27).** The draft was externally reviewed — verdict
*adopt after edits*. Every item of that review is accepted and incorporated:
eight factual corrections (A1–A8), six technical-gap items (B1–B6), four
feasibility items (C1–C4), four coherence items (D1–D4), plus the reviewer's
suggestion to name the order-triple identity in M5's acceptance. The
review-disposition table at the end maps every item to where it landed — rule 4
applied to this document's own review.

Adopting this document is itself a commit with mechanics: replace the old
v28/v29/v30 sections **and** the in-tree progress row that still reads "v28
M0–M4 (writer thread) / v29 / v30 | not started" (added with the 0.28.0
release commit), keep the v26/v27 history, add the CHANGELOG entry in the same
commit (rule 3), and link the disposition table from the README's Roadmap
paragraph so "nothing was silently dropped" is discoverable, not merely true.

Companion documents, in dependency order — **not yet in the tree** (of the
audit-era documents `docs/` holds only `request.txt`, the original prompt,
alongside the roadmap and changelog); committing them is v29 M0's
deliverable zero:

- **ChronoKV Full Adversarial Audit** (2026-09-23) — 21 findings, verdict D.
  The event that re-prioritized the previous arc.
- **ChronoKV Remediation Specification** (2026-09-24) — the 20-commit, 5-wave
  fail-first fix plan. **Executed in full and shipped as 0.28.0.**
- **This roadmap** — what "done being broken" buys: the structural overhaul
  (v29) and a run at the incumbents (v30, 1.0.0).

---

## The challenge, stated honestly

ChronoKV is a zero-dependency, single-header, single-process embedded
key-value store. That niche has incumbents, and they are not soft targets:

| | SQLite | LMDB | RocksDB | ChronoKV today (0.28.0) |
| --- | --- | --- | --- | --- |
| Deployment scale | billions | hundreds of millions | industry standard server-side | hobby/evaluation |
| Correctness proof | TH3 + OSS-Fuzz + astronomical field exposure | long field history, simple core | huge field history, battle-tested | adversarial audit + fail-first battery + 4-config sanitizer matrix — *deep but young* |
| Transactions | WAL mode: snapshot isolation; writers serialized | MVCC, single writer at a time | write-batch atomicity; SI via pessimistic/optimistic `TransactionDB` — not serializable | **SSI — serializable, write-skew-safe, multi-writer** |
| Durability I/O | pwrite + fsync (portable) | mmap + msync | pwrite + fsync (pluggable env; io_uring on read paths) | **default-path io_uring write→fdatasync chains** — kernel deadline (`LINK_TIMEOUT`), registered-buffer/file ladder, 3-strike pwrite fallback |
| Recoverable to a point in time | no (backup only) | no | yes (WAL + backups) | **yes — PITR is a first-class API** |
| Memory at scale | page cache, transparent | virtual, sparse | block cache + memtables, tuned | **256 MiB monotonic pool — the credibility gap** |
| Form factor | amalgamation, ubiquitous | small C lib | large library suite | **one header, zero dependencies** |

Read that table twice and the strategy writes itself. ChronoKV will not
out-test SQLite's field exposure in a decade, will not out-feature RocksDB's
tuning surface in two, and should not try. What it can do — what this arc
does — is close the one front where today it is not credible (memory and
steady-state behavior at real scale), press the two fronts where it is
already ahead of every incumbent in its class (transactional semantics and
durability I/O), and convert the front nobody else publishes on (adversarial
verification) from an event into a permanent, machine-checked property.

Four fronts, four claims:

1. **Trust, by construction.** SQLite proves correctness by volume of
   testing and exposure. ChronoKV proves it by *construction*: every
   canonical invariant machine-checked in CI, every audit published in-tree,
   every fix born fail-first, power-loss semantics tested against a block
   layer that actually lies. At 1.0: the most *verifiably* correct embedded
   KV store, not the most *field-proven* one — the claim is checkable, and
   the check ships with the product.
2. **Serializable by default.** Every incumbent in the class stops short of
   serializable: SQLite's WAL mode and LMDB serialize their writers, and
   RocksDB's `TransactionDB` (pessimistic or optimistic) tops out at snapshot
   isolation — SI, not SSI; write skew is not prevented. ChronoKV runs
   concurrent serializable (SSI) transactions and, by v29's end, proves them
   with a write-skew checker in CI.
3. **fsync-per-commit at group-commit prices.** io_uring is not unique in
   the class — RocksDB's POSIX env uses it on read paths — but no embedded KV
   makes it the **default** WAL durability path. ChronoKV's is a hard-linked
   write→fdatasync chain with a kernel-enforced deadline (`LINK_TIMEOUT`), a
   registered-buffer/file ladder, and a runtime-degrading offset-pinned
   pwrite fallback (0.28.0: deep availability probe + 3-strike CQE
   degradation). With the v29 writer-thread rewrite, chained write+fsync
   plus adaptive group commit is the throughput story: durable writes that
   don't serialize the committers.
4. **Real scale, bounded memory.** 100 million keys in bounded *memory* at
   steady state — tree pool, version heap and reader slack, all three
   budgeted: `page_pool_bytes` alone bounds only the B+ tree, while values
   live in `Version` chains on the regular heap — with recovery in tens of
   seconds and numbers published — or the other three claims don't matter,
   because nobody who needs a *database* can deploy a monotonic memory
   ceiling.

What this roadmap does **not** claim, equally on purpose: no SQL, no
multi-process, no replication, no cross-platform, no third-party compression,
no bindings. The deferral table at the end survives the rewrite; "single
header, zero dependencies, verifiably correct" is the product. The challenge
is to make that product *competitive*, not to make it *bigger*.

---

## Where the project actually stands (0.28.0)

The remediation arc is **complete**. All 21 audit findings are closed —
17 fixed with fail-first regression tests, CKV-009 and CKV-015 traced and
verified as non-defects with contract tests pinning the traced behavior,
CKV-017/021 documentation corrected to match the code. Two platform defects
from the external repository review (F1: io_uring availability probing,
F2: nested `wal_dir` creation) landed alongside. The version identity
defect the previous roadmap flagged as "the cautionary tale" is fixed the
right way:

```cpp
// chronokv.hpp:8779 (0.28.0)
static constexpr const char* CHRONOKV_VERSION = "0.28.0";
```

And the changelog that the header had referenced since the v22 arc but that
never existed in-tree now exists, seeded with the full 0.28.0 entry plus
backfilled history:

```text
// docs/CHANGELOG.md (0.28.0 entry, opening)
The 21 findings of the docs/request.txt adversarial audit (CKV-001..021)
plus the two platform defects from the external repository review (F1/F2).
Every fix shipped with a fail-first regression test in the remediation
battery (CKV_ONLY_REMEDIATION=1); "verified" findings ship contract tests
that pin the traced behavior. No on-disk format changes anywhere in the arc.
```

Worth recording because it shapes v29: the implementer did not blindly
transcribe the specification. CKV-004's fix re-scoped the rollback gate to
the *failure stage* rather than the spec's durability-class predicate, with
a documented silent-loss contract for the one surviving case (a published
async batch failing at FSYNC keeps its records, counted in
`async_committed_then_lost`). CKV-012 gained a Phase-2 hardening the spec
implied but did not spell out (the burned cts also latches the WAL
fail-stop, so an unrecoverable interior hole can never be written past).
CKV-013 was fixed with `(st_dev, st_ino)` identity keying where the spec had
recommended dead-path deletion — a strictly stronger outcome. This is what a
healthy implementer looks like: the spec is the floor, review is the judge.

What 0.28.0 did *not* do, and v29 inherits:

- **The re-audit never ran.** The remediation is self-verified (its own
  battery), not independently verified. `docs/audits/` does not exist yet.
  v29 M0.
- **The suite still carries vacuous tests** the audit enumerated beyond the
  arc's scope (v17 GC-boundedness, v18 LSN-contiguity, `m2_phase1`'s
  size-rotation admission, the 30 s soak's missing oracle). Partially
  de-vacuated during the arc; not systematically. v29 M1.
- **The structural debt is untouched**: the monotonic page pool
  (`Options::page_pool_bytes`, 256 MiB default), `PagePool::free()` with no
  *engine* callers (only the unit tests exercise it — `main.cpp:7580`,
  `7602` — which keeps it compiling, not used), `is_hazardous()` with no
  callers at all, no merge/rebalance, GC sweeps at O(N²/256), the
  leader-election handshake in the WAL committer, one ~27k-line translation
  unit — `chronokv.hpp` 13,178 + `main.cpp` 14,231 = 27,409 lines in a
  single TU, and the README's and Makefile's "~17k" counts are stale too
  (v30 M2 fixes all three) — that OOMs a 1 GiB container at `-O2`.
- **The README's only performance number is an indictment — but the tree is
  not as numberless as the README makes it look.** The suite's Phase-3
  item-12 harness (`main.cpp:3344+`) prints *and gates* performance
  baselines on every run: commit throughput at 1 and 4 threads, read latency
  under writes, recovery rates — the last full run during the external
  review, on a 2-CPU / 1 GiB **dev sandbox** (not a CI runner) whose 4.19
  kernel blocked io_uring writes, at `-O1`: 21,083 / 42,719 commits/s,
  838 ns mean read latency, 19.8 ms per 1k records recovered. Two caveats ride every table
  v29 publishes: those numbers measure the **pwrite fallback** path — the
  io_uring backend has never been benchmarked by the suite on this class of
  machine — and the README publishes none of it. The async API at
  **0.256× sync** (thread-per-op) remains the README's lone number. "You
  preach measurement and publish no numbers" — the previous roadmap's own
  words, still true of the README, no longer excusable by the tree.

### Progress

| Milestone | Status | Shipped as |
| --- | --- | --- |
| Adversarial audit (21 findings, verdict D) | **DONE** | 2026-09-23 report |
| Remediation specification (5 waves, 20 commits) | **DONE** | 2026-09-24 report |
| Wave 1 — CKV-001, 002, 003a/b/c, 005 | **DONE** | `9016918`…`f2843d0` |
| Waves 2–5 — CKV-004…021 + F1/F2 + 012R | **DONE** | `1fe98b0`…`3161c77` |
| 0.28.0 release (version + CHANGELOG + tag) | **DONE** | `ced3806`, tag `v0.28.0` |
| v29 M0 — independent re-audit | deliverable zero **DONE** — the audit report and remediation specification are committed under `docs/audits/`; the re-audit commission itself is pending | `docs/audits/2026-09-23-full-adversarial-audit.md`, `docs/audits/2026-09-24-remediation-specification.md` |
| v29 M1 — benchmark arena | **STARTED** — steps 1–4(start): engine-side workloads + YCSB A–F mixes + SQLite/LMDB/RocksDB adapters (six-way table validated) + nightly informational ledger vehicle in CI (fixed 200k-key geometry, artifacts; NOT gating until the calibrated green week per M1 acceptance). Remaining: D/E/F baseline adapters, version pinning/vendoring, noise-band calibration + gate enablement, ledger-promotion decision | `bench/arena.cpp`, `bench/baselines.cpp`, ci.yml `arena` |
| v29 M2–M7 — catcher hardening + the overhaul | not started | — |
| v30 M0–M5 — the challenge, 1.0.0 | not started | — |

---

## Reading rules (kept, plus one new)

The previous roadmap's rules were good rules; the audit proved their worth in
the negative (its findings were, almost uniformly, code drifting from a
documented claim — a "12 bytes" comment over an 8-byte struct, a sentinel
documented as covering all keys). They are restated because the plan is
weaker without them:

- **No speculative work.** Each item is anchored to a measurement in-tree,
  a README known limitation, a landed defect, or an incumbent's shipped
  behavior. The arena items are anchored to the *absence* of numbers, which
  is a measurement of a kind.
- **Every fix ships with a test verified to fail against the unfixed code.**
  Fail-first is now proven at arc scale — stated precisely: the battery is
  ~25 scenarios / ~69 checks; every *defect-asserting* check was
  fail-first-verified (two of them by temporary mutation: CKV-008's epoch
  comparison, CKV-004R's rollback gate), while the deliberate controls
  (torn-tail, outside-range phantom, fresh/empty-destination legs,
  fresh-fence baseline) and the CKV-009/015 traced-verification contract
  tests pass pre-fix *by design*, as their commits say.
- **New invariants get canonical IDs**, extending R/I/D/E/P/B/T. v29 adds
  P1; v30 M0 machine-checks the whole set.
- **On-disk format changes require a documented migration rule before
  implementation.** v29 M6 carries the arc's first (multi-version PITR
  deltas).
- **NEW — measured claims only.** No performance claim appears in the README
  without a methodology, a hardware note, and a reproduction path. Numbers
  that cannot be re-run do not get published. Regression gates beat
  point-in-time victories: a number is a *budget*, not a trophy.

Sizes are T-shirt guesses, not commitments: **S** ≈ days, **M** ≈ 1–2 weeks,
**L** ≈ 3–6 weeks, **XL** ≈ a quarter. At those sizes the arithmetic is: v29
≈ 7–9 serial months (M4 is the XL), v30 ≈ 5–6 more — **1.0.0 in roughly
12–15 months, at the pace that shipped the 21-finding arc in five weeks**.
That pace assumption is stated out loud so "ambitious" does not silently
become "aspirational" around month four; if the pace halves, the plan
re-sequences its milestones, it does not renegotiate its gates.

Citations in this document prefer function-scope anchors over line numbers
and carry an "as of 0.28.0" tag, because this repo proved three times in one
arc that line-number citations rot (CKV-004's stale comment masked a
durability bug; CKV-021 fixed two more).

### Load-bearing constraints

1. **The re-audit (v29 M0) lands before any rewrite.** v29 rewrites the most
   concurrency-sensitive code in the engine. Doing that on self-verified —
   not independently verified — remediation is the same gamble the audit
   already collected on once.
2. **The arena (v29 M1) lands before the rewrites it measures.** Their own
   old rule, restated as constraint: *measure before predict*. The writer
   thread, reclamation, and merge milestones each publish before/after
   numbers against the same harness, or they didn't happen.
3. **Catcher hardening (v29 M2) lands before the rewrites it guards.** The
   v25.3 lesson: never validate a rewrite of race-hazard code with a harness
   that cannot catch the bug class the rewrite creates.
4. **The 1.0.0 gate is not negotiable.** 1.0.0 ships only on a clean
   independent adversarial audit (verdict A/B+) *and* published benchmark
   tables *and* the scale soak. A new Critical holds the release — that is
   what the number is for.
5. **Composition over duration.** Widen the composition of existing regimes
   (crash-fuzz plans, DST scenarios, fault kinds) before inventing new
   harnesses.

---

# v29 — The overhaul (0.29.0)

Theme: close the credibility gap. Every structural weakness a real database
would laugh at — the monotonic pool, the missing merge, the leader handshake,
the O(N²) GC, the invisible benchmarks — gets closed or measured, in an
order where each rewrite is guarded by catchers written before it and
measured by an arena built before it. Three of the four subsystems are
touched (page lifecycle, delete path, WAL committer); the verified-sound list
(SSI validation ordering, group-commit semantics, epoch reclamation E1–E16,
close-race lifecycle, PITR boundaries) is preserved, not redesigned.

## M0 — Independent re-audit  **[S] [GATE]**

**Anchor.** The audit's exit condition: verdict D was issued with a
remediation path; the path's end must be verified, not assumed. 0.28.0's
verification is self-verification (the battery it shipped with).

**Work.** Deliverable zero, before the commission: commit the two companion
documents this roadmap leans on — the audit report and the remediation
specification — under `docs/audits/`. Neither is in the tree today (of the
audit-era documents, `docs/` holds only `request.txt`, the input prompt),
yet the 14-row invariant matrix, rule 8's probe
IDs, and "verdict D" all cite them; until they land, a new contributor
cannot verify half this document's references — a documentation-integrity
failure of exactly the class the reading rules were written against. Then:
commission an independent adversarial re-audit of 0.28.0 at the original's
depth (code-read + PoC + verify, not checklist), with the remediation
specification and its status table handed over as the map. Report committed
under `docs/audits/2026-10-re-audit.md`. Any finding it produces is fixed
fail-first *before* M3 starts — defects first, rewrites never.

**Acceptance.** Zero Critical/High findings, verdict B or better. A Critical
or High loops back to remediation before any rewrite milestone begins.

## M1 — The benchmark arena  **[M] [MEASURE FIRST]**

**Anchor.** The README contains no performance numbers — but the arena does
not start from zero: the suite's Phase-3 item-12 harness (`main.cpp:3344+`)
already prints and gates commit throughput, read latency under writes, and
recovery rates on every run. The arena *extends an existing in-tree
harness*. The caveat that rides every published table: current numbers
measure the pwrite fallback path; the io_uring backend has never been
benchmarked by the suite on this class of machine. Old-arc v30 M1's line
still lands — "you preach measurement and publish no numbers" — but it
indicts the README, not the tree.

**Work.**
1. `bench/arena.cpp` — a db_bench-style micro suite: fill sequential /
   fill random, point read (random + zipfian), overwrite, short and long
   range scans, delete-heavy churn, checkpoint-under-load, cold open /
   recovery, memory-per-key at steady state. `fillseq`/`fillrandom`
   deliberately isolate the **new-key rate** — every new key takes the
   single global mutex in `ensure_index`'s slow path (`nm_`), the same
   serialization the 100M-key soak and cold recovery will drive straight
   through; the arena measures that bottleneck before the soak hits it, and
   the `nm_` sharding decision is taken on its numbers. Memory-per-key is
   measured at the **RSS level** — tree pool + version heap + reader slack,
   not pool-only — because `page_pool_bytes` bounds only the B+ tree while
   values live in `Version` chains on the regular heap, reclaimed by epoch
   GC against the watermark and the oldest reader: nothing to do with the
   pool.
2. YCSB-style workloads A–F (workload mixes, not the framework): the same
   six mixes re-expressed against the arena harness with the documented
   record/request distributions.
3. Vendored baselines, same harness, same hardware, methodology written
   down once and applied identically: **SQLite** (WAL mode;
   `synchronous=FULL` and `=NORMAL`), **LMDB** (defaults), **RocksDB**
   (defaults + one tuned config) — each **version-pinned and build-cached**:
   a cold vendored RocksDB build costs 10–20 minutes per CI run, and pinning
   plus caching is what keeps the ledger's comparisons reproducible across
   runner images. Baselines are benchmarks, not dependencies — they live
   under `bench/third_party/`, never in the engine's include path.
4. CI: nightly arena run on a fixed-spec runner, results appended to a
   tracked `bench/results/` ledger; p50/p99/throughput deltas beyond noise
   fail the job. The noise gate gets a statistical protocol, budgeted into
   this milestone: per-metric noise bands calibrated from repeated baseline
   runs, N-iteration medians, and an explicit re-run policy — otherwise the
   first flaky red on a shared runner teaches the team to ignore the ledger,
   and the ledger is the deliverable. This milestone also **names the soak
   vehicle** — self-hosted runner, rolling chain of nightlies with persisted
   state + aggregate verdict, or an offline rig with committed logs — that
   M4 and v30 M1 depend on: GitHub Actions kills jobs at 6 h, and the
   existing nightly DST already shards 8× to fit 4 h after runner variance
   bit it once. The regression gate is the deliverable — the tables are
   its output.

**Acceptance.** `make arena` produces the six-table comparison; the README's
performance section exists and links the methodology; the nightly ledger
shows one full green week. No engine changes in this milestone — the
yardstick is built before anything is measured against it.

## M2 — Catcher hardening  **[M]**

**Anchor.** Remediation spec §11: "lincheck's SSI blindness (no
anti-dependency checker) … the checker itself is ROADMAP material." Plus the
remaining audit-enumerated vacuous tests.

**Work.**
1. lincheck gains the SSI anti-dependency (write-skew) checker and a
   synthetic write-skew anomaly battery — the checker that proves claim 2.
2. The DST plan grammar grows merge/rebalance/cascade and
   reclamation-retirement scenarios *before that code exists* — scenarios
   written against the design; the implementation must survive them.
3. De-vacuate the remainder: v17 GC-boundedness, v18 LSN-contiguity,
   `m2_phase1` size-rotation, the 30 s soak's oracle. Each upgraded test
   proven once by temporary reversion in a scratch build.
4. The crash-fuzz plan grammar composes GC sweeps, checkpoint, rotation and
   merge windows (the v25.7 lesson, applied to the new surface).

**Acceptance.** A synthetic write-skew history fails the new checker; every
de-vacuated test fails against a reintroduction of its defect class; the
merge/reclaim DST scenarios run green against the current (merge-free) tree.

## M3 — Page reclamation  **[L]**

**Anchor.** README known limitation #1 — `PagePool::free()` has no
*engine* callers (the unit tests at `main.cpp:7580`, `7602` keep it
compiling — the README limitation entry gets the same one-word fix when M3
lands); `is_hazardous()` has no callers at all; the pool is a monotonic
ceiling:

```cpp
// The engine-side ceiling: ChronoKV's constructor default parameter
// (chronokv.hpp:5656, as of 0.28.0); the public Options::page_pool_bytes
// field mirrors it at chronokv.hpp:8886.
size_t page_pool_bytes = 256ULL * 1024 * 1024,   // 256 MiB default

// the admission, as of 0.28.0 (chronokv.hpp:742)
// is_hazardous() had zero call sites, so it protected nothing.
```

**Work.** Wire `PagePool::free()` and give the hazard-pointer machinery its
consumers; retired pages return through the existing epoch reclaimer or
generation-tagged page ids — the ABA (HP4) decision is made **before**
implementation, per the old roadmap's own rule. CKV-008's page mutation
epoch (landed in 0.28.0) is the stale-descent net for **live** pages: it
detects in-place mutation of a still-valid page during fence validation. It
does *not* by itself make page **reuse** safe — a freed and re-allocated
PageId restarts its epoch near zero, so a pre-free captured fence can alias,
which is exactly the ABA the HP4 decision exists for. Reuse safety comes
from the hazard-pointer consumer plus generation-tagged page ids, decided
before implementation (as this milestone already requires). Pool exhaustion
surfaces through the 0.28.0 D4 latch semantics — one health model, not two;
`stats()` gains pool utilization and high-water mark, `health()` degrades at
80 % and fails at 95 %.

**Acceptance.** Delete-then-reinsert workload: high-water mark stabilizes
instead of ratcheting; TSan-clean; no use-after-free under ASan with reuse
enabled; arena delete-churn table shows bounded memory.

## M4 — Leaf merge and rebalance  **[XL]**

**Anchor.** The same README limitation. Feasible *now* because 0.28.0 landed
the plan-before-mutate byte-aware split planner — merges are its mirror
image, sharing the capacity model.

**Work.** Crabbing with exclusive latches on the delete path; sibling borrow
vs. merge; interior key deletion; root collapse. The fence-recheck
machinery already exists for splits.

**New invariant.**
> **P1** — under a sustained delete-heavy workload the page pool reaches
> steady state with bounded utilization, with no use-after-free and no ABA.

**Acceptance.** P1 under a 7-day soak (ASan/TSan interleaved) **on the soak
vehicle named in M1** — GitHub Actions kills jobs at 6 h, so a literal
7-day hosted job is not a thing that can exist: self-hosted runner, rolling
nightly chain with persisted state + aggregate verdict, or offline rig
with committed logs; concurrent cursor + concurrent merge clean under TSan
*and* M2's DST scenarios; the differential-vs-`std::map` test passes with
merges active; the arena's delete-churn and space-amplification tables
move.

## M5 — WAL writer thread  **[L]**

**Anchor.** The most bug-dense concurrency code in the engine exists only
because the leader role migrates between committer threads — the code that
produced C1 and H3:

```cpp
// WalSegments' leader-election member block — the handshake's entire
// state, verbatim (as of 0.28.0; the adjacent pending_/leader_active_
// pair in WalSegments' private members — function-scope anchor per the
// reading rules, line numbers deliberately not cited)
std::deque<std::shared_ptr<Batch>> pending_;
bool leader_active_ = false;
```

**Work.**
1. The M1 arena baseline is the "before" column: throughput and
   p50/p99/max at 1/2/4/8/16/32 committers, per durability mode.
2. Dedicated writer thread + bounded MPSC queue; committers enqueue batch
   descriptors and wait on per-batch completion. **Deletes**
   `leader_active_`, the shared `cur_batch_` mutation, the
   leader/follower handshake and the `pending_` FIFO — the code stops
   existing; it does not get rewritten more carefully.
3. Unlock what the handshake made illegal: `SINGLE_ISSUER`,
   `DEFER_TASKRUN`, and `SQPOLL` worth its kernel thread; queue depth > 1
   pipelines batches.
4. Adaptive group-commit linger (`group_commit_linger_us`, default 0),
   auto-tuned under load.
5. Old path one release behind a compile flag, then deleted; the full
   DST + crash-fuzz + fault matrix re-runs against the new path first.

**Acceptance.** Arena "after" tables: no regression at any concurrency;
measurable win at 8+ committers (the claim-3 gate: fsync-per-commit at
group-commit prices); C1/H3-class reversion probes fail loudly; crash-fuzz
green; dead code gone. The full matrix re-run that precedes deletion
re-proves the triple identity **cts-order == WAL-order == phantom-order** —
named explicitly here because it is the invariant most likely to break under
a writer thread.

## M6 — Space and steady state  **[M]**

**Anchor.** README known limitations: GC sweep O(N²/256); observer
reentrancy deadlock; incremental-checkpoint garbage. The v26.1 review's
deferred direction 3; the old v30 M4 (WAL delta-encoding); the async API
at 0.256× sync.

**Work.**
1. GC: persistent incremental tree cursor — a full sweep becomes O(N/256)
   scan work, not O(N²/256).
2. Incremental-checkpoint garbage: slab compaction reuse with a measured
   space-amplification budget, published and tracked across releases.
3. WAL delta-encoding (dependency-free), measured against that budget — in
   only if it pays.
4. Multi-version PITR deltas (v26.1 direction 3): every `(key, cts, value)`
   version of the checkpoint window retained, making mid-window `as_of`
   reconstructable after rotation. **The arc's first deliberate on-disk
   format change**: delta format version bump + documented migration rule
   before implementation — and the **GC coupling is in scope from day one**:
   window versions must survive epoch GC to checkpoint time, a
   GC-watermark interaction that is precisely why direction 3 was
   deliberately not smuggled into a review-response commit. Format change,
   migration rule, and GC coupling ship together or the item does not ship.
5. Streaming recovery parse: `recover_all` currently slurps whole 64 MiB
   segments into memory and sorts all records; the soak's open-after-kill
   with a large WAL window multiplies that RAM spike. A small item with a
   large worst-case payoff — measured at the soak's WAL-window sizes.
6. Checkpoint stall at scale, measure-then-decide: `checkpoint_mu_`
   exclusivity (the writer-preferring rwlock stalls commit reservation for
   the checkpoint's duration) and the full-tree-walk base/rebase snapshot
   (`tree_scan_all` when no dirty set) — at 100M keys the walk alone is
   minutes, during which rotation cannot proceed and the recovery window
   grows. The arena's checkpoint-under-load table decides: concurrent
   checkpoint or bounded-walk deltas.
7. Observer notification thread: callbacks leave the inline-under-lock
   path (also the permanent home of the 0.28.0 observer-containment
   semantics).
8. Async API: worker pool **or removal** — decided by an arena table, not
   taste. Shipping an async API slower than sync is a trap.

**Acceptance.** Sweep cost linear at 10 M keys; space-amplification number
published and within budget; the PITR format change carries its migration
test; the async decision is an executed commit, not a discussion.

## M7 — Release 0.29.0  **[S]**

**Gate.** Delta re-audit (reclamation, merge, writer thread, format change)
clean; P1 proven; arena tables re-run with no unexplained regressions;
changelog complete; CHANGELOG/README/version all updated in the same
commits they describe.

---

# v30 — The challenge (1.0.0)

Theme: convert the overhauled engine into a *defensible* product: every
property the audits verified by hand becomes machine-checked; every
performance claim becomes a published, regression-gated table; the scale
story gets a number a database person respects; and the version number
means what it says.

## M0 — Invariant machine-checking  **[M]**

**Anchor.** The remediation specification ships a 14-row
invariant-preservation matrix; the README documents the R/I/D/E/P/B/T sets.
Today those are *argued*; at 1.0 they must be *checked*.

**Work.** Every canonical invariant ID gets one of: a runtime checker, a CI
job, or a reviewed checked-by-construction argument. The spec's matrix
becomes a living test-plan document; the v27 M3 untested-error-lines ledger
is driven to zero for the durability path; the docs gain the
invariant→checker→job mapping table.

**Acceptance.** Intentionally breaking any invariant in a scratch build
fails at least one CI gate — proven once per invariant, recorded in the
audit trail. This is claim 1 (trust by construction) made mechanical.

## M1 — The scale soak  **[L]**

**Anchor.** Front 4 of the challenge declaration. Real databases are
deployed on multi-GB datasets; 0.28.0's README has never shown a workload
above the suite's key counts.

**Work.**
1. **100 million keys** (16-byte keys, mixed 100 B/1 KiB values, ~11 TiB of
   logical writes through churn): sustained insert + overwrite + delete mix
   at pool sizes from 4 GiB to 64 GiB; publish the pool-high-water curve,
   memory-per-key at the **RSS level** (tree pool + version heap + reader
   slack — a pool-only number would be discounted by any database reviewer
   the moment they read it), and steady-state space amplification.
2. **Cold recovery**: open-after-kill at 10 M / 50 M / 100 M keys; recovery
   throughput published; target ≤ 30 s at 100 M keys (regression-gated) — a
   target that is **unreachable without a bulk-load / sorted-build recovery
   path**: cold recovery re-inserts every checkpoint entry through
   `ensure_index`'s slow path, taking the global `nm_` mutex per new key.
   Even at optimistic uncontended rates — recovery inserts skip the WAL,
   fsync and publication barrier that the 21k commits/s anchor pays —
   100 M re-inserts are minutes-to-tens-of-minutes, not 30 seconds. The sorted-build path (checkpoint + WAL replay into a
   pre-sized tree) is a *prerequisite* of this target, not an optimization,
   and is listed here so the acceptance criterion cannot be silently
   renegotiated mid-soak; the `nm_` sharding decision is measured by v29
   M1's fill workloads and taken before this milestone ends.
3. **24-hour arena soak** + 7-day steady-state variant under the nightly
   ledger **on the soak vehicle named in v29 M1** (hosted CI jobs die at
   6 h: self-hosted runner, rolling nightly chain with persisted state +
   aggregate verdict, or offline rig with committed logs); p99 drift beyond
   noise fails the job.
4. Read-side budgets: point-read p50/p99 within 2× LMDB at 1 M keys
   (regression-gated), single-writer commit throughput ≥ SQLite
   `synchronous=FULL`, concurrent-commit throughput ≥ 2× SQLite WAL at 8
   committers. Absolute parity is not the gate — *published, regressing
   loudly when touched* is the gate. The tables decide where ChronoKV
   genuinely stands, which is the point.

**Acceptance.** The README performance section carries the 100 M-key table,
the recovery table, and the three head-to-head tables with methodology;
each is wired to the nightly ledger.

## M2 — Build and release engineering  **[M]**

**Anchor.** README known limitation: one ~27k-line translation unit —
`chronokv.hpp` 13,178 + `main.cpp` 14,231 = 27,409 lines in a single TU;
`-O2` needs >1 GiB RSS and is OOM-killed below that. (The README's and
Makefile's "~17k" counts are stale; this milestone corrects all three.)

**Work.** Split the sources and fix every line-count claim that touches
them (README, Makefile, prior plans); generate the amalgamated single header
as a *release artifact* (zero-dependency property preserved for embedders,
repo buildable on small containers); reproducible builds (`sha256`-stable
for a given commit); signed tags and release tarballs; the vendored
baselines stay out of the artifact.

**Acceptance.** A 1 GiB container builds the split repo at `-O2`; the
amalgamated artifact passes the full suite including the arena smoke leg.

## M3 — Power-loss truth  **[L]**

**Anchor.** README known limitation, the audit agreed: "crash testing is
not power-loss testing" — `_exit()` leaves the kernel page cache intact, so
every crash-fuzz result validates the recovery state machine, not
durability against a lying block layer.

**Work.** A `dm-flakey`-based fault-injecting block layer wrapping the real
database directory; crash-fuzz plan grammar extended with fsync-lies and
device-lose-flush events; the D-invariant set finally tested against a
layer that can actually violate it. `dm-flakey` needs root (fine — the
runners have passwordless sudo) but loop devices + `dmsetup` on shared or
nested-virt hosted runners are historically flaky-to-unavailable, so the
**plan B is named in the milestone, not discovered mid-flight**:
`scsi-debug` error injection, or a documented local VM rig with published
logs plus a reduced CI leg; whichever vehicle runs, the
fail-first-at-the-block-layer acceptance survives. This is the trust
claim's missing leg: SQLite has TH3 and the field; ChronoKV will have
*auditable hardware-level fault injection*.

**Acceptance.** D2/D3/D4 detectors run against injected flush-loss; a
deliberately weakened fsync fails the harness (fail-first, at the block
layer); results published in the docs.

## M4 — Documentation overhaul  **[M]**

**Work.** A user guide beyond the README (open/close, transactions, PITR,
backup, health, sizing the pool — the operator questions the audit raised);
a failure-mode catalog — every `Status` and `health()` level with meaning
and operator action; the safety-properties page updated to
post-remediation, post-v29 truth; **the head-to-head page**: an honest
"ChronoKV vs SQLite vs LMDB vs RocksDB" comparison maintained from the
arena tables, strengths *and* losses, because a comparison page that only
wins is marketing and everyone knows it.

**Acceptance.** Docs build in CI; every public API symbol documented; every
`Status` value in the catalog; the comparison page generated from the
ledger, not hand-edited.

## M5 — The 1.0.0 gate  **[S]**

**Work.** The final independent adversarial audit, full depth, published
in-tree. API/ABI freeze and the LTS policy (what gets backported, for how
long). Semantic versioning from here forward — no more unbumped arcs, ever.

**Acceptance (release gate).** Audit verdict **A or B+**; benchmark tables
published and regression-gated (M1); every invariant machine-checked (M0);
the failure-mode catalog complete (M4); the gate signed off in the
changelog. A verdict below B+ holds the release — that is what the number
is for.

---

# Old-arc disposition

Every item of the previous v28/v29/v30 sections, accounted for:

| Old item | New home | Note |
| --- | --- | --- |
| v28 (old) M0 — committer baseline | v29 M5 step 1 | Still first; now the arena's "before" column |
| v28 (old) M1 — writer thread + MPSC | v29 M5 | Unchanged in substance |
| v28 (old) M2 — io_uring unlock | v29 M5 | Rides single-issuer |
| v28 (old) M3 — adaptive linger | v29 M5 | Measured post-rewrite |
| v28 (old) M4 — delete old path | v29 M5 | One release behind a flag, matrix first |
| v29 (old) M0 — surface the ceiling | v29 M3 | Exhaustion flows through the D4 latch |
| v29 (old) M1 — merge & rebalance | v29 M4 | On 0.28.0's byte-aware planner |
| v29 (old) M2 — HP consumers | v29 M3 | With reclamation |
| v29 (old) M3 — ABA / HP4 | v29 M3 | Decide-before-build kept |
| v30 (old) M0 — visitor scan | **PARKED** | Real, but below the challenge line; revisit post-1.0 |
| v30 (old) M1 — bench in CI + numbers | v29 M1 + M4 | The arena, then the docs page |
| v30 (old) M2 — async pool or remove | v29 M6 | Decided by an arena table |
| v30 (old) M3 — rollback-on-destroy opt-in | **PARKED** | Reverses a documented deliberate decision; needs maintainer sign-off |
| v30 (old) M4 — WAL delta-encoding | v29 M6 | Against the space-amp budget |
| v30 (old) M5 — source split + amalgamation | v30 M2 | The 1 GiB OOM anchor kept |
| v26.1 direction 3 — multi-version deltas | v29 M6 | First format change; migration rule |
| (audit) lincheck SSI checker | v29 M2 | Before the rewrites it guards |
| (audit) vacuous-test de-vacuation | v29 M2 | Systematic pass, remainder beyond the arc |
| (this roadmap, prior draft) re-audit gate | v29 M0 | Promoted to the arc's first gate |
| (new) YCSB / db_bench arena | v29 M1 | The challenge's yardstick |

# Explicitly out of scope (kept)

The previous deferrals survive the rewrite because they were correct:
**Raft/replication** (a multi-version project of its own), **language
bindings** (conflicts with single-header purity), **encryption at rest**
(the embedder's filesystem layer), **multi-process / non-Linux**
(documented limitations with real architectural cost), **third-party
compression** (breaks the zero-dependency differentiator; delta-encoding
gets most of the win), **SQL** (a different product; the comparison page
says so plainly).

One genuine strength to keep protecting: `cts` is a monotonic counter, not
wall-clock — the entire clock-skew bug class does not apply. Do not
introduce a wall-clock dependency anywhere in the durability path; it would
open a bug class the project currently does not have.

# Process overhaul (permanent rules)

The audit's meta-finding, made policy — rules 1–8 proven by the 0.28.0 arc,
9–10 new for the challenge arc:

1. **Fail-first or it didn't happen.** Every fix ships with a test verified
   to fail against the unfixed code.
2. **Version identity.** Comments and `CHRONOKV_VERSION` ship in the same
   commit. An arc that exists only in comments does not exist. 0.28.0
   itself only acquired its identity at `ced3806` — roughly ten behavior
   commits, some API-visible, had already landed before the version, tag
   and changelog did. That is the incident this rule encodes; it is stated
   here because the rule reads as preachy unless it owns the violation.
3. **Changelog at merge time, not release time.** `docs/CHANGELOG.md`,
   updated in the commit it describes. Superseded-roadmap items get a
   disposition entry, never a silent drop.
4. **Every accepted external finding gets an ID** — finding → spec →
   commit → test, traceable end to end.
5. **The reviewer checklist** (every PR): fail-first test present? Version
   bumped if the arc advanced? Unrelated changes in the diff? Invariant
   matrix updated? Changelog entry present? Known-limitations still true?
6. **Composition over duration.** Widen existing regimes before inventing
   new harnesses.
7. **Known-limitation entries are written before the fix exists.** The
   honest-docs culture the audit explicitly praised.
8. **Refuted probes stay in the suite.** Of the audit's probes, only
   **probe14** is referenced in-tree (the CKV-003b/D4 chain:
   `chronokv.hpp:5353`, `6664`; `main.cpp:11806`); probe9 and probe15 live
   in the audit report, which is exactly why M0's deliverable zero commits
   it under `docs/audits/` — the probe-ID → shipped-test mapping must be
   checkable from the tree, not from memory. A "fix" that makes a refuted
   probe fail is reverting correctness.
9. **Measured claims only.** No README performance claim without
   methodology + hardware + reproduction path; regression-gated, published
   in the ledger. Numbers are budgets, not trophies.
10. **Baselines are benchmarks, not dependencies.** Vendored comparisons
    live under `bench/third_party/`, never in the engine's include path;
    the zero-dependency property is the product and survives the challenge.

# Suggested first commit

**v29 M0, the re-audit commission.** Zero code; maximal leverage. First
commit the audit report and the remediation specification under
`docs/audits/` (deliverable zero — it also makes this roadmap's probe-ID and
verdict references checkable from the tree). Then hand an independent
reviewer the 0.28.0 tag, the original audit, and the remediation
specification's status table, and get the verdict that says the foundation
is what the changelog claims — *before* a single line of the engine is
rewritten on top of it. In the same window, stand up the arena skeleton
(M1 step 1) so the yardstick exists before the first rewrite starts.
Everything else in this arc is gated behind one of those two.

---

# External review disposition (revision 2)

The draft was reviewed externally on 2026-09-27; verdict *adopt after
edits*. Rule 4 applies to this document's own review: every accepted
finding gets an ID and a traceable disposition. All items accepted, none
rejected — and the review's "what is right — do not dilute" list (the
ordering discipline, the delete-don't-rewrite posture for the leader
handshake, the disposition table, the honest non-claims, rules 9–10, the
1.0.0 hold) is preserved untouched by construction: the edits below
tighten claims, they do not soften ambition.

| ID | Finding (abridged) | Disposition in this revision |
| --- | --- | --- |
| A1 | RocksDB cell false — TransactionDB ships SI | Cell rewritten: "SI via pessimistic/optimistic TransactionDB — not serializable"; claim 2 restated to match |
| A2 | "unique in class" overclaims io_uring | Cell + claim 3 now mechanism-specific: default-path write→fdatasync chains, `LINK_TIMEOUT` deadline, registered ladder, 3-strike fallback |
| A3 | "~13k-line TU" undercounts 2× | 27,409 lines (13,178 + 14,231) stated; stale "~17k" README/Makefile counts assigned to v30 M2 |
| A4 | "only performance number in the tree" false | Reframed: item-12 harness credited (21,083 / 42,719 commits/s, 838 ns, 19.8 ms/1k on the 2-CPU/1 GiB `-O1` box); fallback-path caveat carried into M1's anchor |
| A5 | "24 battery tests, all fail-first" overstates | Precise wording: ~25 scenarios / ~69 checks; defect-asserting checks fail-first (two by mutation); controls + CKV-009/015 contract tests pass by design |
| A6 | Rule 8 cites uncommitted probe IDs | Rule 8 rewritten around in-tree probe14 with line cites; audit committed as M0 deliverable zero |
| A7 | Line-number citations rot | Function-scope anchors + "as of 0.28.0" tags throughout; rot rule added to the reading rules |
| A8 | "PagePool::free() no callers" false literally | "No *engine* callers" + unit-test cites (`main.cpp:7580`, `7602`); README one-word fix noted for M3 |
| B1 | `nm_` serialization unassigned; ≤ 30 s unreachable | New-key rate isolated in v29 M1 (`fillseq`/`fillrandom`); bulk-load / sorted-build recovery added as v30 M1 prerequisite; `nm_` sharding decision added |
| B2 | "Bounded memory" bounds the wrong half | RSS-level budget (tree pool + version heap + reader slack) in claim 4, v29 M1 methodology, and v30 M1 item 1 |
| B3 | M3 epoch sentence claims the wrong mechanism | Corrected: epoch = stale-descent net for live pages; reuse safety = HP consumer + generation-tagged ids (HP4), decided before implementation |
| B4 | Checkpoint stall at scale never addressed | M6 item 6: measure-then-decide on `checkpoint_mu_` exclusivity + full-tree-walk base snapshot |
| B5 | Recovery RAM spike unassigned | M6 item 5: streaming recovery parse, measured at the soak's WAL-window sizes |
| B6 | M6.4 omits its hardest coupling | GC-watermark coupling in scope from day one; format change + migration rule + GC coupling ship together |
| C1 | 6 h job limit vs 24 h / 7-day soaks | Soak vehicle named in v29 M1 item 4, cited by M4's and v30 M1's acceptance |
| C2 | `dm-flakey` flaky on hosted runners | Plan B named in v30 M3: `scsi-debug` or local VM rig + published logs + reduced CI leg; fail-first acceptance kept |
| C3 | Arena noise gates will flap | Statistical protocol in v29 M1 item 4: calibrated bands, N-iteration medians, re-run policy |
| C4 | Vendored RocksDB build cost | Version-pinned + build-cached, in v29 M1 item 3 |
| D1 | Companion docs load-bearing but not in repo | M0 deliverable zero commits audit + spec under `docs/audits/`; flagged in the header and the companion list |
| D2 | Rule 2 should own 0.28.0's own violation | Incident sentence added: identity arrived at `ced3806`, ~10 behavior commits late |
| D3 | ROADMAP.md integration mechanics unspecified | Adoption mechanics in the header: old sections + progress row replaced, v26/v27 kept, CHANGELOG entry in the same commit, README links the disposition table |
| D4 | Pace assumption unstated | 12–15 months to 1.0.0 at the 5-week-arc pace, stated with the T-shirt sizes |
| E2 | Name the order-triple in M5's acceptance | **cts-order == WAL-order == phantom-order** named in M5's acceptance |
| E* | "What is right — do not dilute" | Ordering, delete-don't-rewrite, disposition table, non-claims, cts-monotonicity line, rules 9–10, 1.0.0 hold — all preserved verbatim in spirit |

---

# Appendix — the superseded 2026-09-17 plan (v26 → v27 history)

> Kept verbatim per the active plan's adoption mechanics ("the v26/v27
> history from that plan is preserved in the appendix at the end of this
> file"). This plan's **v28/v29/v30 sections were superseded on
> 2026-09-27** by the active plan above; every one of their items is
> accounted for in the active plan's Old-arc disposition table (rule 3:
> nothing is silently dropped), so those sections are not reproduced here.
> The progress-table row for them is marked SUPERSEDED in place; everything
> else below is unmodified history, including the v25.7 "load-bearing
> constraint" note the active plan's ordering rules generalize.

# ChronoKV roadmap — v26 → v30

Written 2026-09-17 against `main @ 3929d25` (v25.3), following an external code
review of that commit.

Progress:

| Milestone | Status | Shipped as |
| --- | --- | --- |
| v25.3 — review defect fixes (C1, H1, H3, H4 of the 2026-09-12 review) | **DONE** | `3929d25`, `0.25.3` |
| v26 M0 — durable rollback truncation (D2) | **DONE** | `3509586`, `0.25.4` |
| v26 M1 — adversarial fsync semantics (D3) | **DONE** | `f73d7f6`, `0.25.5` |
| v26 M2 — randomized crash-point fuzzing | **DONE** | see below, `0.25.6` |
| v25.7 — 2026-09-18 external-review defect fixes: **H1** MANIFEST ckpt_ts zeroed by size rotation (DB unopenable after checkpoint+rotation), **H2** GC busy-spin above 256 keys (one core burned idle), **M1** observer data race (TSan-confirmed); plus lifecycle hardening (streams/handles/async vs close), transaction commits now notify observers | **DONE** | see below, `0.25.7` |
| v26 M3 — online backup API (invariant B1) | **DONE** | see below, `0.25.7` |
| v25.8 — adversarial-review response: **rank-1** close()-race (plain-bool `closed_` + engine TOCTOU; v25.7's close-safety claim was false for concurrent *creation*), **rank-2** bk_* points folded into crashpt::kAll + fuzz plan grammar, **rank-3** compound fault×crash-point plans; plus a PRE-EXISTING checkpoint re-emission defect the matrix found on its first run | **DONE** | see below, `0.25.8` |
| v26 M4 — point-in-time restore | **DONE** | see below, `0.25.8` |
| v26.1 — adversarial review of `929cb00`: **rank-1** PITR mid-window `as_of` silently loses acknowledged commits (whole-delta skip × post-checkpoint WAL rotation) — fixed with per-entry delta filtering + loud failure on unprovable partial windows + in-suite detectors matching the reviewer's positive control; review hygiene notes (`restore_pitr` engine access, stale `bk_*`-not-in-`kAll` comments) | **DONE** | see below, `0.26.1` |
| v27 M1 — history → strict-serializability checker: **DONE (completed at 0.26.3** — set-shape checker, range-scan modeling, async/batch recording, mixed-API workload; start shipped at 0.26.1**)** — `txnrec::` structured recorder (runtime-armed, hooks builds) + `lincheck::` checker (cts-total-order snapshot verification, real-time order, Elle-style list-append: duplicates / fabrication / lost appends / order cycles / write-fold) + 15-case synthetic anomaly battery + engine workload with mutation battery. The checker found a LIVE anomaly on its first engine run (publication-prefix lag: acknowledged writes invisible to later snapshots while a lower cts is in flight — 109–163 instances per 4-thread run); fixed in the same release via a commit-ack publication barrier + burn-on-throw on the install tail | **DONE** | see below, `0.26.1` + `0.26.3` |
| v27 M2 — CI integration: dedicated `crashfuzz` (N seed bases via `CKV_CRASHFUZZ_SEEDS`, run-id-derived base echoed for deterministic replay) and `dst` (seeded-stress full suite × N interleaving seeds + lincheck engine workload × N seeds) jobs; bounded PR configs, long nightly schedule, both folded into `ci-passed`; the dst runner swapped to the real M0 harness at 0.26.3, job name and gate contract unchanged as planned | **DONE** | see below, `0.26.2` |
| v27 M0 — deterministic scheduler for the bug-dense paths: **DONE** — `dst::` seeded baton controller behind every `stress_point` (WAL group-commit/handoff, GC+epoch vs scans, tree cursor vs splits; 11 new points at the historical bug windows), fork-isolated 4-scenario harness with `(seed, op-count, last-point)` replay handles, bounded-patience steal as the documented deadlock breaker. Acceptance PROVEN: reintroduced C1 caught as a deadline-hang (>=3/10 seeds/run at the widened 4x8 scenario), reintroduced H3 caught as SEGV on 12/12 seeds, clean tree silent over 200-seed sweeps; CI runs 100k scenario-seeds nightly | **DONE** | see below, `0.26.3` |
| adversarial review of `6d8a13d` (v26.3), **no version bump** (maintainer directive): **MEDIUM–HIGH** residual PITR hole — a key rewritten inside the mid-window whose WAL a later checkpoint rotated away reads absent-or-older at `as_of` (silently wrong; the v26.1 "documented residual limit" ranked as a defect). Fixed per the review's direction 1/2: filtered skips over an uncovered window now fail LOUD ("not reconstructable" + remedies) instead of serving a best-effort snapshot; the v26.1 fully-rotated-window open capability is deliberately traded away (indistinguishable on disk from the hole — both reviews accept loud failure over silent loss). Direction 3 (multi-version deltas) tracked below as the sound capability-recovery path | **DONE** | see below, `0.26.3` (unbumped) |
| v27 M3 — coverage aimed at error paths: **DONE** — gcov build (`make coverage`), `CKV_COVERAGE_FAULT` forcing (per-kind, whole-run, independent of test-local arm/disarm; budget-bounded; fire counts echoed so vacuity is visible), `scripts/fault_coverage.py` publishing per-kind error-path coverage + UNIQUE per-kind contributions + the ledger of error-path lines NO run reaches; CI `coverage` job folded into `ci-passed` (PR vehicle: review gate per kind; nightly: full suite per kind + unforced headline run). First measurement already exposes the anchor's point: OpenFail/DirFsyncFail fire 0 times under the gate vehicle | **DONE** | see below, `0.27.0` |
| v28 — AUDIT REMEDIATION (docs/request.txt adversarial audit, CKV-001..021) + external-review platform defects (F1 io_uring availability, F2 nested wal_dir): **DONE** — CKV-001/002(+Blockers 1-4)/003a-c/005/006/012/018 as individual fix commits over 0.27.0; CKV-007/008(+Blocker 5)/010/011/013/014/016/019/020 + F1/F2 fixed with fail-first tests; CKV-009/015 traced-verified with contract tests (no defect); CKV-017/021 documentation corrected. Shipped as `0.28.0` | see docs/CHANGELOG.md |
| v28 M0–M4 (writer thread) / v29 / v30 | **SUPERSEDED 2026-09-27** by the active plan above | see its Old-arc disposition table |

> **v25.7 note (the load-bearing constraint, re-confirmed).** H1 and H2 were
> found by an outside reviewer *reading code and writing three-line repros* —
> again. The suite executes checkpoints, rotations and GC sweeps thousands of
> times, but its plan grammar never *composed* them (rotate always preceded
> checkpoint in the crash-fuzz `Plan`; the m2_phase3 test explicitly avoids a
> post-checkpoint rotation), and nothing asserts "an idle database consumes
> no CPU". Both detectors now ship in-suite, the crash-fuzz `Plan` gained
> `rotate_after_ckpt` (reverting the H1 fix makes the fuzzer itself fail), and
> the GC-idle test polls for quiescence instead of sleeping a fixed time so it
> stays valid under TSan/stress. This is the same signal that gates v28/v29
> on v27 — widening the *composition* of existing regimes is cheaper than the
> full DST harness and should not wait for it.

## How to read this

Every item follows the project's own stated rules, because they are good rules and
the plan is weaker without them:

- **No speculative work.** Each item is anchored to a measurement already in-tree, a
  limitation already written into README, or a defect found and confirmed.
- **Every fix ships with a test verified to fail against the unfixed code.** This is
  the discipline used for v25.3 and it is the only reason those four tests are
  trustworthy.
- **New invariants get canonical IDs**, extending the existing R1/I2/I3/I6/D1/R-REBASE/T1
  set.
- **On-disk format changes require a documented migration rule before implementation.**
  Good news: *nothing in this plan changes the WAL or checkpoint format.* The v26
  backup marker is a new file in a new directory. v29 changes only in-memory page
  lifecycle. No migrations anywhere.

Sizes are T-shirt guesses, not commitments: **S** ≈ days, **M** ≈ 1–2 weeks,
**L** ≈ 3–6 weeks, **XL** ≈ a quarter.

The findings that motivated v25.3 and the "load-bearing constraint" below are
written up in the review that accompanied that commit (defect IDs C1, H1, H2,
H3, H4, M1–M4 are referenced throughout this document and originate there).

---

## The load-bearing constraint

> **v27 must land before v28 and v29.**

v25.3's four defects were found by an outside reviewer *reading code*, not by a suite
that runs the full four-config sanitizer matrix plus fuzzing, crash matrices and
differential oracle testing. That is the real signal: **the suite cannot reproduce
races.** Both C1 and H3 were concurrency bugs in paths the suite executes thousands of
times without ever hitting the bad interleaving.

v28 (removing the leader handshake) and v29 (leaf merge/rebalance) both rewrite the
most concurrency-sensitive code in the file. Doing either before the reproduction
problem is fixed would be reckless — you would be validating a rewrite of your race
hazard with a harness that cannot find races.

Everything else can interleave.

```
v26 durability + backup ──┐
                          ├──> v27 reproducibility ──> v28 WAL writer thread
v30 ergonomics (any time) ┘                        └─> v29 page reclamation
```

---

# v26 — Durability correctness + online backup

**Status:** M0 **DONE** (`3509586`, `0.25.4`) · M1 **DONE** (`f73d7f6`, `0.25.5`) · M2 **DONE** (`b793486`, `0.25.6`) · M3 **DONE** (`0.25.7`, see below) · M4 **DONE** (`0.25.8`, see below). Post-arc patch `0.26.1`: the adversarial review of `929cb00` found one HIGH correctness defect in M4's PITR (mid-window `as_of` × WAL rotation ⇒ silent loss of acknowledged commits); fixed with per-entry delta filtering, a loud failure for unprovable partial windows, published-watermark coverage of delta-only versions, and in-suite detectors (the reviewer's positive control now passes and was verified to fail pre-fix). See chronokv.hpp's v26.1 header block and README "Safety properties".
Post-arc review of `6d8a13d` (no bump): the v26.1 "documented residual limit" (mid-window REWRITE x fully-rotated WAL => absent-or-older at `as_of`) was ranked MEDIUM–HIGH — documented-and-silent is still silently wrong for a restore-to-`as_of` API. Policy refined to conservative loud failure: any filtered skip (delta entry `hc > as_of`) over a window the surviving WAL does not cover throws "not reconstructable" with remedies (boundary `as_of`, covering `backup()`, retain WAL). Non-destructive; `restore_pitr` propagates; surviving-window mid-cuts and boundary opens unchanged; in-suite detectors flipped/added (block 13 = the reviewer's repro, verified silent pre-fix). **Future work (review direction 3): multi-version deltas** — retain every `(key, cts, value)` version of the checkpoint window in the delta instead of only each key's newest, making mid-window `as_of` soundly reconstructable even after rotation and restoring the v26.1 capability. Requires a delta format version bump and a GC interaction decision (window versions must survive anchor-severing until the next checkpoint); candidate for the v29/v30 durability arc or a standalone patch.

Theme: the durability path has a confirmed live defect, and the backup API is cheap
because both hard primitives already exist. Small, coherent, ships fast.

## M0 — Fix the non-durable rollback truncation  **[S] [DEFECT] — DONE, shipped as 0.25.4**

**Anchor.** Confirmed by inspection at `chronokv.hpp:2821`. After a durability failure
the leader calls `ftruncate(active_fd_, batch_start)` to remove the failed batch, but
**nothing fsyncs that truncation**. The rollback exists only in the page cache, so a
power loss immediately after a `WalFailure` can restore the batch — precisely the
resurrection the truncation exists to prevent.

The existing `fault-inj: WAL fsync fail -> no resurrection on restart` test passes
because it does a clean process restart, not a crash. Clean restart flushes; power loss
does not.

**Work.**
1. fsync (or fdatasync — it persists file-size changes) the fd after a successful
   `ftruncate` on the failure path.
2. Treat failure of *that* fsync as fail-stop, not a `std::cerr` warning. If the
   rollback cannot be made durable, the WAL instance cannot make any promise about what
   it contains.
3. ~~Decrement `active_segment_bytes_` … every failed batch inflates the counter.~~
   **This claim was wrong — corrected during implementation.** The increment
   `active_segment_bytes_ += batch_bytes` is guarded by `if (io_ok)`, so a failed batch
   never inflated the counter in the first place. What shipped instead is *drift
   correction*: re-sync the tracked size to the post-truncate file size, which matters
   when a partial write grew the file without `io_ok` ever being set.

   Likewise, item 2's "fail-stop" turned out to need no new code: the truncation block is
   only reachable when `io_ok == false`, and the leader already sets `failed_` for
   `!io_ok` a few lines later. So the log level was raised WARNING → FATAL and the
   counter bumped, but no new fail-stop path was added. Recording both corrections here
   because the plan was wrong and silently leaving it wrong would mislead the next
   reader.

**New invariant.**
> **D2** — a batch for which any caller observed `WalFailure` is absent from the WAL
> after *any* crash, not merely after a clean restart.

**Acceptance — and an honest limit on it.** True power loss is **not** testable in CI:
`_exit()` and `kill -9` do not discard the kernel page cache, so a process-death test
cannot distinguish "fsynced" from "still dirty". Simulating real power loss needs
hardware or a fault-injecting block layer (`dm-flakey`).

What shipped instead:
- **D2a (the detector)** — arm `FsyncFail` for *two* charges; the batch's own fsync
  consumes one, so the second can only be consumed by an fsync issued during the
  rollback. Assert `fault::remaining == 0`. Plus guards that the segment really shrank
  and that `active_segment_bytes_` matches the real file size.
  **Verified against reverted code: FAILS with "1 FsyncFail charge(s) left armed".**
- **D2b (regression guard)** — fork, commit `a` durably, fail `b`, `_exit(1)` with no
  destructors, reopen and assert `a` present / `b` absent.
  **Passes pre-fix**, which is exactly why it is labelled a guard and not a detector —
  it documents the limit above rather than pretending to close it.

## M1 — Adversarial fsync semantics  **[M] [HIGHEST VALUE] — DONE**

> ### What the audit changed about this milestone
>
> The plan below was written before the fsync call sites were audited. Three of its
> premises were wrong, and the audit found two defects the plan did not anticipate.
> Recorded here rather than quietly rewritten, because the corrections are more
> useful than the original plan.
>
> **1. The "policy change needing sign-off" does not exist.** The plan called for making
> fsync errors fatal to the WAL instance and flagged it as a behaviour change requiring
> approval. It already works that way: `failed_` latches and is never cleared anywhere;
> `commit_txn` short-circuits to `TxnResult::DatabaseFailed` (→ `Status::Failed`) once
> `wal_->is_failed()`; and `health()` reports level 2 with reason `"wal fail-stop mode
> active"`. Reads keep working, writes are refused, and the state is observable through
> the public API. So M1 needed *consistency*, not a policy change. Asking for sign-off
> was a mistake born from planning before reading.
>
> The `DatabaseFailed` vs `WalFailure` distinction is deliberate and worth preserving:
> `WalFailure` means "this write failed", `Failed` means "this instance is done, stop
> trying". The first draft of test D3d asserted `WalFailure` for post-latch writes and
> was simply wrong; the comment in the test now says so, to stop someone "fixing" it back.
>
> **2. New defect found — `maybe_rotate_segment()` discarded four durability results.**
> Not in the plan. The size-based rotation path (the one that runs every 64 MiB) ignored
> the old segment's pre-close fsync, the new segment's fsync, `write_manifest()`'s return
> and `fsync_dir()`. `rotate_after_checkpoint()` checks all four correctly, so the right
> pattern already existed in the same file — this path just didn't follow it.
>
> The consequence is *not* silent corruption: `recover_all()` rejects the resulting state
> loudly. A non-durable MANIFEST leaves it naming segment N while N+1 exists → `"orphan
> WAL segment"`; a non-durable directory entry leaves the MANIFEST naming N+1 with no
> file → `"missing WAL segment"`. Either way the database will not open, and the only
> practical remedy is deleting the offending segment — which discards writes already
> acknowledged durable. Now fail-stops at the moment of failure, while the on-disk state
> is still self-consistent.
>
> **3. New defect found — fsync fault injection was inert on the io_uring path.** When
> `used_iouring` is true the chain runs `IORING_OP_FSYNC` itself, so `checked_fsync()` —
> where fault kinds are injected — is never called. That branch checked `FsyncFail` only,
> so **any other fsync fault kind silently did nothing on every modern kernel**. No
> existing test was affected (they all use `FsyncFail`), but it is exactly the trap that
> makes a test suite pass vacuously. Now every fsync fault kind is honoured on both paths.
>
> **4. Two planned fault kinds were NOT implemented.**
> - `FsyncSilentLoss` (fsync returns 0, data never persisted) is **untestable in-process**
>   for the same reason M0's power loss is: nothing in a user process can discard the
>   kernel page cache. Adding the kind would create a knob that tests nothing and invites
>   false confidence. Recorded as a coverage gap requiring `dm-flakey` or real hardware.
> - `PartialWrite` was dropped as redundant: `write_all()` already loops on short writes,
>   `Kind::WriteShort` already exercises that, torn-tail recovery already handles a lost
>   remainder, and v25.2 requires the full write length on the io_uring CQE.
>
> **What actually shipped:** the `maybe_rotate_segment` fail-stop fix; the io_uring
> fault-injection fix; `Kind::FsyncFailAfterPersist` (the data IS durable but the call
> reported an error — the other half of fsyncgate, and the case that tests whether the
> rollback truncation prevents "told-failure-but-actually-durable"); tests D3a–D3e; and a
> correction to a stale `recover_all()` comment claiming MANIFEST is written only by
> `rotate_after_checkpoint()`.
>
> **Verified against reverted code:** D3a, D3b and D3c all FAIL when the rotation fix is
> reverted. D3d and D3e pass either way, and are labelled a guard and a new-capability
> test respectively rather than being passed off as detectors.
>
> ---
>
> *Original plan, preserved for the record:*

**Anchor.** `checked_fsync()` retries on `EINTR` and returns `false` otherwise; the
leader then truncates, returns `WalFailure`, and *keeps running*. That policy is unsafe
under real kernel behaviour.

This is the **fsyncgate** class. On Linux, when writeback fails the page cache may
already have dropped the dirty pages. A later `fsync` on the same fd can then return
**success** while the data is permanently gone. Retrying or continuing after an fsync
error can therefore convert a loud failure into silent data loss. This is the bug class
that produced real defects in PostgreSQL (which now PANICs on fsync failure), MySQL,
etcd and others.

For a single-node embedded store this — not network partitions — is the equivalent of
what Jepsen hunts.

**Work.**
1. New fault kinds, so the semantics are testable rather than assumed:
   - `FsyncSilentLoss` — returns 0, data never persisted.
   - `FsyncFailAfterPersist` — returns `EIO`, data *is* on disk.
   - `FsyncErrorNotSticky` — first call `EIO`, later call succeeds (the Postgres case).
   - `PartialWrite` — `write`/`pwrite` returns short and the remainder is lost rather
     than retried.
2. **Policy change (needs your sign-off — it is a behaviour change):** an fsync error
   becomes **fatal to the WAL instance**. Set `failed_ = true`, return `WalFailure`,
   and refuse further writes until the process restarts and recovery runs. Rationale:
   after an fsync error you cannot know what is durable, so no further promise is
   honest. This matches PostgreSQL's post-fsyncgate stance.
3. Audit every `checked_fsync` call site against the new policy: WAL batch, segment
   rotation, MANIFEST write, checkpoint tmp+rename, `fsync_dir`, and the M0 truncation.
   Each must either be fatal or be provably idempotent-and-recoverable.

**New invariant.**
> **D3** — once any fsync on the WAL or checkpoint path returns an error, the instance
> makes no further durability promise and accepts no further writes.
>
> *Status: already held on the main WAL path before M1 (see correction 1). M1 extended it
> to the size-based rotation path, which was the one place that violated it.*

**Acceptance.** A matrix of {fault kind} × {crash point} × {Sync, Group, Async} asserting
jointly: no resurrection (D2), no silent loss of an acknowledged-durable record, no LSN
gap or duplicate, and `verify_wal_dir()` clean after recovery. Every cell must be
exercised — publish the matrix as coverage, not just as a pass count.

## M2 — Randomized crash-point fuzzing  **[M] — DONE, shipped as 0.25.6**

> ### It found a bug on the first run
>
> **An interrupted segment rotation left the database permanently unopenable.**
> `maybe_rotate_segment()` creates the new segment *before* it can make the MANIFEST
> durable, so a crash in that window leaves MANIFEST naming `N` while `N+1` exists.
> `recover_all()` rejected any segment above `active_id` as an "orphan" and threw — so a
> **routine** crash during a size-based rotation (one happens every 64 MiB) bricked the
> database until someone manually deleted the file.
>
> Found by killing at `man_after_tmp_write`. Scene preserved by the fuzzer:
> `MANIFEST active_id=2`, `wal_000001.log` 287 bytes, `wal_000002.log` 0 bytes,
> `wal_000003.log` 0 bytes, plus a stale `MANIFEST.tmp`. All 7 ledgered writes were safe
> in segment 1 — so no data loss, purely an availability defect, but a total one.
>
> Fix: tolerate an orphan that is **empty and at exactly `active_id+1`**. Provably safe —
> rotation completes before any batch is written to the new segment, so a segment the
> MANIFEST does not yet name cannot hold acknowledged data. It is *ignored*, not adopted:
> `active_id` stays authoritative. The tolerance is deliberately narrow — a **non-empty**
> orphan still throws (a MANIFEST rename that landed without its directory fsync can
> revert while the newer segment holds acknowledged writes), and a **non-contiguous**
> orphan still throws (no single interrupted rotation can produce one).
>
> Verified: reverting the tolerance gives 2 violations and a FAIL; with it, 0 violations
> across all 20 points.
>
> ### The anti-vacuity check paid for itself twice
>
> The fuzzer asserts that *every* instrumented point is actually reached, because an
> unreached point contributes nothing and looks identical to a pass — the same trap as the
> io_uring fault-injection gap found in M1. That assertion failed on the first two runs and
> caught two bugs **in the fuzzer itself**:
>
> 1. Occurrence index was fully random, so a point executing once per run was missed ~2/3
>    of the time. Round 0 now always uses occurrence 0; later rounds randomise it.
> 2. Workload plans were fully random, so rotation-only and checkpoint-only points were
>    skipped whenever the seeded plan did not rotate or checkpoint. Plans are now forced
>    to the regimes the target point needs.
>
> First run reported 6 of 20 points never hit. After both fixes: 20 of 20, every run.
> Without the coverage assertion this would have shipped as a fuzzer silently testing 70%
> of what it claimed.
>
> ### Deviations from the plan
>
> - The plan said "extend `stress_point()` into kill points". **Not possible** — it compiles
>   to a no-op unless `CHRONOKV_STRESS` is defined, so it is inert in the release/asan/tsan
>   builds. A separate `crashpt` mechanism gated on `CHRONOKV_FAULT_INJECTION` was added.
> - The plan targeted 10k crash points per CI run. The in-suite default is 240 (12 rounds ×
>   20 points, ~10 s) to keep the always-run suite fast; `CKV_CRASHFUZZ_ROUNDS=500` gives
>   the 10k figure (~7 min at -O0 on 2 CPUs) for nightly. Round 0 alone guarantees full
>   boundary coverage, so the default is not merely a sample.
> - The fuzzer drives the **public `chronokv::Database` API**, not the engine directly, so
>   it also covers the wrapper paths a real embedder uses.
>
> ---
>
> *Original plan, preserved for the record:*

**Anchor.** Your own v20 M3 note: *"The 6-boundary crash matrix from the original plan
was NOT built."* Twelve versions later it still isn't. Hand-picked crash boundaries
only find the bugs you thought to imagine.

**Work.**
1. Extend `stress_point()` into kill points: at a seeded subset of instrumented
   locations, `_exit(1)` immediately (fork first, so the parent can recover and assert).
   Instrument the whole durability state machine: WAL append, batch write, fsync,
   truncation, segment rotation, MANIFEST write, checkpoint tmp write, fsync, rename,
   dir fsync, delta delete, rebase.
2. Build a **durable-acknowledgement ledger**: a sidecar file (outside the DB) recording
   every commit that returned `Committed` under Sync or Group. This is what makes
   no-loss assertable — without it a crash test can only prove "it didn't corrupt," not
   "it didn't lose an acknowledged write."
3. Post-crash verifier asserts all of: WAL parses; no LSN gap/duplicate; checkpoint
   chain valid; recovered state ⊇ ledger (no loss); recovered state contains no record
   that was `WalFailure`d (no resurrection, D2); `verify_publication()` and
   `verify_checkpoint_chain()` clean.
4. Seeded, so any failure reproduces from one number.

**Acceptance.** 10k crash points per CI run in release config; 100k nightly. Zero
violations. Then: **deliberately reintroduce the M0 defect on a branch and confirm the
fuzzer finds it within N seeds.** That is the only honest proof the fuzzer works — the
same standard applied to the v25.3 tests.

## M3 — Online backup API  **[M]** — DONE, shipped in `0.25.7`

> ### What actually shipped
>
> `Database::backup(dest_dir)` and static `Database::verify_backup(dest_dir,
> reason*)`, implemented as planned: exclusive `checkpoint_mu_` hold →
> checkpoint (→ rotation) → copy MANIFEST-referenced segments (id ≤
> active_id) + MANIFEST + checkpoint base/deltas → fsync every file and both
> directories → `BACKUP_COMPLETE` written LAST (magic `BKM1`, CRC32 over the
> payload; payload = cts, active_id, checkpoint basename, and
> {name, size, crc32} per copied file). `verify_backup` re-checksums every
> file, runs `verify_checkpoint_chain`, and does a full read-only
> `recover_all()` parse of the copied WAL — no Database instance, so it can
> never collide with the flock guard. Restore is the existing recovery path
> pointed at the copy. Works for WAL-less (checkpoint-only) databases too.
>
> **Dependency discovered during implementation:** backup *requires* the
> v25.7 H1 fix. A backup taken after any size-based rotation copies a
> MANIFEST whose `ckpt_ts` must have survived the rotation; with the old
> `ckpt_ts=0` behaviour, `verify_backup`'s own `recover_all()` would reject
> every legitimate backup of a checkpointed database ("missing WAL segment").
>
> **Tests (all shipping, all green in the four-config matrix):** round-trip
> differential over a multi-segment + delta-chain source (oracle diff over
> every write acknowledged before `backup()` returned — B1's "contains"
> half — plus a no-over-copy assertion for a write committed after);
> corrupted-file / truncated-marker / missing-marker rejection; interrupted
> backup via fork + kill at EACH of six `bk_*` crash points with
> per-point reachability assertions (exit code 97) — `bk_marker_after_rename`
> is asserted to be ACCEPTED, since after the marker rename the copy is
> complete for process-crash purposes (only power loss could revert it —
> the same semantics the MANIFEST rename relies on everywhere else);
> backup concurrent with active writers (pre-backup acknowledged prefix
> present in the restore); no-WAL backup round-trip. A public-API subset
> runs in the hooks-off smoke build.
>
> **Deviations from the plan (recorded, not hidden):**
> 1. `bk_*` crash points are deliberately NOT added to `crashpt::kAll`: the
>    fuzzer's plan grammar has no backup regime, and the dedicated
>    interrupted-backup test arms every point exactly once with an
>    anti-vacuity assertion — a stronger per-point guarantee than seeded
>    sampling. Folding a backup regime into the fuzzer is a legitimate
>    follow-up if backup grows more state machine.
> 2. The round-trip differential uses a `std::map` oracle rather than
>    `SerialOracle`: the workload is sequential and deterministic, for which
>    the two are equivalent. The concurrent-writer case asserts the property
>    B1 actually promises (the acknowledged prefix), not full equivalence.
> 3. Because commits are blocked for the whole copy, the documented window
>    "at least as new as the checkpoint, at most as new as copy completion"
>    collapses in practice to exactly the checkpoint cts. Documented as such
>    in the header; still NOT PITR (M4).
>
> **B1 status:** the "only-if" half is mechanically enforced (marker absent
> or any mismatch → reject; verified by the corruption/interruption tests).
> The "if" half rests on the exclusive-lock freeze + checkpoint-before-copy
> ordering, and is asserted by the ledger-style round-trip and
> concurrent-writer tests.

**Anchor.** "Online" = while running. No server, no network: it copies a consistent
snapshot to another directory (local path, external drive, NAS mount — anything the
filesystem can see). Restore is *already implemented*: point `Options` at the copy with
`recover_on_open`.

**Work.**
1. `Database::backup(dest_dir)` — hold `checkpoint_mu_` exclusively for the duration so
   no concurrent checkpoint or rebase can swap the base file mid-copy; then checkpoint →
   rotate (via the existing `rotate_after_checkpoint`) → copy only **immutable** segments
   + MANIFEST + checkpoint base/deltas → fsync each file and the directory → write
   `BACKUP_COMPLETE` **last**.
2. `BACKUP_COMPLETE` carries the cts boundary, `active_id`, the segment list, and a
   checksum per copied file. A backup interrupted mid-copy is then *detectable* rather
   than silently plausible. (Directory rename is atomic only within one filesystem; the
   marker works across filesystems, which is the common case for backups.)
3. `Database::verify_backup(dest_dir)` — checks the marker, re-checksums, validates the
   checkpoint chain and parses every segment without opening the DB (so it cannot
   collide with the `flock` guard).
4. Document the semantics precisely: **at least as new as the checkpoint, at most as new
   as copy completion.** Not a point-in-time snapshot. Anyone assuming PITR from the
   name will be wrong.

**New invariant.**
> **B1** — `verify_backup(dir)` succeeds if and only if `dir` restores to a state
> containing every record acknowledged durable before `backup()` returned.

**Acceptance.** Round-trip differential against `SerialOracle`; interrupted-backup test
(fork + kill at *each* copy step → `verify_backup` must reject); backup of a multi-segment
DB with deltas; backup concurrent with active writers; restore into a fresh instance and
diff against the source.

## M4 (stretch) — Point-in-time restore  **[S]** — DONE, shipped in `0.25.8`

> ### What actually shipped
>
> `Options::pitr_as_of_cts` (read-only as-of open) +
> `Database::restore_pitr(src_wal_dir, src_ckpt_path, dest_dir, as_of_cts,
> dest_opts)` (writable materialization into a fresh directory) +
> `Database::backup_cts(dir)` (marker boundary reader). Recovery replays the
> chain and WAL up to the boundary: future deltas are skipped via a new
> `peek_ckpt_cts()` header read and LEFT ON DISK, future WAL records break
> replay and stay untouched — a PITR open is non-destructive by construction
> (asserted: a normal reopen after a PITR open still recovers the full
> timeline). `as_of` below the base's cts fails loud. The PITR instance is
> read-only (writes `Status::Failed`, `checkpoint()` throws, `health()`
> level 1 names the mode); `restore_pitr` exports the state via
> `export_checkpoint_no_rotate()` — the source WAL/MANIFEST are never
> rewritten — and returns the fresh directory open and writable.
>
> **The plan was wrong about one thing (recorded per house rules).** The
> sketch said `restore(backup, as_of_cts)`. But `backup()` ALWAYS checkpoints
> first, so every artifact in a backup sits at its boundary cts: PITR
> *inside* a `backup()` copy is vacuous by construction (`as_of >= cts` =
> normal restore; `as_of < cts` = rejected, since the base superseded older
> state). **[v28 correction, audit CKV-003c]** the "rejected" half was
> subtler than recorded: the backup's incremental checkpoint silently
> dropped the source's recovered WAL tail (recovery never re-marked it
> dirty), so `as_of < cts` failed loud via the unreconstructable-window
> detection — while losing data on any post-reopen checkpoint. With the
> tail properly captured (recovery dirty re-marking), the backup chain
> provably covers the window and `as_of < cts` materializes the EXACT
> as-of state via v26.1 per-entry delta filtering; below the chain's BASE
> cts remains a deterministic loud rejection. The boundary test now
> asserts the safety contract: rejected OR exact-state, never wrong-state.
> The real PITR window is a LIVE/CRASHED database directory (or an
> external file-level copy of one), where the WAL extends past the last
> checkpoint — which is what shipped. `backup_cts()` documents the boundary
> a copy restores to.
>
> **Why materialize-fresh instead of write-in-place:** appending to a WAL
> that still holds records newer than the boundary would collide on cts at
> the next recovery (duplicate commit_ts). Exporting to a fresh directory is
> the only option that keeps both the source and the restored instance
> honest. Consequence: PITR does not rewind the source; it forks it.
>
> **Tests:** WAL-mid cut; delta-chain cut + delta-left-on-disk + full-state
> normal reopen; below-base loud failure; beyond-tail equivalence;
> restore_pitr writability + persistence + source-untouched; backup-boundary
> semantics (at-cts full state, below-cts rejected); argument validation;
> public-API subset in hooks-off smoke (Test 19).
>
> **Bonus defect found while building it** (via the v25.8 rank-3 compound
> fault matrix, first run): the checkpoint re-emission brick — a checkpoint
> failing after its delta rename left `dirty_since_ckpt_` uncleaned, the next
> checkpoint re-emitted identical (key, commit_ts) entries into a second
> delta, and recovery's strictly-increasing guard rejected the database
> forever. Pre-existing since v18-style delta chains; fixed by treating
> equal (key, cts) as the idempotent re-emission it is (skip; strictly
> decreasing still throws). Fuzzer-preserved scene confirmed byte-exact.

---

# v27 — Make concurrency bugs reproducible

**Status: COMPLETE (0.26.1 → 0.27.0).** M1 start `0.26.1` · M2 `0.26.2` ·
M0 + M1 completion `0.26.3` · M3 `0.27.0`. The DST harness is green in CI
(100k seeds nightly), so v28's gate condition is met.

Theme: fix the reproduction problem before touching concurrent code again.

## M0 — Deterministic scheduler for the bug-dense paths  **[L]**

**Anchor.** C1 and H3 were both found by reading, not by running. `CHRONOKV_STRESS`
already has a seeded PRNG and `stress_point()`, but the yields are real
`std::this_thread::yield()` — so an interleaving you hit once cannot be replayed.

**Work.** Replace non-deterministic yields with a seeded, centrally-controlled scheduler
at instrumentation sites: each worker calls `sched::point(id)`, a controller picks the
next runnable thread from the seeded PRNG. A failure then reproduces from `(seed,
op-count)`.

Scope it to the three areas where the bugs actually were, not globally:
- the WAL group-commit / batch handoff path,
- GC + epoch reclamation vs. concurrent scans,
- B+ tree cursor vs. concurrent splits.

Full global DST is the FoundationDB end-state and is not worth it yet.

**Acceptance.** Deliberately reintroduce C1 and H3 on a branch; the harness must find
both, deterministically, within a bounded seed count. Then run N=100k seeds in CI.

> ### What shipped at 0.26.3 (M0)
>
> **Scheduler — `dst::` (chronokv.hpp, CHRONOKV_STRESS builds, runtime-
> armed).** Exactly the plan's shape: each instrumentation site calls
> `sched::point(id)` — here, the EXISTING `stress_point(name)` sites route
> to `dst::point(name)` while the controller is armed (unarmed they keep
> the old seeded 1/8-yield behavior, so the standing stress suite is
> untouched), and a central controller picks the next runnable thread from
> the seeded PRNG. Baton semantics: one runner at a time; reaching the
> next point hands the baton back. Two additions reality forced:
> (1) RENDEZVOUS — the first grant waits until all N scenario threads have
> parked, so even the initial schedule is a pure function of the seed
> (thread-spawn timing cannot leak in); (2) BOUNDED-PATIENCE STEAL — the
> engine blocks on real mutexes/cvs we do not intercept (group-commit
> followers park on `batch_cv_` BY DESIGN), so if the baton makes no
> progress for 5 ms any parked thread steals it; steals are COUNTED in the
> shared progress page. Clean scenarios that never block-behind-a-parked-
> holder run strictly serialized; a stealing run is replayable from
> `(seed, op-count)` up to the logged steal points — the honest limit of
> retrofit DST on lock-based code, documented rather than papered over.
>
> **Instrumentation — 11 new points at the windows the bugs actually lived
> in** (on top of the 9 existing sites, which now double as scheduling
> points): `wal_mixed_handoff` (the exact line the H3 orphaning happened
> on), `wal_leader_elected` / `wal_leader_done` (leader I/O window and the
> C1 cleanup/state-flip window), `gc_pass_begin` / `gc_epoch_advance` /
> `gc_reclaim_begin` + `scan_guard_acquired` (the pin-vs-reclaim pair),
> `tree_leaf_latch_gap` (BOTH put and erase descents: shared-release ->
> exclusive-acquire), `tree_split_begin`, `tree_cursor_leaf_switch`
> (latch released, successor not yet latched).
>
> **Harness — `run_dst_test()` (main.cpp, `CKV_ONLY_DST` gate; in-suite on
> every stress run).** Four fork-isolated scenarios: `wal_mixed_handoff`
> (H3 class: durability flipper + 3 committers, every commit must land),
> `wal_leader_fault` (C1 class: forced rotation + armed `SegOpenFail` +
> 4x8 concurrent puts — resurrects the variant the suite DROPPED because
> it wedged ~Database pre-fix; fork isolation turns the wedge into a
> deadline FAIL), `gc_epoch_scan` (churn + synchronous passes + scans with
> a written-value legitimacy oracle), `tree_cursor_split` (400 ascending
> inserts vs contiguous-prefix scan validation). Parent classifies each
> seed: exit / signal / deadline, and every failure prints the replay
> handle: `(scenario, seed, ops, last_point, steals)` + a copy-pasteable
> `CKV_DST_SEED=... CKV_DST_SEEDS=1` command. Progress lives in a shared
> mmap page, so even SIGKILLed hangs report their op-count.
>
> **Acceptance — PROVEN on scratch trees (not pushed):** reintroducing C1
> (unconditional `lk.lock()` -> EDEADLK skips the cleanup) makes
> `wal_leader_fault` HANG; caught in every control run at >=3/10 seeds
> (per-seed catch ~0.6 at the widened 4x8 shape — the hang needs a
> follower in-flight against the doomed batch, since post-failstop puts
> legitimately short-circuit; at the CI PR count of 100 seeds/scenario the
> miss probability is ~1e-14). Reintroducing H3 (leader drains `cur_batch_`
> with no null guard) SEGVs on **12/12 seeds across 4 control runs**,
> signal 11, matching the historical signature. Clean tree: 200-seed
> sweeps, zero failures, ~70 ms/seed at -O0. The harness additionally
> caught its OWN S3 oracle bug on the first run ever (per-key version
> monotonicity is false under a shared seq counter — snapshot order
> follows commit cts, not reservation order): a detector that has never
> failed is not a detector, and this one failed first.
>
> **CI:** the `dst` job (name and gate contract preserved from M2, as
> planned) now runs the real harness: 100 seeds/scenario on push/PR,
> 25,000 x 4 scenarios = the roadmap's **N=100k** nightly.
>
> **v28's gate condition ("do not start before the DST harness is green")
> is satisfied for M0**; v27 M3 (coverage) remains open.

## M1 — History → strict-serializability checker  **[M]**

**Anchor.** `history::` recorder and `SerialOracle` both exist; nothing checks a
*recorded* history. You log anomalies instead of detecting them.

**Work.**
1. Exploit a property you already have: **cts is a total, monotonic counter.** So strict
   serializability does not need general Knossos-style search — take cts order as the
   candidate total order and verify (a) each transaction read exactly the state as-of its
   snapshot cts, and (b) real-time order is respected. That is sound here *because* cts
   is total, and it is orders of magnitude cheaper than search.
2. Add an **Elle-style list-append / set checker** for adversarial workloads: write
   unique tokens, read back sets, detect lost updates, fractured reads and duplicates.
   This catches anomalies the cts-order check can miss.

**Acceptance.** The checker must flag a deliberately injected anomaly (e.g. a snapshot
violation, a lost update) in a synthetic history. A checker that has never failed is not
a checker.

> ### What shipped at 0.26.1 (M1 START)
>
> **Recorder — `txnrec::` (chronokv.hpp, `CHRONOKV_TEST_HOOKS`, runtime-armed).**
> `history::` is a human-readable text dump behind a compile flag no CI build
> sets. `txnrec` is the machine-readable feed: structured per-op records
> (snapshot cts, observed version cts, assigned commit cts, committed flag,
> written value, steady-ns real-time interval) stamped at the OUTERMOST public
> wrappers (`Database::get/put/erase/begin`, `Transaction::get/put/erase/commit/abort`)
> so recorded intervals over-approximate true call intervals — the sound
> direction for real-time checks. Disarmed cost is one relaxed atomic load, so
> it ships in every hooks build. Not recorded (documented): async/batch APIs,
> range scans, engine-internal `ReadWriteTransaction` use.
>
> **Checker — `lincheck::` (main.cpp).** Exactly the plan's two items:
> (1) cts-total-order verification — every read must equal the state produced
> by replaying committed writes with `cts <= snapshot` in cts order
> (read-your-writes overlay included), no read may observe a version newer
> than its snapshot, committed writers must have unique cts above their own
> snapshot, and real-time edges (ack-before-begin, with a 4 µs slack absorbing
> in-library timestamping error) must respect cts order and snapshot freshness;
> (2) Elle-style list-append analysis — per-key duplicate/fabricated-token
> detection, canonical-prefix checks against append cts (lost updates),
> a cts-INDEPENDENT precedence-cycle check (fractured reads), and a
> committed-write fold check (each append must extend the prior committed
> value by exactly its last token). No search is needed: cts is total, per the
> plan's own argument.
>
> **Acceptance met:** `run_lincheck_test()` fires all thirteen synthetic
> anomaly kinds (each asserted flagged, clean baseline asserted silent), runs
> a 4-thread list-append + reader engine workload (≈650 txns) with ZERO
> violations, and re-flags four deliberate mutations of the RECORDED engine
> history (dropped token, duplicated token, cts swap across a real-time edge,
> version cts beyond snapshot).
>
> **First live catch (and fix): the publication-prefix lag.** On its first
> engine run the checker reported 109–163 `stale-start` violations: cts is
> reserved in push order under `batch_mu_`, but `pub_.complete(cts)` runs
> per-thread AFTER the shared WAL pass, so completion order can invert
> reservation order; `published_` is a contiguous prefix, so an acknowledged
> write could sit above an in-flight lower cts — and a reader beginning after
> the ack would snapshot below it. Real-time order violation (SSI does not
> promise read freshness; the roadmap's M1 target model — strict
> serializability — does). Fix shipped in the same release: `commit_txn`
> awaits `published >= cts` before acknowledging (bounded wait: every lower
> cts is already past its WAL I/O), and the install tail burns the cts on
> throw (previously a `link_version`/dirty-map throw stalled the prefix
> forever — silent permanent staleness; with the barrier it would have been a
> hang). Positive control verified: removing the one barrier line reproduces
> the failures (163 violations); with it, zero.
>
> The full-suite verification of the barrier then found the seeding hole:
> append-only opens (`recover_on_open=false`, and direct engine construction
> over a non-empty `wal_dir`) seed `clock_` from the WAL (v24 Fix 5) but did
> not seed `published_`, so the first commit's barrier awaited cts 1..N that
> this instance will never complete or burn — a hang (the v20.1-#7 test
> wedged the suite). Fixed by seeding the published prefix alongside the
> clock (`recover_with_checkpoint` still overwrites it with the exact
> contiguous value — possibly lower — on any full recovery); ships with a
> hard-timeout regression test on a worker thread (verified FAIL pre-fix,
> no wedge: the timeout turns the hang into a report).
>
> **M1 is COMPLETE (0.26.3).** All four leftovers shipped: N-seed scaling
> at 0.26.2 (`CKV_LINCHECK_SEEDS`), and at 0.26.3: (1) the set-checker
> shape — `check_set_adds`: order-INSENSITIVE set algebra over read-back
> sets (duplicate / fabricated / snapshot-mismatch vs the canonical
> committed set / write-fold / add-duplicate / cts-independent realtime
> loss), with the defining asymmetry asserted in-suite: a permuted token
> order passes `check_set_adds` and FAILS `check_list_append`; (2)
> range-scan modeling — `Database::range_scan` and transactional scans
> record the engine's PRE-overlay snapshot view (`k\x1Fv\x1E` wire
> format, `effective_ts` as the snapshot; RWT scans attach to the parent
> txn — standalone attribution made correctly-consistent scans read as
> stale-start, caught by the mixed workload on its first run);
> `check_scans` demands the scan equal the EXACT live key set in
> `[lo,hi]` at its snapshot (phantom / missing / stale / bounds /
> duplicate), so the SSI no-phantom claim is now machine-checked on real
> histories; (3) async recording — interval `[API entry, shared-state
> ready]` with `ack_deferred` semantics: the end is DROPPED for real-time
> ack edges (the caller's `future.get()` ack is unobservable in-library;
> deriving edges from readiness would be unsound), begin-side freshness
> and all cts checks still apply; (4) batch recording — `Batch::commit`
> records Stage*+Commit under one id (synchronous: full interval
> soundness, participates in real-time edges — proven by the batch-cts-
> swap mutation). The new mixed-API engine workload (sets + async +
> batch + standalone & transactional scans, seeded) runs all four
> checkers with non-vacuity guards per API family; batteries grew by 6
> scan cases, 9 set cases and 4 mixed-history mutations.
> `RangeScanStream` remains unrecorded (lazy multi-call iteration has no
> single sound interval — documented).

## M2 — CI integration  **[S]**

New jobs: `dst` (N seeds) and `crashfuzz` (from v26 M2). Bounded runtime in PR CI, long
runs nightly. Fold both into the existing `ci-passed` aggregate gate.

> ### What shipped at 0.26.2 (M2)
>
> Both jobs exist, fold into `ci-passed`, and follow the bounded-PR /
> long-nightly policy via a new nightly schedule (03:17 UTC; manual
> dispatch also gets the long config, and the concurrency group changed so
> a push can no longer cancel a nightly/dispatch run mid-flight).
>
> **`crashfuzz`** runs the v26 M2 regime alone behind a new
> `CKV_ONLY_CRASHFUZZ` gate, and scales by SEEDS rather than rounds alone:
> `CKV_CRASHFUZZ_SEEDS=N` sweeps N bases (golden-ratio-prime step from
> `CKV_CRASHFUZZ_SEED`). PR: 2 bases × 24 rounds (1,248 plans); nightly:
> 8 × 120 (24,960 plans). In CI the base derives from `github.run_id` —
> every run explores fresh plan space — and the binary echoes the config
> and every base, so a failure replays deterministically from the log
> alone (`CKV_CRASHFUZZ_SEED/SEEDS/ROUNDS`). Preserved failure scenes
> (`/tmp/ckv_cf_*`, first three violations) upload as artifacts.
>
> **`dst`** — with the honest caveat the plan implies: the M0 deterministic
> scheduler has NOT shipped, so the job runs the two seeded regimes that
> exist today under the reserved name, and the M0 harness swaps in later
> without touching branch protection: (1) the FULL suite under
> `CHRONOKV_STRESS` across N interleaving seeds — new `CKV_STRESS_SEED`
> knob (was a silent hardcoded `0x5EED`), effective seed echoed by every
> stress build, hooks-off smoke included (PR: seed 7; nightly: 7, 21, 42);
> (2) the lincheck engine workload × N seeds — new
> `CKV_LINCHECK_SEEDS`/`CKV_LINCHECK_SEED`, the M1 leftover "N-seed
> scaling (feeds M2's CI job)" (PR: 4; nightly: 64); each seed's history
> must independently pass both checkers and be non-vacuous.
>
> All knobs default to the prior behavior — same seeds, same plans, same
> check names (the only default-run output delta is one echoed crashfuzz
> config line) — so the always-run matrix is untouched. Note the gap vs
> M0's acceptance ("find C1/H3 deterministically within a bounded seed
> count"): until M0 ships, the `dst` job's seeds VARY interleavings
> without making them replayable op-for-op. That is precisely what M0
> exists to close, and v28 stays gated on M0 — not on this job.

## M3 — Coverage, aimed at error paths  **[S]**

**Anchor.** C1 and H3 both lived in code with effectively zero coverage. There is no
coverage signal anywhere today.

gcov/lcov job; publish the number; but more usefully, publish **per-fault-kind coverage**
— which lines are reached under each armed fault. An error-path line no fault ever reaches
is an untested line, and that is exactly where the last four bugs were.

> ### What shipped at 0.27.0 (M3 — v27 arc complete)
>
> **Forcing mechanism (`CKV_COVERAGE_FAULT=<Kind>[,budget]`).** The suite's
> own `fault::arm()/disarm()` windows are narrow by design — coverage
> confined to them would just re-measure the tests that were written.
> Forcing fires the kind at EVERY matching site for the whole run,
> independent of those windows (`disarm()` deliberately does not clear
> it). The budget (default 500) bounds the blast radius so the binary
> still exits cleanly — which is what flushes `.gcda` — and the fired
> count is echoed at exit, so a 0-fire kind is VISIBLY vacuous for that
> vehicle instead of silently publishing the suite's own coverage under a
> fault label. Under forcing the engine fail-stops and the suite REPORTS
> FAILURES: expected; the CI step grades the gcov data, not the verdict.
>
> **Build + analysis.** `make coverage` (--coverage at compile and link,
> -O0 for exact line attribution, no -g — gcov's text output needs no
> debug info and it keeps the instrumented build inside a 1 GiB
> container's commit limit). `scripts/fault_coverage.py` parses raw gcov
> output (zero external deps — no lcov-version roulette across distros),
> classifies error-path lines by a documented heuristic (fail-stop flips,
> `throw`, error returns, `WalFailure`/`Status::Failed`, `FATAL`, `errno`,
> `cerr`), and publishes: per-run lines/%, per-kind error-path counts,
> each kind's UNIQUE contributions (the per-fault-kind number this
> milestone exists for), fire counts, and the **ledger of error-path
> lines no run reached** — job summary + artifact. The job gates on
> pipeline health only; the ledger is the deliverable, and red-PR-ing on
> gap count would gate unrelated changes.
>
> **Vehicles.** push/PR: every kind forced over the review-regression
> gate (`CKV_ONLY_REVIEW` — review + durability + crash-fuzz, the
> error-path-dense subset; ~25 s/kind). Nightly/dispatch: every kind over
> the FULL suite + one unforced full-suite headline run. The split is
> measured, not guessed: **OpenFail and DirFsyncFail fire 0 times under
> the gate vehicle** (their sites live in checkpoint paths the gate never
> enters) — the report flags exactly this vacuity, which is the anchor's
> point demonstrated on day one.
>
> **Verified locally:** all 8 forced kinds complete hang-free (~24 s each;
> fires: SegOpenFail 320, WriteShort 48, FsyncFail 15,
> FsyncFailAfterPersist 15, WriteFail 2, RenameFail 1, OpenFail 0,
> DirFsyncFail 0); gcov invocation form (positional `*.gcda` — the
> binary/source basenames differ) and the script proven end-to-end on
> probe TUs, including the ledger catching a planted untested error
> line. The engine-sized gcov build exceeds the dev container's commit
> limit (~1 GB VA) — the CI run on the 0.27.0 push is its proof, same
> policy as ASan/-O2 builds.
>
> **Nightly-vehicle findings (dispatch runs #20/#21, fixed same-day, no
> bump):** the FULL suite under global forcing dies within seconds for six
> of the eight kinds (std::terminate from escaping async `fut.get()`
> rethrows and stoi cascades, plus signal deaths — the suite was never
> designed to survive every fault being on at once), and a dead process
> flushed no `.gcda`, failing the job. Run #21 then exposed a subtle
> linker trap in the first fix: a WEAK undefined `__gcov_dump` reference
> does not pull its member out of static `libgcov.a` — the "dump" silently
> bound to null. Final design: (1) death hooks with a STRONG dump
> reference under `-DCKV_COVERAGE_BUILD=1` (weak+null-checked elsewhere)
> in `std::set_terminate` (log, `_exit(70)`) and SIGSEGV/BUS/FPE/ILL
> handlers (log, dump, restore `SIG_DFL`, re-raise) — forced death is
> DATA-PRESERVING; crash-fuzz semantics untouched (`crashpt::point` kills
> via `_exit(97)`, never a catchable signal); (2) the nightly vehicle is
> TWO-STAGE per kind — review gate first (proven to survive all eight
> kinds), then the best-effort full suite — libgcov merges both into one
> `.gcda`, so every kind keeps at least gate depth and gains full-suite
> depth wherever it survives; (3) the analyze gate warns per kind and
> hard-fails only when NO kind produced data. **Follow-up (M3-adjacent
> hygiene): harden the suite's escape points** — catch at the async
> `get()` sites and the stoi parsers — so all eight kinds run
> full-suite-deep; each fixed escape point deepens six kinds' coverage at
> once.
>
> **Next consumers of this signal:** the ledger should be triaged into
> new fault kinds/tests where gaps are real (candidate v28 M-adjacent
> hygiene), and v28's WAL-writer-thread rewrite should land with
> before/after per-kind tables so the rewrite cannot silently un-cover
> the error paths it replaces.

---
