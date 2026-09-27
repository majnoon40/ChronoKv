# bench/ — the ChronoKV benchmark arena (v29 M1)

Per `docs/ROADMAP.md` v29 M1: the arena is built **before** the rewrites it
measures (load-bearing constraint 2 — *measure before predict*). Everything
here is a hooks-OFF consumer build: the public API only, exactly what an
embedder compiles.

## Status: SKELETON (M1 step 1, engine-side)

`arena.cpp` ships the engine-side micro suite: `fillseq` / `fillrandom`
(the new-key `nm_`-path isolation workloads), `readrandom`, `overwrite`,
`rangescan` (100 / 10k), `deletechurn` (P1's before-column), `ckptload`
(checkpoint-under-load wall time), `coldrecovery` (the v30-M1 bulk-load
prerequisite anchor) and `memory` (RSS-level bytes/key — pool + version
heap + slack, per the roadmap's B2 discipline). Every run prints a rule-9
methodology header (kernel, CPUs, RAM, CPU model, build flags, version,
durability, geometry, seed) before the TSV results.

Not yet here (subsequent M1 steps): YCSB-style mixes A–F (step 2), the
vendored version-pinned baselines under `bench/third_party/` — SQLite /
LMDB / RocksDB, never in the engine's include path (step 3, rule 10), and
the CI nightly ledger with the calibrated noise protocol (step 4).

## Usage

```sh
make arena                       # -O2 default; small boxes: make arena ARENA_FLAGS="-O1"
./build/arena/arena --keys 1000000 --valsize 100 --threads 4 \
    --durability group --dir /tmp/ckv_arena [--workloads fillseq,readrandom]
```

Numbers are **budgets, not trophies** (rule 9): a result without its
methodology header does not get quoted, and published tables ride the
regression ledger, not commit messages.
