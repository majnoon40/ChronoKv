# ChronoKV Audit-2 — Reconstructed Record

- **Target:** 0.28.0 (tag `v0.28.0`, commit `3161c77`)
- **Round:** second independent adversarial audit ("Audit-2"), commissioned
  per `docs/ROADMAP.md` v29 M0 with the round-1 report and the remediation
  specification's status table handed over as the map.
- **Remediation shipped as:** 0.28.1 — PR #2 (`5cd74b5`, merged `b6fb181`),
  battery `run_audit2_regression_tests` in `CKV_ONLY_REMEDIATION`.
- **This document:** reconstructed 2026-10-08 and committed per process
  rule 8 (the finding→test mapping must be checkable from the tree).

## Provenance — read this first

**The original audit session was not preserved.** The round ran in an
external LLM session whose transcript is lost; no original report file,
prompt text, finding wording, severity assignments, or verdict letter
survives to transcribe. This document does not invent any of those. It
consolidates what *is* checkable:

1. **The in-tree remediation record** — each fix's rationale comment in
   `chronokv.hpp` cites its finding ID and mechanism (e.g. `TXN-1 fix:`,
   `WAL-3: a file IN dir_`, `BT-1 parity ... external-review probe`); the
   15-check Audit-2 battery names every detector; the 0.28.1 CHANGELOG
   entry and the header's v28.1 preamble block carry the arc summary.
2. **The cross-verification log** — an independent review agent (a second
   LLM session, distinct from both the audit and the remediation author)
   re-ran differential PoCs against the pre-fix header at tag `v0.28.0`,
   ran the full verification matrix on the patched tree, and recorded
   results in its review notes (2026-10-04..07). PoC outputs quoted below
   are from that log; every one is re-runnable from the tree per the
   reproduction notes.
3. **The CI ledger** — runs #62–#67 on the GitHub Actions record.

Severity classes below are **assigned in reconstruction** from impact
(soundness / memory-safety / durability / lifetime / hygiene), not
transcribed. Where this document says "the audit found X", the citation
is the shipped fix comment and its battery test — the two artifacts rule 8
exists to keep in sync.

## Method and coverage (as reconstructable)

Full read-through of engine and suite at 0.28.0 with PoC-first discipline:
every finding below was demonstrated against the target before its fix was
written (the fail-first property the battery preserves). The
cross-verification independently reproduced four findings against the
pre-fix header in a sandbox (2-core Xeon 2.5 GHz, 1 GiB RAM, kernel 4.19
with io_uring I/O ops seccomp-blocked, overlayfs) and verified the
remediation end-to-end: full suite 495 checks green, remediation battery
green, lincheck + 8 mutation detectors green, DST 3×20 seeds green,
crash-fuzz 624 iterations / 316 armed crash points / 0 violations,
hooks-off consumer build green.

## Findings summary

Thirteen findings. All remediated in 0.28.1; all with battery tests.

| ID | Class (reconstruction-assigned) | Component | One-line mechanism |
| --- | --- | --- | --- |
| TXN-1 | **Critical — SSI soundness** | PhantomTracker / commit_txn | Only existence transitions were recorded: a value UPDATE inside a concurrently scanned range did not conflict the scanner — value-based write skew |
| TXN-2 | High — resource exhaustion | commit_txn read-set path | Absent read-set keys materialized an invisible index entry per negative lookup — unbounded pool growth |
| TXN-3 | Medium — lifetime leak | ReadWriteTransaction / Transaction | Cleanup keyed on the Database open-flag, not engine liveness: close() with a live txn pinned its reader slot and phantom registration forever; member order destroyed the keepalive before the txn |
| WAL-1 | High — recovery classification | wal_recover_buf | Torn-tail-vs-interior decision trusted the damaged frame's own length field: a corrupted interior length made interior damage truncatable as a torn tail |
| WAL-2 | High — durability | WalSegments::open_segment | Segment creation never fsynced the directory: acknowledged records could live in a file whose directory entry was not durable |
| WAL-3 | High — durability | rotation paths | `fsync_dir(path)` syncs the PARENT of `path`; both rotation call sites passed the WAL dir itself — syncing the wrong directory, leaving the MANIFEST rename non-durable |
| WAL-4 | Medium — recovery DoS | recover_all | A forged/corrupt MANIFEST `active_id` sent recovery into a ~2^64-iteration segment loop |
| BT-1 | High — acked-key loss on exhaustion | BTree::put cascade | Split cascade allocated upper levels after lower levels had split and relinked: mid-cascade pool exhaustion orphaned acknowledged keys |
| BT-2 | Medium — standalone concurrency | BTree writers | Standalone BTree had no writer serialization; concurrent structural writers lost keys (engine unaffected — `nm_` serializes there) |
| EXTRA-1 | High — fail-stop window | group_append reservation catch | The reservation-throw latch fired only in commit_txn's catch, after batch_mu_ was released: a concurrent committer could reserve and write past the burned cts (interior gap at next open) |
| EXTRA-2 | **Critical — memory safety** | leaf in-place update | Slab offset computed in uint16_t: a value larger than free_hi wrapped, passed both size guards, memcpy'd out of bounds across pages |
| API-1 | Medium — ODR / consumer contract | whole header | No include guard; non-inline statics and static member definitions: the "single header" failed double-include and two-TU linking |
| API-3 | High — lifetime / move semantics | Database observers | Observer registry and notify paths held raw `Database*`: a moved Database silently dropped its observers; an outliving handle's unregister lambda could dangle |

## Findings detail

### TXN-1 — scanned-range value updates did not conflict (Critical, SSI soundness)

**Mechanism.** `PhantomTracker::record_transition` early-returned unless
`old_exists != new_exists`. SSI's rw-antidependency coverage therefore
spanned inserts and deletes only. A transaction that scanned a range, read
a key's *value* through that scan, and then wrote on the basis of that
value committed cleanly across a concurrent update of the scanned key —
value-based write skew, the exact anomaly class claim 2 of the roadmap
exists to prevent. Round 1's report graded phantom detection "(verified
sound)"; both statements were true of their round (roadmap reading rule:
verdicts are round-scoped).

**PoC (cross-verification log, pre-fix header at `v0.28.0`).** k="0",
x="0"; T1 scans [k,k]; T2 (read x) updates k→"1", commits OK; T1 writes
x="1", commits — **T1=Committed (anomaly)**. Post-fix: **T1=Conflict**.

**Fix.** Record every committed write. **Refinement over the audit's own
fix** (shipped in the same arc, cross-verified): a no-op delete of an
ABSENT key (`!old_exists && !new_exists`) stays unrecorded — its tombstone
carries a cts above every concurrent snapshot, so no visible scan output
can change; recording it is pure false conflict. The v17 no-op-delete
contract (guard tests 3 and 5) stays green by soundness, not by
accommodation.

**Detectors.** `A2 TXN-1 scan/update conflict`, `A2 TXN-1 SSI write-skew
still rejected`; v17 guard test 4 **flipped and renamed** (`update of
present key in scanned range CONFLICTS`) per process rule 14 — its old
expectation encoded the anomaly.

**Consequence for the plan.** lincheck's SSI-blindness (remediation spec
§11, round 1) was demonstrated in production: this was found by an audit
round, not by a machine checker. v29 M2 item 1 (write-skew checker) is
re-justified, with this PoC's history as its validation target: the
checker must flag the pre-fix engine and pass the post-fix one.

### TXN-2 — negative lookups materialized index entries (High, exhaustion)

**Mechanism.** commit_txn ran `ensure_index(k)` for every read-set key,
creating (and never reclaiming) an invisible KeyEntry per absent key —
the pool grew with negative-lookup count, not with data.

**PoC.** 1 MiB pool, txn loop {get absent_i; put w}: pre-fix
**bad_alloc at i=16,554**; post-fix **30,000 clean**.

**Fix.** Absent read-set keys become `[k,k]` point range reads validated
through the phantom tracker (`find_index`, non-creating). Sound *because
of* TXN-1's completeness: a later insert of k is a recorded
false→true transition inside `[k,k]`. Check-then-insert still conflicts.

**Detectors.** `A2 TXN-2 absent reads do not leak`, `A2 TXN-2
check-then-insert conflicts`.

### TXN-3 — close-with-live-txn pinned reader state; destruction-order hazard (Medium, lifetime)

**Mechanism.** `ReadWriteTransaction`'s destructor cleaned up only
`engine_live()` (the Database open-flag): after `close()`, a live
transaction's reader slot and phantom registration stayed pinned
forever. Separately, `Transaction`'s member order destroyed
`engine_keepalive_` before `txn_` — cleanup ran with the engine pin
already released.

**Fix.** Cleanup keys on engine liveness via a `weak_ptr<ChronoKV>` pin;
member order fixed so `txn_` dies while the keepalive still pins.

**Detector.** `A2 TXN-3 close with live txn unpins` (asserts
`phantom_reader_count == 0` after close-then-abort, via a test engine
keepalive).

### WAL-1 — interior damage classified by the damaged frame's own length (High, recovery)

**Mechanism.** On a parse failure, the torn-tail-vs-CORRUPT decision read
the failing frame's length field and looked for a valid frame at exactly
that offset. A corrupted length (pointing anywhere, or past EOF) made
*interior* damage look like a torn tail — and torn tails are truncated,
silently discarding every valid record after the damage. This is the
CKV-019 "fail loud, never truncate silently" invariant, violated through
a different door than round 1's.

**Fix.** Never trust the damaged length: scan EVERY later byte offset for
a CRC-valid frame continuing the LSN sequence (cheap LSN + structural
prefilter; CRC only on plausible candidates — linear in practice). Found
→ CORRUPT (loud, file untouched); none → TORN_TAIL.

**Detectors.** `A2 WAL-1 corrupt length is not a torn tail` — three
trials: interior length = 0xFFFFFFFF (must refuse, file unchanged),
interior length +1 (must refuse), true torn tail (must recover a,b).

### WAL-2 — segment creation did not fsync the directory (High, durability)

**Mechanism.** Records were acknowledged after file-data fsync, but a new
segment's *directory entry* was never made durable: power loss could
leave acked records in a file the filesystem never committed to the
directory. Also covers the retry shape: an earlier attempt that created
the file but failed the dir fsync leaves an EMPTY segment a later open
would treat as "existing".

**Fix.** `open_segment` fsyncs the WAL directory when it creates a
segment *or* reopens a still-empty one; a failed dir fsync fails the
open (and therefore every later ack).

**Detectors.** `A2 WAL-2 new segment dir fsync` (armed DirFsyncFail is
consumed and the open/put fails), `A2 WAL-2 retry after failed dir
fsync` (the empty-segment retry path re-syncs).

### WAL-3 — rotation fsynced the wrong directory (High, durability)

**Mechanism.** `fsync_dir(path)` strips the last path component and
syncs the *parent*. Both rotation-path call sites passed `dir_` (the WAL
directory itself) — syncing `dir_`'s parent, so the MANIFEST rename
inside `dir_` was never made durable on those paths. Found by reading
the helper's semantics against its call sites; the v25.7 manifest-brick
history is why the round looked. Invisible to `_exit()`-based crash
fuzzing (page cache survives) — only a lying block layer or a real power
cut exercises this class (v30 M3's anchor, with this finding as evidence).

**Fix.** Both sites pass `manifest_path()` — a file IN `dir_`, so `dir_`
itself is synced. Verified: no `fsync_dir(dir_)`-shaped call remains.

**Detector.** Covered compositionally by the crash-fuzz rotation plans
(`rot_after_dir_fsync` point) and the WAL-2 fault-kind runs; the fix is
call-site-verifiable by grep, which the cross-verification performed.

### WAL-4 — unbounded recovery loop on a corrupt MANIFEST (Medium, DoS)

**PoC shape.** Forged MANIFEST `active_id` = 10^8 or UINT64_MAX (CRC
recomputed): pre-fix recovery iterated toward 2^64 segment ids.

**Fix.** Bound the loop by what is on disk: `active_id > max_present+1`
cannot come from any interrupted rotation → loud immediate failure; the
checkpoint-deleted prefix (`ckpt_ts > 0`) is skipped in O(1).

**Detector.** `A2 WAL-4 huge active_id fails fast` (fork-isolated,
5 s deadline: control opens, forged manifests exit non-zero fast).

### BT-1 — exhaustion mid-split-cascade orphaned acked keys (High)

**Mechanism.** The split cascade allocated upper-level pages *after*
lower levels had split and relinked: pool exhaustion partway left the
tree mutated with no rollback — acknowledged keys unreachable. (Same
family as round 1's CKV-003, on the cascade path the plan-before-mutate
planner did not cover for *allocation* failure.)

**Fix.** `require_split_headroom()` — conservative worst case
(`5*(height+1)+1` pages, justified from the greedy group planner's ≤5
pages per level) checked BEFORE any mutation; throws with the tree
untouched. In-place updates (new value ≤ old) are exempt on **both**
write entry points — `put` and `put_with_old` — because they allocate
nothing (the EXTRA-2 `fits()`/compact path).

**Parity note (rule 12's incident).** The exemption shipped first on
`put` only, while the changelog claimed it generally; the cross-
verification harness probed `put_with_old` and found the unconditional
headroom check. Fixed fail-first: the extended exhaustion test was
verified throwing bad_alloc on `put_with_old` pre-fix, green post-fix.

**Detectors.** `A2 BT-1 exhaustion keeps acked keys` (48 pool/shape
combos, sorted and shuffled), `A2 BT-1 in-place update after exhaustion`
(drives both entry points on an exhausted pool).

### BT-2 — standalone BTree had no writer serialization (Medium)

**Fix.** `write_mu_` serializes structural writers (put / put_with_old /
erase). Uncontended under the engine (`nm_` already serializes); makes
the standalone tree — which BT-1's tests and any direct consumer use —
writer-safe.

**Detector.** `A2 BT-2 concurrent writers` (4 threads × 20k keys, zero
missing).

### EXTRA-1 — reservation-throw latch window (High, fail-stop)

**Mechanism.** CKV-012R's latch (no writes past a burned, frameless cts)
fired in commit_txn's catch — *after* group_append had unwound and
released `batch_mu_`. In that window a concurrent committer could reserve
ts+1 and write it past the hole → interior gap → the whole directory
rejected at next open: a transient OOM becoming an unopenable database.

**Fix.** Latch `failed_` INSIDE group_append's catch, under `batch_mu_`
(commit_txn's latch remains, idempotent).

**Detector.** `A2 EXTRA-1 latch set before unlock` — a test hook parks
the throwing committer in the exact window (AllocFail armed); a second
committer must be refused, and reopen must recover the earlier acked
write. (The audit's own widened-window race PoC ran from a scratch
header with an injected sleep, per the battery's comment.)

### EXTRA-2 — uint16 slab-offset wrap in the update path (Critical, memory safety)

**Mechanism.** The leaf in-place-update computed `uint16_t new_off =
free_hi(p) - value.size()`: for `value.size() > free_hi` the offset
wrapped near 2^16 and passed BOTH size guards — a memcpy far outside the
page, into neighboring pages. Same uint16-narrowing class as round 1's
CKV-001/002, on a path round 1 did not walk. Engine-level exposure was
narrow (engine values are 8-byte encoded pointers); standalone-BTree
exposure was direct.

**PoC.** 3,000 keys × ~100 B values, then a 2,500-byte update of one
key: pre-fix **4 neighboring keys corrupted**; post-fix **0**.

**Fix.** Size_t arithmetic with an exact `fits()` predicate
(`value.size() <= free_hi && free_hi - value.size() >= free_lo`),
before and after compaction.

**Detector.** `A2 EXTRA-2 update-path wrap`. **Permanent gate:** the
-Wconversion count ratchet (`scripts/lint_wconversion.sh`, baseline 33)
makes this narrowing class a CI-gated property — new implicit
conversions fail the lint job.

### API-1 — the "single header" was not single-header-safe (Medium, ODR)

**Mechanism.** No include guard; `static` free functions and variables;
out-of-line definitions of static members (`ChronoKV::instance_registry_mu_`
et al.). Double-inclusion redefined; two TUs linked got duplicate
symbols / per-TU registry copies (the instance registry — the reentrancy
guard — silently split per TU).

**Fix.** `#pragma once`; `static` → `inline` (functions and variables);
`static inline` members. `main.cpp` now double-includes the header as a
live check.

**Detectors.** CI-gated (not informational): `make lint-header-reinclude`
and `make lint-header-2tu` in the `lint` job.

### API-3 — observers did not survive a Database move; raw-pointer notify paths (High, lifetime)

**Mechanism.** The move constructor/assignment moved the engine and
liveness flag but not the observer registry; async workers, `Batch` and
`Transaction` notified through a captured raw `Database*`. After a move:
observers silently stopped firing (PoC: **registered observer fired 0
times post-move; post-fix 1**), and an unregister lambda from an
outliving handle could run against a destroyed/moved-from object.

**Fix.** Observer registry lives in a shared `ObserverState` that moves
with the Database; every notify path holds a `shared_ptr` (async lambdas
capture it by value); handles hold a `weak_ptr` to the state, never a
raw `Database*`.

**Detector.** `A2 API-3 observers survive move` (move-construct and
move-assign shapes).

## Cross-verification record (rule 12's evidence)

The remediation was reviewed by an agent independent of both the audit
and the fix authorship, in a 2-core / 1 GiB / io_uring-blocked / overlayfs
sandbox (floors, not ledger material):

- Differential PoCs re-run against the pre-fix header at `v0.28.0`:
  TXN-1 (anomalous commit), TXN-2 (exhaustion at 16,554), EXTRA-2
  (4 corrupted keys), API-3 (0 observer fires) — all reproduced, all
  closed post-fix.
- Full verification matrix on the patched tree: full suite 495/0 (−O0),
  remediation battery, lincheck + 8 mutations, DST 3×20, crash-fuzz
  624/316/0, hooks-off smoke, `make lint` incl. the ratchet at 33 on
  g++ 12.2 against the 13.3-recorded baseline.
- Found and fail-first-fixed the `put_with_old` parity gap (BT-1 above).
- Measured the remediation's cost (rule 9): ~10–13% on write-heavy arena
  mixes (fillseq 30,077→26,056; fillrandom 39,530→35,067; ycsb_a
  ~40k→~36k), reads unchanged, ycsb_a RSS +10.7% — accepted and recorded.

## What this round did NOT cover (honest gaps)

- **Power-loss / lying block layer.** WAL-2/WAL-3-class defects are
  invisible to `_exit()` crash fuzzing; they were found by reading. Only
  v30 M3's dm-flakey (or plan-B) vehicle tests this class mechanically.
- **Sanitizer coverage during verification.** The 1 GiB sandbox could not
  build ASan/TSan at any -O level; the experimental CI legs (green from
  #64 on) are the sanitizer evidence, not the audit.
- **Real-kernel io_uring.** Sandbox kernels block the I/O ops; the mock
  state machine and the pwrite fallback were exercised, the native chain
  was not (CI's io_uring-enabled legs cover it).
- **Depth re-audit of PITR/backup** beyond the WAL-4 recovery bound, and
  **no performance work at scale** — the arena ledger (v29 M1) owns that.
- The delta confirmation round over 0.28.1 (M0 closure) had not run at
  reconstruction time; its four hunt-classes are named in ROADMAP v29 M0.

## Rule-8 mapping — finding → detector → commit

| Finding | Battery detector(s) | Landed in |
| --- | --- | --- |
| TXN-1 | A2 TXN-1 ×2; v17 guard test 4 (flipped, renamed) | `5cd74b5` (merged `b6fb181`) |
| TXN-2 | A2 TXN-2 ×2 | same |
| TXN-3 | A2 TXN-3 | same |
| WAL-1 | A2 WAL-1 (3 trials) | same |
| WAL-2 | A2 WAL-2 ×2 | same |
| WAL-3 | call-site grep + crash-fuzz rotation plans | same |
| WAL-4 | A2 WAL-4 (fork-isolated) | same |
| BT-1 | A2 BT-1 ×2 (48 combos + both entry points) | same; parity in the v6 pre-merge revision of the same PR |
| BT-2 | A2 BT-2 | same |
| EXTRA-1 | A2 EXTRA-1 (hook-parked window) | same |
| EXTRA-2 | A2 EXTRA-2 + lint ratchet | same |
| API-1 | lint-header-reinclude / lint-header-2tu (CI-gated) + main.cpp double-include | same |
| API-3 | A2 API-3 (both move shapes) | same |
