# bench/third_party — vendored baselines (rule 10)

**Baselines are benchmarks, not dependencies.** Nothing in this directory (or
any third-party header it points at) may ever enter the engine's include
path: `chronokv.hpp` and `main.cpp` compile with zero external includes, and
the only file in the repository that includes a third-party storage header is
`bench/baselines.cpp`.

## Current status (M1 step 3, start)

The adapters develop against **distro packages** (Debian bookworm at the time
of writing: sqlite3 3.40.x, lmdb 0.9.30, rocksdb 7.8.3) — detected by header
presence in the Makefile, so a missing library simply omits that engine from
the binary ("not compiled in" at run time).

## What lands here before the ledger gates anything (M1 step 3 completion)

1. **Version pins**: exact upstream versions per baseline, recorded in
   `versions.txt` (name, version, source URL, sha256 of the release
   artifact), reviewed like any format change.
2. **Vendored sources or fetch recipes** under this directory — SQLite's
   amalgamation, LMDB's libraries, and a RocksDB build recipe (its full
   source is too large to vendor blindly; a pinned fetch + cached build in
   CI is the rule-10-compatible shape).
3. **Build caching** for the CI ledger job (a cold RocksDB build costs
   10–20 minutes; the nightly job must not rebuild it per run).

## Durability-class mapping (printed in every methodology header)

| baseline config | class | ChronoKV comparator |
| --- | --- | --- |
| `sqlite-full` (WAL, synchronous=FULL) | power-loss | Sync / Group |
| `sqlite-normal` (WAL, synchronous=NORMAL) | process-crash | Async |
| `lmdb` (defaults) | power-loss | Sync / Group |
| `rocksdb-sync` (WriteOptions.sync=true) | power-loss | Sync / Group |
| `rocksdb-tuned` (sync=false + WAL, 128 MiB memtable, no compression, 4 bg jobs) | process-crash | Async |

Comparisons across classes are invalid and the harness refuses to imply
them: every table row carries its class, and the ledger groups by class.
