# M5 WAL writer thread: the rewrite decision

**Date:** 2026-10-20
**Status:** DRAFT — DECIDED pending the maintainer's answers to §12 (open
questions). This is the decide-before-build document the v29 M5 milestone
gets by the arc's standing process (M3's HP4/ABA doc and M4's merge-
protocol doc are the precedents; M5 is the arc's largest DELETION and the
house rule for those is the plan's own sentence: "the code stops existing;
it does not get rewritten more carefully").
**Scope:** the WalSegments group-commit rewrite — dedicated writer thread
+ bounded MPSC queue replacing the leader/follower handshake; the
io_uring ladder consequences (SINGLE_ISSUER / DEFER_TASKRUN / SQPOLL /
queue depth); adaptive linger; the compile-flag lifeboat and its deletion
gate; the seam/hook/crash-point/fault re-anchoring the DST, crash-fuzz
and fault matrices depend on. NOT in scope: the on-disk WAL format (there
is NO format change — §6), recovery/replay logic, the publication tracker
and the strict-serializable ack barrier (both carried verbatim — §5),
checkpoint internals (only `rotate_after_checkpoint`'s call shape moves),
the async API's `std::async` layer (M6's worker-pool-or-removal decision
is untouched), and PITR/backup paths.
**Code basis:** every anchor below was read at 0.28.1, commit `92ffaba`
(v29 M4 landing; WAL side identical to `f053698`/0.28.1). Citations use
function-scope anchors — line numbers rot in this repo; names do not.

---

## 1. The decisions

1. **One dedicated writer thread per `WalSegments`, owning ALL WAL file
   I/O.** Started at open when `wal_dir` is non-empty (PITR read-only
   opens start none — they never write), joined at close/destruction
   (NEVER detached). The writer is the sole executor of: batch writes
   (io_uring chain or offset-pinned pwrite fallback), fsyncs, size
   rotations (`maybe_rotate_segment`), checkpoint rotations (as a queue
   command, §1.8), rollbacks (truncate + SRC-B zero-fill). The M5
   deletion list executes verbatim: `leader_active_`, the shared
   `cur_batch_` mutation, the leader/follower election loop and the
   `pending_` FIFO **stop existing** — not rewritten, deleted. The
   `Batch` struct SURVIVES as the drain unit (records + `written` /
   `done` / `failed` terminal flags are still the completion vehicle;
   the roadmap's deletion list never included it).
2. **The reservation window moves WHOLESALE to the queue producer, under
   `queue_mu_`.** One critical section performs, in today's exact order:
   `failed_` gate → cts reservation (`clock.fetch_add`, seq_cst) →
   `on_reserve` (the v17 validation + transition publication that MUST
   run inside the reservation section) → no-op-record substitution on
   conflict → `wal_ser` → class tagging → enqueue. EXTRA-1's latch point
   moves with the window VERBATIM, as the roadmap requires: a throw
   anywhere inside (on_reserve re-throw, `wal_ser`/enqueue bad_alloc,
   the `AllocFail` fault site which moves with the window) latches
   `failed_` UNDER `queue_mu_` before any concurrent committer can
   reserve past the burned cts, burns via `on_abandon` (delegated
   `pub_.burn`, CKV-012's shape), and rethrows for `commit_txn`'s
   WalFailure translation + idempotent CKV-012R latch. CKV-012's
   window-END re-anchors from "the `has_async` store" to "enqueue
   complete": from there on, every item reaches a TERMINAL state by the
   writer or by the death-drain (§1.6) — the no-orphaned-cts argument is
   re-derived in §4 WW1, not assumed.
3. **Bounded MPSC queue; FIFO == cts order by construction.** Producers
   (committer threads) enqueue; one consumer (the writer) drains.
   Bounded by item count AND total record bytes (defaults 4096 / 32 MiB,
   `Options`-tunable). A producer finding the queue full waits on the
   queue cv WITHOUT holding `queue_mu_` and **before reserving its cts**
   — reservation happens only once space is assured, in the same
   critical section as the enqueue, so backpressure can never create a
   reserved-but-unqueued hole, and cts order == enqueue order == drain
   order == WAL offset order is STRUCTURAL. Consequence: today's
   defensive `std::sort` of the batch by cts in the leader path is
   subsumed — the writer appends frames in dequeue order, full stop.
   (Today's `pending_` is UNBOUNDED — the bound is a strict improvement,
   recorded per rule 9: this is a capacity-semantics change, producers
   can now block where they previously could not; the block is the same
   class as waiting for `batch_mu_` behind a leader's fsync.)
4. **Durability-class separation becomes a drain policy.** v24-Fix-4's
   async-XOR-durable batch invariant survives as: the writer drains the
   MAXIMAL SAME-CLASS PREFIX of the queue (class of the head item) per
   I/O batch. No mixed batch can exist, so CKV-004's stage-gated
   rollback policy ("truncate iff no caller can have acked": any
   WRITE-stage failure of any class, any failure of a durable batch; the
   single no-truncate branch is a published-`written` async batch failing
   at FSYNC, debited to `async_committed_then_lost`) carries over with
   ZERO policy edits — its `written && has_async` gate reads the same
   flags on the same `Batch`. The H3 orphan class is structurally dead:
   there is no handoff, and every queued item has exactly one owner (the
   writer) plus exactly one death path (§1.6).
5. **Completion keeps today's waiter semantics exactly.** One
   `done_cv_` (broadcast) + per-`Batch` terminal flags: durable waiters
   wake on `done || failed`, async waiters on `written || failed` —
   the predicates are today's `group_append` waiter loop, unchanged;
   `written` is still published (release) after a successful WRITE stage
   and BEFORE the fsync runs, so the async silent-loss contract and the
   D2 truncation gate keep their exact stage semantics. The state flip
   (`done`/`failed`, `failed_` latch, `active_segment_bytes_`
   accounting) runs under `queue_mu_` — the successor to today's
   "re-acquire `batch_mu_`, flip, `leader_active_ = false`,
   `notify_all`" block, minus the parts that die.
6. **New invariant W-STRAND (the C1-class successor): writer failure is
   always terminal-for-waiters, never silent.** The writer loop's body is
   total-catch: any exception or I/O verdict failure → latch `failed_`,
   fail the current batch, then DRAIN THE REMAINING QUEUE failing every
   item (each waiter's predicate wakes with `failed`), then park in the
   failed state (new producers are refused at the `failed_` gate, as
   today). The thread never exits while waiters exist; `std::terminate`
   is the loud floor under the catch-all. Controls kill/throw the writer
   through hooks-gated seams and assert bounded WalFailure for every
   in-flight waiter AND every later commit, with no hang (the C1 battery
   re-anchors here per rule 14 — see §11).
7. **`rotate_after_checkpoint` becomes a queued ROTATE command.** The
   checkpoint flow enqueues `ROTATE{ckpt_ts}` (after the checkpoint
   artifact is durable) and waits for its terminal state; the writer
   executes it in FIFO position — so R-REBASE's "rotation can never
   overake or interleave with batches that were committed before the
   checkpoint" is ORDER-STRUCTURAL instead of lock-argued (today:
   `batch_mu_` + the `!leader_active_` cv wait). The body is today's
   function verbatim (fsync/close old, open/fsync new, MANIFEST
   write+rename+dir-fsync with the WAL-3 anchor comment, the
   post-unlink dir-fsync that stays counted-not-fail-stop per SRC-C,
   H1's `last_ckpt_ts_` preservation). Size rotation
   (`maybe_rotate_segment`) stays writer-internal at the top of each
   drain, exactly where the leader does it today.
8. **Close/shutdown: drain-then-stop-then-join.** `close()` sets
   `stop_requested_`, wakes the writer; the writer finishes terminal
   states for ALL queued items (durable batches complete their I/O — a
   clean close loses nothing, today's property) and exits; the join
   happens before any fd close (`~WalSegments` keeps the
   `FdGuard`/`checked_close` order, with the join FIRST). Producers
   racing close get today's documented contract ("completes against the
   keepalive-held engine or fails cleanly"); queue-full waiters are
   woken with Failed by the stop signal. PITR opens construct no writer
   (they construct no writable WAL at all — `wal_` null or read-only
   per CKV-011; the decision is recorded because `inject_iouring_for_
   test`'s null-`wal_` guard proves the shape exists).
9. **io_uring ladder: SINGLE_ISSUER + DEFER_TASKRUN become
   unconditional; SQPOLL ships flag-gated OFF; queue depth stays 1 in
   slice 1.** The ring's sole issuer is now the writer thread by
   construction, so the setup ladder grows two rungs: probe
   {COOP_TASKRUN | SINGLE_ISSUER | DEFER_TASKRUN} (6.0+) →
   {COOP_TASKRUN | SINGLE_ISSUER} (5.19+) → {COOP_TASKRUN} → plain —
   each falling back independently like every existing rung (the feature
   summary line gains the two flags). SQPOLL ("worth its kernel thread"
   is the roadmap's own gate) ships behind `Options::wal_iouring_sqpoll`
   default OFF with a runtime probe (privileged-kernel check); the arena
   write-mix measures it in slice 3 and the DEFAULT is a measured
   follow-up decision, never a guess. Depth-2 pipelining (next batch's
   write overlapping the current fsync — legal: writes are offset-pinned
   increasing, every fsync covers all prior bytes, and the
   publication barrier, not the ring, defines ack order) is RECORDED as
   the slice-3 measurement candidate, not slice-1 machinery: slice 1's
   I/O shape is deliberately identical to today's (one chain in flight)
   so the rewrite changes WHO executes, not WHAT executes.
10. **Adaptive linger, default start 0.** The writer blocks on the queue
    cv; on wake it may wait up to `linger_cap_us` (default 100 µs,
    tunable) for more same-class arrivals, and does so only when recent
    history pays for it (previous drains averaged ≥ 2 records, or the
    queue already holds ≥ 2 at wake). Linger never applies while
    `failed_`/`stop_requested_`, never delays a drain that would empty
    the queue, and is recorded in diagnostics (`lingers_taken`,
    `linger_us_total`) so the arena can attribute latency. The natural
    batching today's design gets for free (records pile up during the
    leader's I/O) is preserved by construction — linger only adds
    batching in the gap case, which is why it starts at zero.
11. **RECORDED FINDING, no change: Sync and Group are mechanical
    ALIASES at this commit.** `group_append` branches only on
    `i_am_async = (mode == Async)`; Sync and Group take identical batch,
    leader, fsync and ack paths (`durability_` is instance-wide via
    `set_durability`). The README's table ("Sync: every commit / Group:
    per batch of concurrent committers") describes intent the code never
    differentiated — both are "fsync per drained batch, acked after".
    M5 preserves the alias EXACTLY (mode affects only async-vs-durable
    class and diagnostics) and this document is the record; making Sync
    strictly serial-fsync would be a semantic change owing its own
    decision round + README flip (§12.4).
12. **Compile-flag lifeboat, one release, measured deletion.** The
    legacy leader path ships behind `CKV_WAL_LEGACY_LEADER` (default
    OFF = new path) for exactly one release, per the roadmap's own
    sentence. CI runs both legs (the legacy leg is today's suite
    unmoved; the new leg is the default everywhere). The deletion is a
    LATER dedicated commit gated on the roadmap's condition — full DST +
    crash-fuzz + fault matrix re-run on the new path, cited in the
    deletion commit — and per the M5 culture the deletion removes the
    flag too (no permanent dual-path).
13. **Lock-order substitution, documented verbatim.** Everywhere the
    ordering docs say `batch_mu_`, M5 substitutes `queue_mu_` with the
    same edges: `checkpoint_mu_`(shared) → per-key commit mutexes →
    `queue_mu_` → {PhantomTracker `mu_`, PublicationTracker `mu_`
    (leaf), `done_cv_` mutex}. The writer thread takes ONLY `queue_mu_`
    and WAL-internal state — it never takes an engine lock, so no new
    edge can point back at a committer; `await_published` still waits
    without `queue_mu_` held (today's acyclicity argument carries with
    the substitution). The writer's I/O section runs with NO mutex held
    (today's leader unlocks for I/O; the writer never holds `queue_mu_`
    across a syscall).

**The principle in one sentence:** *the committer's contract ends at the
queue — reservation, validation and enqueue stay one atomic producer
step, and everything that touches the WAL file belongs to one thread
whose every failure mode is terminal-for-waiters by construction.*

---

## 2. Why a decision was required (the problem, precisely)

The leader/follower handshake is the engine's densest defect history:
C1 (THREE stacked defects in the leader error path — stranded followers
with `leader_active_` stuck true, a null `cur_batch_` dereference in
`sort`, an EDEADLK re-acquire), H3 (the mixed-durability handoff
orphaning a batch — "a leader will emerge from among them. It will
not"), EXTRA-1 (the reservation-window throw latching fail-stop too
late), CKV-004 (rollback gated on the wrong axis), CKV-012/012R (the
burned-cts wedge and its latch), v24 Fixes 2/4 (leader exception
cleanup, durability-class separation), v25.7 notes 2/3 (byte accounting
under the mutex, the H1 MANIFEST `last_ckpt_ts` brick). Every one is a
bug in the DISTRIBUTED part — the election, the handoff, the shared
batch mutation, the waiter/leader role flip — not in the I/O. M5's
thesis (the roadmap's): a single owner of the file makes that entire bug
class unreachable, and the deletion is the fix.

The performance case is the arena's honest before-column (calibrated
2026-10-17): durable write ratios vs the same-night baseline geomean —
`fillseq` ~0.20–0.22×, `fillrandom` ~0.27×, `ycsb_a` ~0.38–0.41×,
`overwrite` ~0.41–0.44× (night-7 raws: group-durability fillseq 3,985
ops/s vs sqlite-full 9,628, sqlite-normal 38,694). Every committer
today pays: mutex handoff, leader election yield, the possibility of
DOING the I/O itself (the elected leader's latency is the batch's), and
a syscall-per-batch io_uring enter from whichever thread won. A
dedicated writer removes the election entirely, makes the ring
single-issuer (cheaper enters, DEFER_TASKRUN), and lets the linger and
depth knobs exist at all. M5's acceptance is these ratios MOVING with no
unexplained regression elsewhere (roadmap claim 3 — the ledger is the
measured before-column).

## 3. The design (producer / writer / completion)

### 3.1 Producer path (`group_append`'s successor — same signature)

```
lock queue_mu_
  if failed_  -> return {1, 0}                        (today's entry gate)
  while (queue full && !failed_ && !stop) queue_cv_.wait   (BEFORE reserving)
  if failed_ || stop -> return {1, 0} / LifecycleError path as today
  try {
    ts = clock.fetch_add(1, seq_cst)                   (reservation)
    on_reserve(ts) -> conflict => no-op record         (v17 closure, in-window)
    rec = wal_ser(ts, ws-or-noop)
    item = {Batch with rec, class tag}
    queue.push_back(item)                              (enqueue == window end)
  } catch (...) { failed_ = true (under queue_mu_); on_abandon(ts); throw; }
                                                          (EXTRA-1 verbatim)
unlock; wait on done_cv_ for the item's Batch terminal state
  durable: done || failed        async: written || failed   (today's predicates)
return {reserve_conflict ? 2 : (ok ? 0 : 1), ts}
```

### 3.2 Writer loop

```
forever:
  wait on queue_cv_ for (item available || ROTATE cmd || stop || failed_)
  [adaptive linger: same-class arrivals, §1.10]
  if stop && queue empty: exit
  if failed_: drain-fail everything queued (W-STRAND), park
  pop maximal same-class prefix (<= drain caps)  OR  the ROTATE command
  ROTATE -> rotate_after_checkpoint body verbatim; terminal-state the cmd
  batch  -> under NO lock:
              maybe_rotate_segment()                 (size rotation, H1)
              batch_start = lseek(END)               (offset-pinned)
              base_lsn = next LSN run                (writer-exclusive state)
              frames -> batch_buf                    [wal_batch_framed]
              io_uring chain | pwrite fallback       [wal_after_write]
              publish written (release) + notify     (async waiters free)
              fsync / fault-kind honoring            [wal_after_fsync]
              on failure: CKV-004 stage-gated rollback + SRC-B zero-fill
                                                 [wal_before_rollback]
            under queue_mu_:
              flip done/failed, failed_ latch, active_segment_bytes_
              notify_all                             [wal_before_state_flip]
  any throw anywhere -> W-STRAND death-drain (§1.6)
```

### 3.3 What the engine side does NOT do

`commit_txn` is untouched except the call it already makes: same
`group_append(clock_, ws, durability_, on_reserve, on_abandon)`
signature, same WalFailure translation, same CKV-012R idempotent latch,
same install tail (`wal_durable_before_install` seam stays engine-side),
same `pub_.complete(cts)` BY THE COMMITTER (the writer never touches
`pub_` — publication ordering stays the committers' + tracker's job),
same `pub_.await_published(cts)` barrier under `checkpoint_mu_`-shared +
per-key mutexes with the CKV-021 locking note re-verified against the
new shape (§4 WW8).

## 4. Window & hazard analysis (what could break, and what closes it)

* **WW1 — reservation-window move (EXTRA-1/CKV-012 re-derivation).**
  The window's atomicity is `queue_mu_` (today: `batch_mu_`); its END is
  enqueue-complete (today: `has_async` store). Post-window, the item is
  owned: the writer terminal-states it (normal or death-drain), and the
  waiter's predicate covers `failed` — so a reserved cts is either
  WAL-framed or burned, never orphaned; the published prefix can always
  advance. The fail-first teeth: the EXTRA-1/T3 crash-fuzz template
  (batch pressure × AllocFail) re-runs against the moved fire site; the
  post-reservation-throw hook battery re-anchors unchanged (its seam is
  in `commit_txn`'s catch, engine-side).
* **WW2 — writer death (C1-class).** W-STRAND (§1.6): death-drain gives
  every queued waiter a terminal `failed`; the `failed_` latch refuses
  new producers at the gate; D3's fail-stop semantics (reads keep
  working, health level 2) are exactly today's. The old C1 mutants
  (cleanup skipped → followers strand) have NO new-path analogue to
  mutate — that is the point — so the control inverts: the new battery
  KILLS the writer (hooks-gated throw/park seams) and asserts bounded
  WalFailure-everywhere + no-hang, and the mutant "skip the death-drain"
  reddens it (the strand returns).
* **WW3 — close vs in-flight.** Drain-then-stop-then-join (§1.8): a
  clean close finishes durable I/O for everything queued (no acked write
  can be un-framed by close — today's property, now ordered by the join
  BEFORE fd close); racing producers get Failed-or-complete per the
  documented close contract; blocked queue-full waiters are woken by the
  stop signal. The destructor join makes "thread outlives its fds"
  unrepresentable.
* **WW4 — checkpoint/rotation ordering (R-REBASE, H1, WAL-2/WAL-3).**
  ROTATE-as-command makes rotation FIFO-ordered against batches
  (§1.7) — strictly stronger than today's `!leader_active_` cv wait,
  which serializes but does not order against QUEUED batches. The
  rotation bodies (crash points `rot_*`, `seg_*`, `man_*`, `rotc_*`,
  the SRC-C counted-not-fail-stop final dir-fsync with its anchor
  comment, H1's MANIFEST `last_ckpt_ts_`) move WITHOUT EDITS — they run
  on the writer thread instead of the checkpoint caller's, and
  fork-kill crash-fuzz semantics are thread-agnostic (`_exit` kills the
  process at the point regardless of which thread reaches it).
* **WW5 — async `written` publication (CKV-004/D2).** Stage-gated
  rollback carries with zero policy edits (§1.4); `written`'s
  release-store still precedes the fsync; async waiters still return
  Committed on `written` and accept the documented silent-loss contract;
  `async_committed_then_lost` debit unchanged. The SRC-B zero-fill
  machinery (chunked pwrite loop, EINTR/short-write retries,
  fired-witness test slots, the operator-repair WARNING with segment/
  offset/byte facts) moves verbatim into the writer's failure path.
* **WW6 — mixed durability (H3-class).** Class separation is the drain
  policy (§1.4); the orphan is structurally impossible (one owner, one
  death path). The `wal_mixed_handoff` seam's successor is
  `wal_drain_class_switch` (the writer's prefix-cut moment); S1's
  engine-level oracles (mixed-durability crash classes) run unchanged
  over the new seam.
* **WW7 — bounded-queue backpressure vs reservation.** Space is assured
  BEFORE the cts is reserved (§3.1) — backpressure can never burn or
  stall the prefix; a full queue blocks producers exactly where a busy
  leader blocks them today (behind `batch_mu_`/I/O), with the same
  lock-before-reserve ordering vis-à-vis `checkpoint_mu_` and the
  per-key mutexes (a blocked producer holds those, takes no WAL lock
  while cv-waiting; the writer takes no engine lock — WW8's acyclicity).
* **WW8 — `await_published` boundedness (strict-serializable ack).**
  The README/header argument survives verbatim under substitution: cts
  order == enqueue order == drain order (§1.3), so when a commit's own
  batch is durable, every lower cts is PAST its WAL I/O (terminal) and
  only in-memory install tails can be outstanding; those tails are
  lock-disjoint from the waiter by the lock-before-reserve argument
  (CKV-021's two-clause deadlock-freedom proof, with `queue_mu_`
  substituted for `batch_mu_` — the writer never appears in it because
  the writer never waits on an engine lock). The hard-timeout regression
  test (append-only opens seeding the prefix) re-runs unchanged.
* **WW9 — crash-fuzz equivalence.** All five `wal_*` crash points keep
  their NAMES and stage positions (they name the durability state
  machine, not the thread shape); the every-point-reached assertion
  binds reachability on the new path; the plan grammar's windows
  (GC×checkpoint, rotation×dir-fsync, pressure×AllocFail, big-record×
  write-fault) fire against producer or writer sites as mapped in §11.
* **WW10 — sanitizer/lock surface.** The TSan surface strictly shrinks:
  today ANY committer can become the leader (roles swap at runtime);
  M5 has one fixed I/O thread, one producer mutex, two cvs. The full
  four-config matrix + both compile-flag legs run before the default
  flips in CI (slice plan §8); S1/S2 re-anchor per §11 (rule 14: the
  scenarios are renamed/re-pointed with the finding IDs cited at the
  scenario sites, never silently dropped).

## 5. Invariants (carried, and the one that is new)

| ID | Fate under M5 |
| --- | --- |
| D1 (acked ⇒ durable per class) | carried: terminal states + stage semantics unchanged (§1.5) |
| D2 (WalFailure'd batch absent after ANY crash) | carried: rollback policy + durable truncate-fsync + SRC-B fill move verbatim (WW5) |
| D3 (fsync-error fail-stop) | carried: `failed_` latch points identical (producer window, writer state flip, rotation failures); SRC-C's single deliberate exception untouched |
| R-REBASE | STRENGTHENED: FIFO command ordering (§1.7, WW4) |
| I2 (recovery sees every cts exactly once) | untouched: no format change, reservation contiguity argument re-derived (WW1) |
| EXTRA-1 | carried verbatim by §1.2's wholesale window move |
| CKV-004/012/012R | carried (WW1/WW5); CKV-012's window-end re-anchored |
| Triple identity cts-order == WAL-order == phantom-order | STRUCTURAL (§1.3) — re-proven by the full matrix per the roadmap, with TXN-1's "phantom-order carries every write" content intact |
| **W-STRAND (new)** | writer failure is always terminal-for-waiters, never silent (§1.6); checker: the writer-death battery + the death-drain mutant |

## 6. What does NOT change

On-disk everything: frames, segments, MANIFEST, recovery/replay, the
torn-tail/CRC/interior-hole classification (WAL-1/CKV-019), LSN
continuity, cts gap/duplicate detection — the writer emits
byte-identical WAL. `PublicationTracker` (prefix, burn, `await_
published`). `commit_txn`'s shape. The async API's `std::async` layer.
Checkpoint internals. The fault-kind taxonomy and fired-witness
patterns. `wal_stats()` (extended, never renumbered). io_uring wrapper
internals except the setup-flag ladder (§1.9) — registered buffers/
files, hard-linked chains, LINK_TIMEOUT deadline, degradation tracker
(review-F1), the sync-pwrite fallback all run unchanged ON THE WRITER
THREAD.

## 7. Acceptance mapping (ROADMAP §M5, verbatim legs)

| Leg | How it is met |
| --- | --- |
| arena "after" tables: no regression at any concurrency, measurable win at 8+ committers | nightly arena vehicle (before-column banked 2026-10-17); slice-3 runs; band-update policy applies to any movement (dedicated commit citing M5) |
| C1/H3-class reversion probes fail loudly | W-STRAND battery + death-drain mutant (WW2); class-switch seam + S1 re-anchor (WW6); the legacy-flag leg keeps the OLD probes runnable for the reversion comparison until deletion |
| crash-fuzz green | all crash points mapped with names kept (WW9); the every-point-reached assertion is the anti-rot gate |
| dead code gone | the deletion commit (post-flag-release, matrix cited) removes leader/follower entirely — this document's §11 inventory is the checklist |
| triple identity re-proven by the full matrix | lincheck + DST + stress + fault matrix on the new path, both flag legs, before default flip |

## 8. Slice plan

* **Slice 1 [L] — the rewrite, behind the flag.** Queue + writer thread
  + producer path + W-STRAND + ROTATE command + seam/hook/crash-point
  mapping (§11) + `CKV_WAL_LEGACY_LEADER` + controls: writer-death
  battery (kill/throw/park seams; bounded WalFailure; no-hang),
  class-switch control (mixed durability drain order + async written-
  stage semantics), backpressure control (full queue blocks BEFORE
  reservation — anti-hole assertion), close-drain control (clean close
  frames everything queued; racing producer gets Failed), EXTRA-1/T3
  battery re-run on the moved window, append-only prefix-seed hard-
  timeout regression, recovery differential across the flag legs.
  Mutants: NO-DEATH-DRAIN (strand returns → battery RED),
  NO-CLASS-PREFIX (mixed batch → CKV-004 control RED), RESERVE-BEFORE-
  SPACE (backpressure hole → control RED), SKIP-ROTATE-CMD-ORDER
  (R-REBASE probe RED). Full suite × {asan, tsan, stress, release} ×
  {new, legacy} legs; DST + crash-fuzz + fault matrix on the new leg.
* **Slice 2 [M] — the ring + the linger.** SINGLE_ISSUER/DEFER_TASKRUN
  rungs (probe ladder + feature-summary line), SQPOLL flag-gated OFF
  with the privilege probe, adaptive linger with diagnostics,
  linger-sensitivity controls (latency vs batch-fill at 1/4/16
  committers, in-suite, bounded). Mutant: linger-ignores-stop (close
  latency control RED).
* **Slice 3 [M] — the measurement.** Arena after-tables on the nightly
  vehicle (write mixes at the pinned geometry + an 8/16/32-committer
  concurrency sweep — the "measurable win at 8+" leg needs the sweep,
  recorded as an arena-adjacent one-off, methodology-header per rule 9);
  depth-2 pipelining measured and decided (ship or record-refuse);
  SQPOLL measured and default-decided; band updates per policy.
* **Deletion (next release boundary).** Legacy path + flag removed in
  one dedicated commit citing the matrix re-runs; §11's inventory rows
  for leader-era seams/hooks/tests execute their rule-14 dispositions
  (the ones marked RETIRE retire WITH the code they probe; the ones
  marked RE-ANCHOR are already re-anchored in slice 1).

## 9. Named risks (recorded, not hidden)

* **R-1 slice-1 blast radius:** the writer touches every durability
  test's execution thread. Mitigation: identical I/O shape (depth 1,
  same stages, same crash-point names), both flag legs in CI for the
  whole slice, and the suite's existing fail-first batteries are the
  regression net (they probe contracts, not thread shapes — the ones
  that DO probe shape are inventoried in §11 with dispositions).
* **R-2 linger latency regression at low concurrency:** a badly-tuned
  linger taxes single-committer p99. Mitigation: default 0, adaptive
  gate requires evidence of arrivals, slice-2 sensitivity controls.
* **R-3 arena vehicle availability** for slice 3 (§12.1 — shared with
  M4's soak leg; §13 carries the recommendation).
* **R-4 the 2-CPU sandbox's thread budget:** the writer is ONE more
  thread per open database; DST scenarios fork per child (one writer
  each) — recorded, no action (the gc thread precedent).
* **R-5 `WalSegments` lifetime vs the writer:** the thread references
  `this`; join-before-fd-close (§1.8) plus the destructor join make
  use-after-destroy unrepresentable, but any future early-return path
  in open() that constructed the writer must join it — checklist item.

## 10. Checklist the implementation owes this document

1. Every §11 inventory row executed (deletion, successor, or re-anchor
   with the rule-14 citation at the site).
2. Lock-order comments updated at all four documented sites (the
   CKV-021 note, the CKV-012 window comment, `await_published`'s
   header, the memory-ordering audit block) — `batch_mu_` → `queue_mu_`
   substitution is a DOC change with the same force as the code change.
3. `wal_stats()` extended (queue depth/high-water, drain sizes,
   lingers, class switches, backpressure waits/time, writer deaths
   which must read 0 outside controls) — the arena and the churn tables
   read them.
4. Both flag legs green in CI before the default is anything but the
   new path; the legacy leg's sunset is the deletion commit, not a
   silent CI drop.
5. The feature-summary line (io_uring banner) gains the new rungs — the
   suite prints it at the top of every run; a silent ladder regression
   is a visible diff.

## 11. Inventory: every leader-era artifact and its disposition

| Artifact (at `92ffaba`) | Disposition |
| --- | --- |
| `leader_active_`, `cur_batch_`, `pending_`, leader election + waiter loop in `group_append` | DELETE (§1.1) |
| `batch_mu_` / `batch_cv_` | SUCCEED: `queue_mu_` (producer+state) / `queue_cv_` (writer wake, producer space) / `done_cv_` (waiter completion) |
| `Batch` struct (`records`, `has_async`, `written`, `done`, `failed`) | KEEP as the drain unit |
| leader's `std::sort` by cts | DELETE (FIFO-structural, §1.3) |
| `maybe_rotate_segment`, `open_segment`, `write_manifest`, `rotate_after_checkpoint` bodies | KEEP verbatim; execution moves to the writer; `rotate_after_checkpoint` call shape → ROTATE command (§1.7) |
| SRC-B zero-fill block + SRC-E4 witnesses + operator WARNING | KEEP verbatim inside the writer's failure path |
| crash points `wal_batch_framed/after_write/after_fsync/before_rollback/before_state_flip`, `rot_*`, `seg_*`, `man_*`, `rotc_*` | KEEP names + stage order (WW9) |
| stress seams `after_cts_reserve` (producer), `wal_durable_before_install` (engine) | KEEP in place |
| stress seams `wal_leader_elected`, `wal_leader_done`, `wal_mixed_handoff` | SUCCEED: `wal_writer_wake`, `wal_writer_batch_done`, `wal_drain_class_switch`; NEW: `wal_enqueue_full` |
| hooks `leader_pre_drain_hook_`, `leader_post_unlock_hook_`, `force_next_leader_rotation_` | SUCCEED: `writer_pre_drain_hook_`, `writer_io_throw_hook_`, `force_rotation_for_test` → ROTATE-command enqueue; NEW: `kill_writer_for_test` / `throw_in_writer_for_test` (W-STRAND battery) |
| test accessors taking `batch_mu_` (`active_segment_bytes_for_test`, `set_segment_max_bytes_for_test`, `leader_active_for_test`) | first two re-anchor to `queue_mu_`/writer-safe reads; `leader_active_for_test` RETIRES with the leader (its consumers — the C1 battery — re-anchor to W-STRAND per rule 14, citation at the test site) |
| DST S1 (mixed durability, H3) | KEEP oracles; comment re-anchors `wal_mixed_handoff` → `wal_drain_class_switch` (rule-14 note citing this doc) |
| DST S2 (leader hang, C1) | RE-ANCHOR: the no-strand oracle binds the writer via the death seams; scenario renamed in place with the C1 citation kept (rule 14 — flipped/re-pointed, never deleted) |
| fault kinds + fire sites (`AllocFail` in-window; write/fsync/truncate/seg-open kinds at I/O sites) | KEEP kinds; fire sites move with their windows (WW1/WW5); fired-witness anti-vacuity asserts re-run per kind |
| C1/CKV-012/CKV-004/EXTRA-1/SRC-* regression batteries | KEEP contracts; execution-thread notes updated; shape-probing assertions re-anchor per their rows above |

## 12. Open questions (maintainer decisions this document defers)

1. **Arena/soak vehicle** (shared with M4 §12.2 and v30 M1): §13
   carries the recommendation (rolling nightly chain) — it needs the
   maintainer's adoption because it edits the nightly workflow's
   contract and budget (rule 13: the segment budget ships with a
   measured run or an explicitly-generous cap + tighten-follow-up).
2. **SQPOLL default:** flag-gated OFF at slice 2, arena-decided at
   slice 3 — agreed, or does the maintainer want it out of scope
   entirely (the roadmap names it, so this document keeps it in)?
3. **Queue bound defaults** (4096 items / 32 MiB): fine, or tie the
   byte bound to `RLIMIT_MEMLOCK`/pool sizing?
4. **Sync-vs-Group alias** (§1.11): record-and-preserve (this
   document's position), or commission the semantic split (strict
   serial-fsync for Sync) as its own decision + README flip?
5. **Flag name** `CKV_WAL_LEGACY_LEADER` and the one-release sunset:
   confirm the release boundary is 0.29.0 (M7) → deletion lands in the
   v30 arc.

## 13. Soak/arena vehicle — recommendation (closes M4 §12.2, unblocks
##     M5 slice 3 and v30 M1's 24h/7d legs)

**Recommendation: the rolling nightly chain with persisted state and an
aggregate verdict** — the only one of the roadmap's three options that
needs no maintainer hardware, secrets, or offline rig, and the engine
already ships the state-transfer primitive the chain needs: `backup()`
is a consistent, self-verifying copy (invariant B1 — per-file size +
CRC32 marker, `verify_backup` gate) and restore is "point Options at the
copy".

Shape: the existing nightly grows a `soak` job running one ~4.5 h
segment (rule 13: the cap ships generous with a measured tighten-after
follow-up in the job itself); the segment opens the prior night's backup
artifact (or a seeded genesis DB for segment 1), runs the delete-heavy
P1 workload + concurrent scanners under the segment's config — release,
ASan+UBSan, and TSan rotating by night (the roadmap's "interleaved"),
stress-seeded where the config allows — then `backup()`s, verifies, and
uploads. A committed ledger row per segment (run id, config, seed,
high-water, merge/gc counters, verdict) makes the aggregate verdict a
readable artifact: 7 consecutive green segments with FLAT
pool-high-water = P1's soak leg PASSED; a missing or red night RESTARTS
the chain loudly (never silently skips — the chain's continuity IS the
7-day claim). Cost: one nightly slot; no new infrastructure; every
number published. The self-hosted runner (true continuous 7 days) and
the offline rig (which v30 M3's dm-flakey power-loss work needs anyway)
are recorded as the upgrades, not the blockers.
