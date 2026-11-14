# Observer notification dispatcher — work order (finding #7b, 2026-11-14)

Status: WORK ORDER (a threading milestone — M6's own reentrancy decision —
not a same-commit patch). The spec is the deliverable.

**The contract today (all sites verified in-tree).** Observers fire
INLINE on the committing thread: `Database::notify_observers(ws)` →
`notify_state(ObserverState*, ws)` (chronokv.hpp, the v25.1 M1.5 block)
walks the write-set under `st->mu` and calls each matching callback with
`old_val == nullopt` (blind-write path). Containment is CKV-007
(v28): a throwing callback is caught, counted in
`diag::observer_exceptions`, delivery continues — a callback throw can
never invert the commit result at the sync caller or rethrow at the
async future (CKV-016's shared rule). The registry lives in a shared
`ObserverState` that MOVES with the Database (API-3; handles hold weak
references — the v25.7 move-safety fix). The recorded limitation
(README "Observer reentrancy", main class comment's M6 note at the
notify_state block): re-entering the observer machinery from inside a
callback — `observe()` from a callback, or an observer whose callback
calls `db.put()` (whose commit notifies again) — DEADLOCKS on
`st->mu` (non-recursive).

**Goal.** Move callbacks OFF the committing thread to a dedicated
dispatcher thread; the commit path enqueues `(callback, key, new_val)`
pairs and returns. Preserve, in order:
1. CKV-007 containment (a throwing callback counted, never inverts
   commit) — the enqueue side must not throw either (a full queue is a
   DROPPED-and-counted event, never a failed commit; bounded queue,
   drop-count diagnostics — the writer thread's queue-bounds precedent
   is the shape).
2. API-3 move-safety (the dispatcher holds the shared `ObserverState`,
   not the Database).
3. Ordering: per-key FIFO in commit order (the queue is the same FIFO
   the WAL writer rides — the M5 slice-1 ordering argument applies
   verbatim); cross-key ordering best-effort and DOCUMENTED as such.
4. Latency change documented with queue-depth diagnostics
   (`observer_queue_depth_max`, `observer_drops`) — this is a measured
   change to every commit's fast path (the arena's ycsb legs are the
   before/after instrument).

**The reentrancy decision (define it, then test it).** Allowed:
- a callback calling `observe()` (REGISTER) — registration is a
  vector push under `st->mu` on the DISPATCHER thread now; a callback
  registering another observer must not deadlock. Requires: the
  dispatcher must call callbacks WITHOUT holding `st->mu` (snapshot the
  matched callback list under the lock, release, invoke — the
  tombstone-never-erase rule already keeps indices stable).
- a callback calling `unregister` on ITSELF (the handle's lambda) —
  tombstoning is lock-taken briefly; safe from the dispatcher thread.
- a callback calling `db.put()` (a write that itself notifies) — the
  reentrant notification enqueues behind the current drain: FIFO,
  bounded, and documented as "reentrant delivery is deferred, never
  lost, never deadlock". A depth cap (default 1: one level of
  reentrancy) turns runaway recursion into a counted drop.
Forbidden (asserted): `close()`/`~Database()` waiting on an empty queue
while a callback waits on the queue's lock — close() must quiesce the
dispatcher BEFORE destroying `ObserverState` (the streams' keepalive
pattern is the template; a callback in flight holds a shared_ptr to
the state).

**Fail-first battery (CKV_ONLY_OBS7=1, the house forked style).**
- Reentrant self-unregister + throw: an observer whose callback
  unregisters itself then throws — delivery continues, exit clean,
  `observer_exceptions` == the throw count, no deadlock (forked child
  with a deadline, the C1 idiom).
- Reentrant `observe()` from a callback: new observer receives LATER
  notifications, no deadlock.
- Reentrant `db.put()` from a callback: the nested notification is
  delivered (deferred), the outer commit's result UNCHANGED (OK), queue
  depth bounded.
- Commit-result invariance under a throwing observer: commit() returns
  OK (the CKV-007 control re-run on the dispatcher).
- Ordering: N commits from one thread deliver their observer events in
  commit order (the FIFO control).
- Close-while-dispatching: `close()` with a callback in flight exits
  clean (no use-after-free — ASan leg).

**Sequencing note.** M4's documentation overhaul already owes the
failure-mode catalog; the dispatcher's drop/depth diagnostics join it
there. The arena's txn series (landed 2026-11-14, #6) give the
before/after latency instrument for the enqueue-path change — measure
on the calibration dispatch, record in the decision doc, then land.
