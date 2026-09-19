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
| v28–v30 | not started | — |

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
> state). The real PITR window is a LIVE/CRASHED database directory (or an
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

# v28 — Remove the leader-election handshake

Theme: the most bug-dense code in the file exists only because the WAL leader role
migrates between committer threads. Your own io_uring comment already names the fix.

**Gated on v27.** Do not start before the DST harness is green.

## M0 — Measure first  **[S]**

Your own rule: measure before predict. Publish commit throughput and p50/p99/max latency
at 1/2/4/8/16/32 concurrent committers, per durability mode, on the existing path.
Without this baseline you cannot claim the rewrite helped, and you cannot detect a
regression in it.

## M1 — Dedicated writer thread + bounded MPSC queue  **[L]**

Committers enqueue a batch descriptor and wait on a per-batch completion signal. One
thread owns the ring for its lifetime. This **deletes** `leader_active_`, the shared
`cur_batch_` mutation, the leader/follower handshake and the `pending_` FIFO — i.e. the
exact code that produced C1 and H3. It does not get more careful; it stops existing.

## M2 — Unlock the io_uring features you had to reject  **[M]**

`SINGLE_ISSUER` and `DEFER_TASKRUN` become legal with one issuer; `SQPOLL` becomes
worth its kernel thread because submits stop costing a syscall each. The ring is already
sized 64/128, and queue depth > 1 finally lets you pipeline batches instead of waiting
per batch.

## M3 — Adaptive group-commit linger  **[M]**

`group_commit_linger_us` (default 0 = no added latency), auto-tuned under load. Probably
the largest throughput win available under concurrency, and trivial once one thread owns
batching.

## M4 — Delete the old path  **[S]**

Keep it behind a compile flag for exactly one release, then remove. Re-run the *entire*
DST + crash-fuzz + fault matrix against the new path before deleting anything.

---

# v29 — Close the memory ceiling

Theme: the binding constraint on real use. Pages are `aligned_alloc` RAM and never touch
disk; `PagePool::free()` has no callers; the pool grows monotonically to `bad_alloc`.

## M0 — Surface it before it is fatal  **[S]**

Pool utilization and high-water mark into `stats()`; `health()` → degraded at 80%,
failing at 95%; a clear `Status`/exception naming the cause instead of a bare
`bad_alloc` from inside a `put()`. Cheap, and immediately useful even if M1 slips.

## M1 — Leaf merge and rebalance  **[XL]**

The real work. Crabbing with exclusive latches on the delete path, sibling borrow vs.
merge, interior key deletion, root collapse. The fence-recheck machinery and latch
crabbing already exist for splits — merges are the mirror image, which is the reason
this is feasible at all.

## M2 — Give the hazard pointers a consumer  **[M]**

`HPRegistry::is_hazardous()` still has no caller, and `PagePool::free()` and
`LatchTable::erase()` still have none. As of v25.3 the slots are bounded and per-tree, so
the scaffolding is finally in a state worth wiring up. M1 produces retired pages; M2
decides when they are safe to reuse.

## M3 — ABA safety  **[M]**

Your own comment names this as *specified but not implemented*: "HP4 — the free-list
reuse is tagged or Version allocations are never reused." Free-list reuse plus hazard
pointers is the classic ABA setup. Either tag page ids with a generation counter, or
route reuse through the epoch reclaimer you already have for version chains. Decide
before M2, not during.

**New invariant.**
> **P1** — under a sustained delete-heavy workload the page pool reaches steady state
> with bounded utilization, with no use-after-free and no ABA.

**Acceptance.** Delete-heavy soak at steady state; concurrent cursor + concurrent merge
clean under TSan *and* the v27 DST harness; the existing differential-vs-`std::map` test
still passes with merges active.

---

# v30 — Ergonomics and measured performance

Independent of the spine; can be done in pieces at any time.

| # | Item | Size | Anchor |
| --- | --- | --- | --- |
| M0 | Visitor scan `for_each_in_range(lo,hi,fn)` + `get_into(key,out)` | S | `range_scan` materializes `vector<pair<string,string>>` — two heap allocations per entry; `get()` copies |
| M1 | Un-gate `CHRONOKV_BENCH`, run in CI, publish a numbers table in README | S | Your own lesson: *"bench-gated tests are invisible to the sanitizer matrix."* You preach measurement and publish no numbers |
| M2 | Async API: back it with a worker pool **or remove it** | M | Measured at **0.256× sync** (thread-per-op). Shipping an async API slower than sync is a trap |
| M3 | `Transaction` rollback-on-destroy behind an opt-in strict flag | S | Needs your sign-off — reverses a documented decision and the `abort-on-drop` test |
| M4 | Delta-encode WAL values against their previous version | M | Dependency-free, fits the existing version chain. Measure WAL volume before/after |
| M5 | Split sources + generate the amalgamated header as a release artifact | M | `-O2` on one ~17k-line TU needs >1 GiB RSS and is OOM-killed below that |

---

## Explicitly out of scope

| Item | Why not |
| --- | --- |
| **Raft / replication** | The *only* thing that would make Jepsen applicable. A multi-version project in its own right, correctly deferred already |
| **Language bindings** (GAP-EF1) | Conflicts with single-header purity. Your own note says defer until a milestone needs it |
| **Encryption at rest** | Different product; belongs in the embedder's filesystem layer |
| **Multi-process / non-Linux** | Documented limitations with real architectural cost |
| **Third-party compression** | Breaks the zero-dependency property, which is the differentiator. M4/v30 gets most of the win without it |

Your deferrals here are correct. "Single header, zero dependencies, verifiably correct"
is the actual product; each of the above dilutes it.

---

## On Jepsen specifically

**Not reachable, and not a quality judgement.** Jepsen needs a distributed system:
multiple nodes, a network API, replication, and a client protocol it can drive while
partitioning the network and pausing processes. ChronoKV is single-process by design —
`flock` actively fails fast on a second opener. There is nothing for Jepsen to connect to.

What Jepsen *represents* — adversarial fault injection plus history checking against a
consistency model — is fully reachable, and for a single-node store the equivalent bug
class is crashes and fsync lying rather than partitions. That is v26 M1/M2 and v27 M1.

One genuine strength to keep: **cts is a monotonic counter, not wall-clock**, so the
entire clock-skew bug class does not apply. Do not introduce a wall-clock dependency
anywhere in the durability path; it would open a bug class you currently do not have.

---

## Suggested first commit

**v26 M0 alone.** It is a confirmed live durability defect, roughly ten lines, and it
comes with a crash test that fails before the fix and passes after. Shipping it first
establishes the crash-test harness (fork + `_exit` + raw-segment inspection) that M1 and
M2 then build on, so the small fix pays for infrastructure the rest of the plan needs.
