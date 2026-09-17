# ChronoKV roadmap — v26 → v30

Written 2026-09-17 against `main @ 3929d25` (v25.3), following an external code
review of that commit.

Progress:

| Milestone | Status | Shipped as |
| --- | --- | --- |
| v25.3 — review defect fixes (C1, H1, H3, H4) | **DONE** | `3929d25`, `0.25.3` |
| v26 M0 — durable rollback truncation (D2) | **DONE** | `3509586`, `0.25.4` |
| v26 M1–M4, v27–v30 | not started | — |

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

**Status:** M0 **DONE** (`3509586`, shipped as `0.25.4`). M1–M4 not started.

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

## M1 — Adversarial fsync semantics  **[M] [HIGHEST VALUE]**

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

**Acceptance.** A matrix of {fault kind} × {crash point} × {Sync, Group, Async} asserting
jointly: no resurrection (D2), no silent loss of an acknowledged-durable record, no LSN
gap or duplicate, and `verify_wal_dir()` clean after recovery. Every cell must be
exercised — publish the matrix as coverage, not just as a pass count.

## M2 — Randomized crash-point fuzzing  **[M]**

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

## M3 — Online backup API  **[M]**

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

## M4 (stretch) — Point-in-time restore  **[S]**

`restore(backup, as_of_cts)` — replay WAL and stop at a cts boundary. Cheap because
recovery already walks records in cts order. Only worth doing if someone needs PITR;
otherwise the M3 semantics are sufficient.

---

# v27 — Make concurrency bugs reproducible

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

## M2 — CI integration  **[S]**

New jobs: `dst` (N seeds) and `crashfuzz` (from v26 M2). Bounded runtime in PR CI, long
runs nightly. Fold both into the existing `ci-passed` aggregate gate.

## M3 — Coverage, aimed at error paths  **[S]**

**Anchor.** C1 and H3 both lived in code with effectively zero coverage. There is no
coverage signal anywhere today.

gcov/lcov job; publish the number; but more usefully, publish **per-fault-kind coverage**
— which lines are reached under each armed fault. An error-path line no fault ever reaches
is an untested line, and that is exactly where the last four bugs were.

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
