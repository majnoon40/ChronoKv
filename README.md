# ChronoKV

[![CI](https://github.com/majnoon40/ChronoKv/actions/workflows/ci.yml/badge.svg)](https://github.com/majnoon40/ChronoKv/actions/workflows/ci.yml)

**ChronoKV** is an embedded, multi-version (MVCC) key-value store with
serializable transactions, written as a single C++20 header
(`chronokv.hpp`). It pairs a write-ahead log (WAL) with optional
checkpointing for durability, serializable-snapshot-isolation (SSI)
transactions with phantom detection, a paged B+ tree index, and
io_uring-accelerated WAL writes — all in one ~8.6k-line header with no
external dependencies.

Current version: **0.25.2** (`CHRONOKV_VERSION` in `chronokv.hpp`).

## Highlights

- **Single-header engine** — drop `chronokv.hpp` into any C++20 project;
  the repository's `main.cpp` is the full engine test suite.
- **Serializable transactions (SSI)** — snapshot reads with write-set
  validation, including phantom detection over registered scan ranges
  (no write skews, no phantoms).
- **Crash-safe WAL** — CRC-checked, segmented (64 MiB) WAL with torn-tail
  truncation, batch group-commit, and strict LSN gap/duplicate detection
  on recovery.
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
  page pool, per-page shared latches, hazard pointers, and incremental
  range-scan cursors.
- **Lock-free reclamation** — epoch-pinned deferred GC of old versions;
  no GC-vs-scan lock contention on the read path.
- **Rich public API** — sync + async operations, atomic batches,
  prefix observers, streaming range scans, and read-only health/stats
  diagnostics.
- **Heavy-duty validation** — engine tests, B+ tree fuzzing, fault
  injection, deterministic stress mode, and a linearizability history
  recorder, all run under a four-config sanitizer matrix
  (Release / ASan+UBSan / TSan / Stress).

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
| Observers | `observe(prefix, callback)` | inline callbacks on the committing thread |
| Diagnostics | `wal_stats/gc_stats/epoch_stats/health/published_watermark` | read-only snapshots of engine counters |

**Status codes** (`chronokv::Status`): `OK`, `Conflict`, `TooLarge`
(values are capped just under 1 MiB by the WAL framing), `InvalidTransaction`,
`InvalidState`, `WalFailure`, `Failed`.

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
- Recovery verifies CRCs, truncates torn WAL tails, rejects LSN
  gaps/duplicates, and validates the checkpoint chain.
- A `Transaction` destroyed while still active calls `std::abort()` —
  commit or abort explicitly. Concurrent `Database::close()` with live
  transactions requires external synchronization.

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
pull request, and manual dispatch:

- **hooks-on matrix**: `release` (g++-13, g++-14, clang++-18), `asan+ubsan`,
  `stress`, `release (g++-12)` on ubuntu-22.04 (older 5.15 kernel —
  exercises the io_uring fallback ladder), and `release` with
  `CKV_IOURING_DISABLED` (sync-fallback compile-out path)
- **TSan**: the full suite in a single job
- **hooks-off matrix**: public-API smoke under `release`, `asan+ubsan`,
  `tsan`, and `stress` — proves the header is usable without any test
  hooks defined
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

- **Observer reentrancy**: callbacks run inline under the observer lock;
  re-entering the observer machinery from a callback deadlocks
  (tracked for a future dedicated notification thread).
- **Stream consistency**: `RangeScanStream` guarantees per-page snapshots,
  not cross-page consistency during concurrent writes.
- **Single process**: the `flock` guard fails fast on accidental second
  openers; there is no multi-process mode.
- **Linux only**: no macOS/Windows support.

## License

[MIT](LICENSE)
