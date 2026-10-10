# ChronoKV

[![CI](https://github.com/majnoon40/ChronoKv/actions/workflows/ci.yml/badge.svg)](https://github.com/majnoon40/ChronoKv/actions/workflows/ci.yml)

**ChronoKV** is an embedded, multi-version (MVCC) key-value store with
serializable transactions, written as a single C++20 header
(`chronokv.hpp`). It pairs a write-ahead log (WAL) with optional
checkpointing for durability, serializable-snapshot-isolation (SSI)
transactions with phantom detection, a paged B+ tree index, and
io_uring-accelerated WAL writes — all in one header with no external
dependencies.

Current version: **0.28.1** (`CHRONOKV_VERSION` in `chronokv.hpp`) — the
**Audit-2 remediation arc**: the second external adversarial round over
0.28.0, shipped with a fail-first regression test per fix (the Audit-2
battery in `CKV_ONLY_REMEDIATION=1 ./build/release/test`): TXN-1 (SSI
value-antidependency — the phantom tracker now records every committed
write, not only existence flips; no-op deletes of absent keys stay
exempt), TXN-2 (negative lookups no longer materialize index entries),
TXN-3 (close-with-live-txn no longer pins reader state), WAL-1…WAL-4
(torn-tail classification that never trusts a damaged length field,
segment-creation dir fsync, the wrong-directory rotation fsync, bounded
recovery loops), BT-1/BT-2 (pre-mutation split headroom; standalone-tree
writer safety), EXTRA-1/EXTRA-2 (reservation-throw latch inside
group_append; uint16 slab-offset wrap), API-1/API-3 (a TRUE single
header — `#pragma once` + inline, CI-gated; observers survive a Database
move), plus the v29 CI hardening (lint gates, -Wconversion ratchet,
shared build artifacts, experimental sanitizer legs). One deliberate
semantic change: updating a present key inside a concurrently scanned
range now conflicts (v17 guard test 4 flipped — its old expectation
encoded the TXN-1 anomaly). The **v28 audit-remediation arc is
complete**: all 21 findings of the
`docs/request.txt` adversarial audit (CKV-001…CKV-021) plus the external
review's two platform defects are resolved or verified-with-tests, each
with a fail-first regression test in the remediation battery
(`CKV_ONLY_REMEDIATION=1 ./build/release/test`). Highlights of the arc:
byte-aware plan-before-mutate B+ tree splits with exact OLC mutation-epoch
fences, loud failure on interior-hole WAL segments (no silent truncation,
no LSN reseed), strictly read-only PITR opens, (dev,ino)-keyed io_uring
registered files, contained observer/async exceptions, and an io_uring
availability definition that matches reality on blocked-ops kernels. The
**v27 arc** (0.27.0) shipped coverage aimed at error paths. The version's MINOR tracks the roadmap arc: the
v26 arc (M0–M4) shipped across 0.25.4–0.25.8 plus patches 0.26.1–0.26.3,
which also carried v27 M0–M2 (the `929cb00`/`6d8a13d` review responses,
the deterministic scheduler with proven C1/H3 catch, the completed
strict-serializability checker, and the seed-scaled CI jobs). 0.27.0 ships
**v27 M3** — coverage aimed at error paths: a gcov job with per-fault-kind
forced runs, publishing each kind's unique error-path contributions and a
ledger of the error-path lines no run reaches ("untested lines, and that
is exactly where the last four bugs were"). v28's gate ("do not start
before the DST harness is green") is met.

## Highlights

- **Single-header engine** — drop `chronokv.hpp` into any C++20 project;
  the repository's `main.cpp` is the full engine test suite.
- **Serializable transactions (SSI)** — snapshot reads with write-set
  validation, including phantom detection over registered scan ranges
  (no write skews, no phantoms).
- **Strict-serializable acknowledgements (v27 M1, completed in 0.26.3)** —
  a commit is acknowledged only once the published prefix covers its cts,
  so any snapshot taken after an ack necessarily includes that write
  (real-time order). The in-suite `lincheck` checker verifies snapshot
  soundness, real-time order, Elle-style list-append properties, exact
  range-scan snapshot consistency (`check_scans`), and order-insensitive
  set algebra (`check_set_adds`) on recorded histories — covering the
  sync, transactional, async, batch and scan APIs — and it found the
  publication-prefix lag it now guards on its first engine run.
- **Reproducible concurrency testing (v27 M0)** — under `CHRONOKV_STRESS`,
  a seeded baton scheduler (`dst::`) turns every instrumentation point in
  the WAL group-commit, GC/epoch-vs-scan and B+ tree cursor-vs-split paths
  into a deterministic scheduling decision; failures reproduce from
  `(seed, op-count)`. The fork-isolated harness runs four scenarios
  (including the C1 leader-hang and H3 mixed-durability-crash classes) at
  100 seeds/scenario on PRs and 100k nightly, and its acceptance was proven
  by reintroducing both historical bugs on scratch trees: the harness
  caught the C1 hang and the H3 SEGV every run.
- **Crash-safe WAL** — CRC-checked, segmented (64 MiB) WAL with torn-tail
  truncation (interior corruption fails loud instead of being silently
  cut), batch group-commit, per-segment LSN continuity validation, and
  strict commit-timestamp gap/duplicate detection on replay.
- **Three durability modes** — `Sync` (fsync per commit), `Group`
  (default; batched fsync, durable against power loss), `Async`
  (durable against process crash only).
- **io_uring WAL writes** — modern kernel io_uring backend with a
  runtime feature ladder: probed `IORING_SETUP_COOP_TASKRUN` rings
  (5.19+), registered fixed buffers (`WRITE_FIXED`, sized to
  `RLIMIT_MEMLOCK`), registered fixed file slots, one hard-linked
  `write -> [LINK_TIMEOUT 500 ms] -> fdatasync` SQE chain per durability
  batch (guaranteed write-before-fsync ordering, a single
  `io_uring_enter`, kernel-enforced deadline), plus a sync `pwrite`
  fallback wherever io_uring is unavailable (e.g. seccomp-restricted
  containers, `CKV_IOURING_DISABLED` compile-out).
- **Paged B+ tree index** — 4 KiB pages with a 256 MiB (configurable)
  page pool, per-page shared latches, and incremental range-scan cursors.
  Hazard-pointer slots are maintained per tree and recycled (bounded by
  peak cursor concurrency); page *reclamation* is not implemented yet —
  see Known limitations.
- **Lock-free reclamation of MVCC versions** — epoch-pinned deferred GC of
  old *versions*; no GC-vs-scan lock contention on the read path. (This
  covers version chains only, not B+ tree pages — see Known limitations.)
- **Rich public API** — sync + async operations, atomic batches,
  prefix observers, streaming range scans, and read-only health/stats
  diagnostics.
- **Online backup** (v26 M3) — `Database::backup(dest_dir)` copies a
  consistent snapshot (checkpoint + WAL artifacts) while the database
  stays online, with a self-verifying `BACKUP_COMPLETE` marker
  (per-file size + CRC32) written last; `Database::verify_backup(dir)`
  validates a copy without opening it. Restore = point `Options` at the
  copy. Invariant **B1**.
- **Point-in-time restore** (v26 M4; mid-window policy refined by the
  `6d8a13d` review) —
  `Options::pitr_as_of_cts` recovers a database directory to an exact cts
  boundary (read-only open), and `Database::restore_pitr(...)` materializes
  a **writable** as-of database into a fresh directory. The as-of window of
  a live/crashed directory is the WAL beyond its last checkpoint; a
  `backup()` copy restores to exactly its marker cts (`Database::backup_cts`).
  An `as_of` that falls *between* checkpoint boundaries is recovered by
  per-entry filtering of the first delta beyond the boundary **while the
  window's WAL survives**; once later checkpoints have rotated that WAL
  away, a filtered open is refused loudly instead of risking a silently
  wrong snapshot (a mid-window rewrite and a first write after `as_of`
  leave byte-identical artifacts). See Safety properties for the exact
  reconstructability rules.
- **Heavy-duty validation** — engine tests, B+ tree fuzzing, fault
  injection, deterministic stress mode, the v27 M0 deterministic-scheduler
  (DST) harness, randomized crash-point fuzzing, a strict-serializability
  history checker (v27 M1 `lincheck`, with synthetic anomaly batteries
  proving every violation kind fails when it should), a linearizability
  history recorder, and per-fault-kind error-path coverage with an
  untested-lines ledger (v27 M3) — all run under a four-config sanitizer
  matrix (Release / ASan+UBSan / TSan / Stress) plus the gcov build.
- **Crash-consistent recovery** — 20 instrumented crash points cover the whole
  durability state machine (WAL leader, segment rotation, MANIFEST
  write/rename, checkpoint, rebase). The fuzzer forks a child, kills it at a
  seeded point, then reopens and asserts the database is recoverable with no
  lost acknowledged write and no resurrected rejected one. It also asserts
  *every* crash point is actually reached, so coverage cannot silently rot.
  Note this exercises the recovery state machine, not power loss — `_exit()`
  does not discard the kernel page cache (see Known limitations).

## Requirements

- Linux (uses `io_uring`, `flock`, `fork`-based tests)
- C++20 compiler — tested with g++ 12/13/14, clang 18, g++ 15.2
- pthreads

## Quick start

```cpp
#include "chronokv.hpp"

int main() {
    chronokv::Options opts;
    opts.wal_dir        = "/tmp/mydb/wal";        // "" = in-memory only
    opts.checkpoint_path = "/tmp/mydb/ckpt";      // "" = no checkpoints
    opts.durability     = chronokv::DurabilityMode::Group;

    auto db = chronokv::Database::open(opts);

    // Simple single-key operations (blind writes)
    db.put("k1", "v1");
    auto val = db.get("k1");                     // std::optional<std::string>
    db.erase("k1");

    // Serializable transaction
    auto txn = db.begin();
    if (auto v = txn.get("counter")) {
        txn.put("counter", std::to_string(std::stoi(*v) + 1));
    }
    chronokv::Status s = txn.commit();           // Status::Conflict on write skew

    // Atomic batch (single commit, no isolation overhead)
    auto batch = db.create_batch();
    batch.put("a", "1");
    batch.put("b", "2");
    batch.erase("c");
    batch.commit();

    // Snapshot range scan [lo, hi] (inclusive)
    for (auto& [k, v] : db.range_scan("a", "z")) { /* ... */ }

    // Prefix observer (RAII handle; fires on put/erase/batch commits)
    auto h = db.observe("watch:", [](const std::string& key,
                                     const std::optional<std::string>& old_val,
                                     const std::optional<std::string>& new_val) {
        // ...
    });

    db.checkpoint();
    db.close();
}
```

Online backup (v26 M3):

```cpp
db.backup("/mnt/nas/ckv-bak");           // consistent copy, DB stays online

std::string reason;
if (!chronokv::Database::verify_backup("/mnt/nas/ckv-bak", &reason))
    std::cerr << "backup invalid: " << reason << "\n";

// Restore = open a fresh instance against the copy:
chronokv::Options ro;
ro.wal_dir         = "/mnt/nas/ckv-bak/wal";
ro.checkpoint_path = "/mnt/nas/ckv-bak/ckpt";   // basename of your checkpoint_path
auto restored = chronokv::Database::open(ro);
```

Point-in-time restore (v26 M4) — undo durable-but-wrong writes by
recovering a database directory to an exact commit timestamp:

```cpp
chronokv::Options po = opts;
po.pitr_as_of_cts = good_cts;          // from published_watermark()/diagnostics
auto view = chronokv::Database::open(po);   // READ-ONLY as-of view
auto v = view.get("counter");               // writes return Status::Failed
view.close();

// Materialize a WRITABLE as-of database in a fresh directory:
auto db = chronokv::Database::restore_pitr(
    opts.wal_dir, opts.checkpoint_path, "/tmp/restored", good_cts);
db.put("counter", *v);                      // writable again
```

The boundary must not precede the checkpoint base's cts (older state was
superseded — `open` fails loud). For a `backup()` copy the only valid
boundary is its marker cts (`Database::backup_cts(dir)`): backups
checkpoint before copying, so they hold exactly one point in time.

Mid-window boundaries (v26.1): an `as_of` *between* checkpoint boundaries
is recovered from the checkpoint chain plus whatever WAL still covers the
window. The first delta beyond the boundary contributes its entries with
`cts <= as_of` (per-entry filtering); a later checkpoint's rotation may
already have unlinked the segments covering the window. Rules, in short:
boundary cts values always work; a mid-window cts works when the window's
WAL survives (the filtered delta and the surviving records dedupe against
each other); and any window the WAL no longer covers fails loud at open —
partially rotated (a surviving record witnesses the hole) or fully rotated
with filtered delta entries (the `6d8a13d` review: a skipped post-`as_of`
entry is indistinguishable from a lost mid-window rewrite, so serving the
best-effort snapshot could be silently wrong). Never silently wrong — see
Safety properties.

Compile:

```sh
g++ -std=c++20 -O2 -I. my_app.cpp -o my_app -lpthread
```

## API overview

| Surface | Entry points | Notes |
| --- | --- | --- |
| Database | `open/get/put/erase/range_scan/begin/checkpoint/close` | move-only; ops on a closed DB throw `LifecycleError` |
| Transactions | `get/put/erase/range_scan/commit/abort` | SSI; read-your-writes over the snapshot; must commit or abort before destruction |
| Batch | `create_batch()` → `put/erase/commit` | atomic write-set commit, cheaper than a transaction |
| Async | `put_async/get_async/erase_async` | `std::future`-based; errors via `Result<T>` / `Status` |
| Streams | `RangeScanStream::has_next/next` | incremental B+ tree cursor; per-page snapshot consistency |
| Observers | `observe(prefix, callback)` | inline callbacks on the committing thread; fired by `put/erase/Batch::commit` **and `Transaction::commit`** (v25.7); `old_val` is always `nullopt`; a throwing callback is **contained** (counted in diagnostics) and never inverts the commit result (v28 CKV-007) |
| Backup | `backup(dest_dir)` / `verify_backup(dest_dir, reason*)` / `backup_cts(dest_dir)` | v26 M3; requires `checkpoint_path`; restore by opening `Options` against the copy |
| PITR | `Options::pitr_as_of_cts` / `restore_pitr(src_wal, src_ckpt, dest, as_of)` | v26 M4; PITR opens are read-only; `restore_pitr` materializes a writable as-of DB in a fresh directory |
| Diagnostics | `wal_stats/gc_stats/epoch_stats/health/published_watermark` | read-only snapshots of engine counters |

**Status codes** (`chronokv::Status`): `OK`, `Conflict`, `TooLarge`
(values are capped just under 1 MiB by the WAL framing; keys are capped at
`TREE_MAX_KEY_BYTES` = **4048 bytes** — the B+ tree page-safe bound derived
from the 4 KiB page layout: 4096 − 32 header − 8 leaf slot − 8 encoded-pointer
value; the WAL framing itself would allow 64 KiB keys, but a key that
cannot fit a page is rejected before any reservation, WAL write, or index
mutation), `InvalidTransaction`, `InvalidState`, `WalFailure`, `Failed`.

**Exceptions**: recoverable conditions return `Status`; unrecoverable ones
throw — `CorruptionError` (on-disk data), `LifecycleError` (closed DB /
inactive transaction), `Error` (engine failures), `NotYetImplementedError`.

**Durability modes** (`chronokv::Options::durability`):

| Mode | fsync behavior | Survives |
| --- | --- | --- |
| `Sync` | every commit | power loss |
| `Group` *(default)* | per batch of concurrent committers | power loss |
| `Async` | none | process crash only |

### Safety properties

- Opening the same `wal_dir` twice concurrently throws (advisory `flock`
  inter-process guard).
- Recovery verifies CRCs, truncates torn WAL tails (a tail of garbage at
  EOF only — an interior hole with parseable records after it is CORRUPT
  and fails loud, never silently truncated), validates per-segment LSN
  continuity, rejects commit-timestamp gaps/duplicates on replay, and
  validates the checkpoint chain.
- Invariant **D2**: a batch for which any caller observed `WalFailure` is
  absent from the WAL after *any* crash, not merely after a clean restart —
  the rollback truncation is itself fsynced, so it cannot be undone by a
  power loss. **Known indeterminate window (SRC-B, reproduced 2026-10-10,
  contract decision pending):** if the rollback `ftruncate` *itself* fails,
  the rejected batch's CRC-valid frame may remain in the WAL and be
  **replayed on the next open — on a clean restart, no power loss
  involved**; if the rollback's *fsync* fails, the guarantee degrades to
  clean-restart durability (only a power loss can undo the truncation).
  Both sub-cases fail-stop the instance (D3), log `WARNING`/`FATAL`,
  count in `wal_stats().truncate_fails`, and — since the 2026-10 option-4
  fix — add a distinct `rollback-indeterminate` reason to `health()`
  (in-memory only; it does not survive the reopen). Recovery cannot
  distinguish the poisoned tail from legitimate data, so the window
  survives the reopen; zero-fill neutralization (probe: converts the tail
  into the ordinary torn-tail shape) and a persisted marker remain under
  maintainer decision. See `docs/audits/2026-10-src-audit-findings.md`
  (addendum) for the reproduction, probe outputs and options.
- Invariant **D3**: once any fsync on the WAL path returns an error the
  instance fail-stops — `failed_` latches permanently, writes return
  `Status::Failed` (`TxnResult::DatabaseFailed`), reads of already-durable
  data keep working, and `health()` reports level 2 with reason
  `"wal fail-stop mode active"`. A later *successful* fsync can therefore
  never be mistaken for evidence that earlier data survived; this is the
  property that makes the Linux fsync-error semantics (the "fsyncgate"
  family) safe here. Every fsync on both rotation paths is checked, with
  exactly ONE deliberate exception (SRC-C, pinned by test): the post-unlink
  directory fsync that finishes a checkpoint rotation is counted
  (`wal_stats().fsync_fails`) and warned but does *not* fail-stop — the
  MANIFEST rename is already durable at that point, the call only persists
  the unlink of checkpoint-covered segments, and recovery tolerates their
  resurrection (records ≤ `ckpt_ts` are deduped on replay).
- A `Transaction` destroyed while still active calls `std::abort()` —
  commit or abort explicitly. Concurrent `Database::close()` with live
  transactions requires external synchronization.
- **Lifecycle of auxiliary handles (v25.7, race-free since v25.8)**:
  every public API call is safe to race against `Database::close()` —
  `close()` and all engine access are serialized by an internal mutex,
  and each call either completes against a keepalive-held engine or
  fails cleanly (`LifecycleError` / `Status::Failed`). `RangeScanStream`
  iteration after close throws and destroys cleanly; `ObserverHandle`s
  may outlive the `Database` object; transactions pin the engine for
  their lifetime. Because streams/transactions hold engine keepalives,
  `close()` **defers engine teardown and the WAL flock release** until
  the last such handle dies. Destroying the `Database` *object itself*
  while calls are in flight still requires external synchronization.
- **PITR opens are read-only (v26 M4)**: with `pitr_as_of_cts` set,
  writes return `Status::Failed`, `checkpoint()` throws, and `health()`
  reports level 1 with the mode — the WAL still holds records newer than
  the boundary, and appending after them would collide on cts. Use
  `restore_pitr()` to obtain a writable as-of database. A PITR open does
  not modify the source directory at all (v28 CKV-011: no lock file, no
  writer flock, no torn-tail repair, no orphan-.tmp sweep, no stale-delta
  cleanup — mutations are left to a normal open). Consequently an as-of
  open does NOT serialize against the source's writer; opening a view of a
  *live* directory races the writer's rotation and fails loud (never
  silently wrong) if a segment disappears mid-replay — restore from a
  backup copy or a quiesced directory. `restore_pitr` refuses a non-empty
  destination (v28 CKV-010).
- **PITR mid-window reconstructability (v26.1, review rank-1 fix)**:
  for `as_of` strictly between checkpoint boundaries, recovery applies the
  base plus every delta whose header cts `<= as_of` in full, then applies
  the first delta beyond the boundary **per entry** (`hc <= as_of` only),
  then replays surviving WAL records in `(last_applied_header, as_of]`.
  Consequences, precisely:
  * a delta entry with `hc <= as_of` is *provably exact* — it is the key's
    newest version at the delta's snapshot, so nothing in `(hc, as_of]`
    superseded it, regardless of which WAL segments survive;
  * if the WAL covering the window was rotated away by a later checkpoint
    and a surviving record still sits **inside** the window, the per-key
    coverage of the hole cannot be proven — `open` fails loud with a
    "rotated away" diagnosis (never silently wrong);
  * **fully-rotated window with filtered entries ⇒ loud rejection
    (`6d8a13d` review fix)**: if the covering WAL is *entirely* gone and
    the first delta beyond the boundary holds **any** entry with
    `commit_ts > as_of` (per-entry filtering had to skip it), the as-of
    state of that key is unprovable: the delta keeps only its post-`as_of`
    version, and "rewritten after `as_of` over a mid-window version" is
    byte-identical on disk to "first written after `as_of`". v26.1 served
    the best-effort snapshot here; the review showed that can be silently
    WRONG (its `k="at4"`/`k="at5"` repro returned `k` absent), so `open`
    now throws a "not reconstructable" diagnosis with the remedies
    (boundary `as_of`, a covering `backup()`, or retaining WAL segments).
    A filter pass that skips NOTHING over a rotated window still opens —
    every window write is then provably in the applied chain. Restoring
    the rejected capability soundly requires multi-version deltas (the
    review's direction 3 — tracked as future work in the ROADMAP).
- **Strict-serializable acknowledgements (v27 M1)**: `commit_txn` waits
  for the contiguous published prefix to cover its cts before returning
  `Committed` (a publication barrier). Therefore: if a write is
  acknowledged before a transaction begins, that transaction's snapshot —
  and every read in it — includes the write (real-time order); writer
  order equals cts order because cts is reserved under the WAL batch
  mutex; and reads are exact state-as-of-snapshot by the MVCC construction
  the `lincheck` checker verifies on recorded histories. The barrier's cost
  is bounded: cts order equals WAL order, so by the time a commit's own
  records are durable, every lower cts is past its WAL I/O and only
  in-memory install can be outstanding. A throw between WAL durability and
  publication now **burns** the cts (prefix keeps advancing; the caller
  fails loud) instead of stalling the prefix forever. Append-only opens
  (existing WAL, `recover_on_open=false`) seed the published prefix
  alongside the commit clock, so the first commit's barrier never waits on
  holes the instance will never track (regression-tested with a hard
  timeout).
- **Backup semantics (v26 M3)**: a backup is *at least* as new as the
  checkpoint taken inside `backup()` and *at most* as new as copy
  completion — not a point-in-time snapshot. `verify_backup()` succeeds
  only if every copied file matches its recorded size and CRC32, the
  checkpoint chain is structurally valid, and the copied WAL fully
  parses (invariant B1); an interrupted copy has no complete
  `BACKUP_COMPLETE` marker and is therefore rejected.

## Build and test

The whole project is two files: `chronokv.hpp` (engine + API) and
`main.cpp` (test suite). `make` drives a four-config matrix, in both
test-hooks-on (full internal suite) and hooks-off (public-API smoke)
flavors:

```sh
make release    # -O2, full test suite
make asan       # ASan + UBSan, full test suite
make tsan       # TSan, full suite in one run
make stress     # deterministic stress mode (seeded RNG)

make smoke_off_release   # public-API-only smoke (no test hooks)
make smoke_off_asan
make smoke_off_tsan
make smoke_off_stress

make clean
```

Useful variables:

```sh
make asan CXX=g++-14                 # pick a compiler
make release CKV_EXTRA_DEFS="-DCKV_IOURING_DISABLED"   # force pwrite fallback
```

**TSan** — v25.2 removed the old `CHRONOKV_TSAN_BATCH` split: `make tsan`
runs the full suite as a single step (`build/tsan/test`), and CI runs it
as one job. The batch mechanism was a workaround for 2-CPU dev VMs;
CI runners complete the whole suite comfortably within one job.

**io_uring backend** — the wrapper self-configures at runtime (no build
flags needed) and prints its feature summary at the top of the test run:

```
io_uring: sq=64 cq=128 coop_taskrun=1 fixed_buf=256K fixed_files=lazy write_fdatasync_chain=1 deadline=500ms
```

Every feature degrades independently: kernels without `COOP_TASKRUN`
(< 5.19) get a plain ring; a small `RLIMIT_MEMLOCK` shrinks the
registered bounce buffer (256 KiB -> 64 KiB -> none); kernels that
reject `LINK_TIMEOUT` (some vendor-hardened 5.10s) run deadline-less
chains; and if the ring cannot be created at all (or
`CKV_IOURING_DISABLED` is defined) every batch uses the sync
`pwrite`+`fsync` fallback. The test suite includes a real-kernel
end-to-end assertion (`iouring-real`) that runs wherever io_uring is
available and skips otherwise.

## CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push,
pull request, manual dispatch, and a nightly schedule (03:17 UTC — the
long-run config; a push can no longer cancel a nightly run mid-flight):

- **hooks-on matrix**: `release` (g++-13, g++-14, clang++-18), `asan+ubsan`,
  `stress`, `release (g++-12)` on ubuntu-22.04 (older 5.15 kernel —
  exercises the io_uring fallback ladder), and `release` with
  `CKV_IOURING_DISABLED` (sync-fallback compile-out path)
- **TSan**: the full suite in a single job
- **hooks-off matrix**: public-API smoke under `release`, `asan+ubsan`,
  `tsan`, and `stress` — proves the header is usable without any test
  hooks defined
- **`crashfuzz` (v27 M2)**: the crash-fuzz regime alone
  (`CKV_ONLY_CRASHFUZZ`), seed-scaled via `CKV_CRASHFUZZ_SEEDS` — 2 bases ×
  24 rounds on push/PR, 8 × 120 nightly. The base seed derives from the
  run id and every base is echoed to the log, so a failing plan replays
  deterministically from the log alone; preserved crash scenes are
  uploaded as artifacts on failure
- **`dst` (v27 M0+M2)**: the real deterministic-scheduler harness — four
  fork-isolated scenarios over the bug-dense paths at `CKV_DST_SEEDS=N`
  per scenario (100 on PR; 25,000 nightly = the roadmap's 100k), failures
  reported with the `(seed, op-count, last point)` replay handle — plus
  the M2 seeded regimes: the full suite under `CHRONOKV_STRESS` across
  interleaving seeds (`CKV_STRESS_SEED`, echoed; 1 seed on PR, 3 nightly)
  and the lincheck engine workloads at `CKV_LINCHECK_SEEDS=N` (4 on PR,
  64 nightly)
- **`coverage` (v27 M3)**: gcov-instrumented build; every fault kind is
  FORCED for a whole run (`CKV_COVERAGE_FAULT`, independent of the
  suite's own arm/disarm windows) — over the review-regression gate on
  push/PR, over the full suite nightly plus an unforced headline run.
  `scripts/fault_coverage.py` publishes per-kind error-path coverage,
  each kind's UNIQUE contributions, forced-fire counts (0 = vacuous kind
  for that vehicle, flagged), and the ledger of error-path lines no run
  reached, as the job summary + an artifact
- **`ci-passed` gate**: aggregates every job into one required check for
  branch protection

## Architecture

`chronokv.hpp` is organized into eight sections:

1. **CRC / I/O primitives** — CRC-32, checked I/O helpers, fault injection
2. **WAL framing** — record encoding/decoding, strict parsers
3. **io_uring wrapper** — runtime feature ladder (COOP_TASKRUN ring, registered
   buffers/files, linked write→fdatasync chain with kernel deadline);
   every step falls back independently, down to sync `pwrite`
4. **WalSegments** — segmented WAL, group commit, torn-tail recovery
5. **PublicationTracker / PhantomTracker** — commit visibility watermark;
   SSI phantom detection over scan ranges
6. **MVCC types** — `Version` chains per key, epoch-pinned retirement
7. **ChronoKV core** — transaction engine, GC, checkpointing
8. (public API layer) **`chronokv::Database` / `Transaction` / Batch /
   observers / streams / diagnostics** — plus the B+ tree index and page
   pool namespaces

Each engine subsystem carries its invariants in extensive source comments
(the header doubles as the design document — see the v23 epoch-reclamation
E1–E16 safety argument and the v24 fix log at the top of the file).

## Known limitations

- **No B+ tree page reclamation**: `PagePool::free()` has no callers and the
  tree has no merge/rebalance — splits only ever add pages. The page pool is
  therefore a **monotonic ceiling**: once it is exhausted
  (`Options::page_pool_bytes`, 256 MiB by default) `alloc()` throws
  `std::bad_alloc`, and delete-heavy workloads never get space back. Size the
  pool for the index's *lifetime* high-water mark, not its current size.
  Implementing leaf merge/rebalance (and wiring the existing hazard-pointer
  slots into a real reclamation check) is the tracked follow-up.
- **`~Transaction()` aborts the process** if the transaction is still active
  and the `Database` is alive. This is deliberate (see Safety properties), but
  it means an exception propagating out of a scope that holds a `Transaction`
  will `SIGABRT` during unwinding. Always commit or abort explicitly, and
  prefer an explicit `try`/`catch` around transactional scopes.
- **Observer reentrancy**: callbacks run inline under the observer lock;
  re-entering the observer machinery from a callback deadlocks
  (tracked for a future dedicated notification thread).
- **Stream consistency**: `RangeScanStream` guarantees per-page snapshots,
  not cross-page consistency during concurrent writes.
- **Single process**: the `flock` guard fails fast on accidental second
  openers; there is no multi-process mode.
- **Linux only**: no macOS/Windows support.
- **Crash testing is not power-loss testing**: the crash fuzzer kills the
  process with `_exit()`, which leaves the kernel page cache intact. It
  therefore validates the *recovery state machine* — torn tails, half-written
  MANIFESTs, orphaned `.tmp` files, rotation and checkpoint boundaries — but
  cannot distinguish "fsynced" from "still dirty". True power-loss validation
  needs a fault-injecting block layer (`dm-flakey`) or real hardware, and is an
  open coverage gap.
- **Compile memory**: the test suite was split into four translation units
  (v29 M2 item 5, 2026-10-10 — `main.cpp` plus
  `tests/tests_{lincheck,remediation,dst}.cpp`, all including the single
  header), lowering the largest cc1plus unit from the 28k-line monolith to
  ~24k lines. Verified on a 1 GiB container after the split:
  `make release RELEASE_FLAGS="-O1"` builds bare (the monolith was
  OOM-killed there), `make stress` at `-O1` builds, and ASan+UBSan builds
  at `-O0` with `--param ggc-min-expand=5` (the full battery suite passes
  under it). `-O2` on ~1 GiB remains untested/tight; CI runners are fine.
  The engine-side split with the amalgamated single-header release
  artifact remains v30 M2 — the zero-dependency embed story is unchanged:
  consumers still compile exactly one header.
- **GC sweep cost**: each GC pass re-scans the whole key tree to collect
  entries before processing its 256-key budget, so a full sweep is
  O(N²/256) scan work at large N (the v25.7 idle-spin fix stopped the
  pathological back-to-back sweeping, but the per-pass rescan remains).
  A persistent tree cursor for GC is a tracked follow-up.

## Roadmap

[`docs/ROADMAP.md`](docs/ROADMAP.md) — the 2026-10-07 plan (v29 → v30,
*"challenge the real databases"*, rebaselined against **0.28.1** after the
Audit-2 round). **v29 — the overhaul:** the independent re-audit gate
(executed — 13 findings remediated fail-first as 0.28.1; closure pending:
the report in-tree, a delta confirmation round, the `v0.28.1` tag), a
benchmark arena with vendored version-pinned baselines (SQLite / LMDB /
RocksDB) and a regression-gated results ledger (the repaired nightly
vehicle awaits its first completed run), catcher hardening (lincheck's
write-skew checker — the blindness TXN-1 demonstrated — plus the
test-suite TU split API-1 unlocked), page reclamation with hazard-pointer
consumers, leaf merge/rebalance (new invariant P1), the WAL writer-thread
rewrite (the leader handshake gets deleted, not rewritten), and
space/steady-state work (linear GC sweeps, streaming recovery parse,
multi-version PITR deltas). **v30 — 1.0.0:** machine-checked invariants,
the 100M-key scale soak with RSS-level memory budgets and a bulk-load
recovery prerequisite, power-loss truth via a fault-injecting block layer,
the source split with an amalgamated release artifact, an honest
head-to-head comparison page generated from the ledger, and a release
gate an audit verdict can hold. Each item carries its anchor, size,
acceptance criteria and any new canonical invariant; sizes are T-shirt
guesses and the pace assumption is stated (the Audit-2 round ran in nine
days, commission to merge). The plan supersedes the 2026-09-27 roadmap
with a full
[disposition table](docs/ROADMAP.md#disposition-of-the-2026-09-27-plan) —
every item mapped, nothing silently dropped — and preserves the
superseded plan (with its v26/v27 history) as its appendix. Process rules
1–14 are permanent; 11–14 are new, each earned by a named incident.

Canonical invariants are defined in `chronokv.hpp`'s header comment and
referenced by ID throughout the roadmap (R1, I2, I3, I6, D1, D2, D3, R-REBASE,
T1, B1, P1).

## License

[MIT](LICENSE)
