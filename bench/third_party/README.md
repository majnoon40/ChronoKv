# bench/third_party — vendored baselines (rule 10)

**Baselines are benchmarks, not dependencies.** Nothing in this directory (or
any third-party header it points at) may ever enter the engine's include
path: `chronokv.hpp` and `main.cpp` compile with zero external includes, and
the only file in the repository that includes a third-party storage header is
`bench/baselines.cpp`.

## Current status (M1 step 3, COMPLETE — 2026-10-10)

**Pins are recorded**: `versions.txt` carries the exact versions the CI
ledger builds against (ubuntu-24.04 distro: sqlite 3.45.1, lmdb 0.9.31,
rocksdb 8.9.1 — cross-checked against run #75's methodology headers) with
each release artifact's sha256, and `fetch.sh` downloads + verifies them
(mismatch is fatal; the GitHub-archive re-record rule is in versions.txt).
CI keeps installing **distro packages** during the calibration stage —
header-presence detection in the Makefile means a missing library simply
omits that engine ("not compiled in" at run time) — and every run's header
records the versions actually used, cross-checkable against the pins.
Local development used Debian bookworm packages (sqlite 3.40.x-era,
lmdb 0.9.30); validation runs are labeled with their headers like every
other ledger row.

## What lands here before the ledger gates anything (M1 step 3 completion)

1. **Version pins** — DONE: `versions.txt` (name, version, source URL,
   sha256 of the release artifact), reviewed like any format change.
2. **Vendored sources or fetch recipes** — DONE in the recipe shape:
   `fetch.sh` (pinned, sha256-verified, curl/wget/python3 fallbacks)
   downloads into `src/` (gitignored). Full in-tree vendoring stays
   deferred: RocksDB's source is too large to vendor blindly, and a
   pinned fetch + cached build is this README's own rule-10-compatible
   shape.
3. **Build caching** for the CI ledger job (a cold RocksDB build costs
   10–20 minutes; the nightly job must not rebuild it per run) — lands
   with gate enablement, keyed on versions.txt's rocksdb sha256. Only
   becomes real once the ledger stops installing distro packages.

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
