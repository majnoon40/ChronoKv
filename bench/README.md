# bench/ — the ChronoKV benchmark arena (v29 M1)

Per `docs/ROADMAP.md` v29 M1: the arena is built **before** the rewrites it
measures (load-bearing constraint 2 — *measure before predict*). Everything
here is a hooks-OFF consumer build: the public API only, exactly what an
embedder compiles.

## Status: M1 steps 1–3(start) — engine-side + YCSB mixes + baseline adapters

`arena.cpp` ships the engine-side micro suite: `fillseq` / `fillrandom`
(the new-key `nm_`-path isolation workloads), `readrandom`, `overwrite`,
`rangescan` (100 / 10k), `deletechurn` (P1's before-column), `ckptload`
(checkpoint-under-load wall time), `coldrecovery` (the v30-M1 bulk-load
prerequisite anchor) and `memory` (RSS-level bytes/key — pool + version
heap + slack, per the roadmap's B2 discipline). Every run prints a rule-9
methodology header (kernel, CPUs, RAM, CPU model, build flags, version,
durability, geometry, seed) before the TSV results.

**Step 2 (shipped):** YCSB-style mixes `ycsb_a` … `ycsb_f` — the six
standard operation mixes (A 50/50 read/update, B 95/5, C read-only,
D read-latest/insert, E scan/insert with len 1–100, F read + RMW) over
zipfian(θ=0.99). Documented deviations (rule 9): the zipfian is a
precomputed-CDF variant (same shape, different index sequence than YCSB's
hashed generator — identical for every engine in the harness, which is what
the ledger compares); D's read-latest is a geometric tail over the inserted
prefix; the CDF is O(n) memory — the 100M-key soak needs the CDF-free
inverse-transform variant first (flagged in the source). `--ops` sets the
per-mix operation count (default: keys).

**Step 3 (start, shipped):** `bench/baselines.cpp` — adapter binaries for
the vendored baselines under the SAME methodology, geometry and
distribution code (rule 10: the only file in the repo that includes a
third-party storage header; nothing here is reachable from the engine):

| `--engine` | config | durability class | ChronoKV comparator |
| --- | --- | --- | --- |
| `sqlite-full` | WAL, `synchronous=FULL`, WITHOUT-ROWID blob PK, one txn/write | power-loss | Sync / Group |
| `sqlite-normal` | WAL, `synchronous=NORMAL` | process-crash | Async |
| `lmdb` | defaults (synchronous commits), 64 GiB sparse map | power-loss | Sync / Group |
| `rocksdb-sync` | defaults + `WriteOptions.sync=true` | power-loss | Sync / Group |
| `rocksdb-tuned` | `sync=false` + WAL, 128 MiB memtable, no compression, 4 bg jobs | process-crash | Async |

Workloads: `fillseq`, `fillrandom`, `readrandom`, `overwrite`, `ycsb_a/b/c`
(D/E/F baseline adapters are the step-3 follow-up). Adapters currently build
against **distro packages** (header-presence detection in the Makefile);
version pinning + vendoring/fetch recipes under `bench/third_party/`
(see its README) and the CI nightly ledger with the calibrated noise
protocol are step 3-completion / step 4.

Validation quirks the adapters hit and fixed (each a fairness issue, not a
flake): LMDB requires `mdb_dbi_open` priming or every dbi-0 op fails EINVAL;
RocksDB `create_if_missing` does not create missing PARENT directories (and
the factory now propagates constructor failures instead of returning a
hollow engine); a SQLite SELECT left sitting on its result row keeps the
connection's WAL read snapshot open, so the next `BEGIN IMMEDIATE` on the
SAME connection returns SQLITE_BUSY immediately WITHOUT invoking the busy
handler — reset-after-read is mandatory in mixed read/update workloads.

## Usage

```sh
make arena                       # -O2 default; small boxes: make arena ARENA_FLAGS="-O1"
./build/arena/arena --keys 1000000 --valsize 100 --threads 4 \
    --durability group --dir /tmp/ckv_arena [--workloads fillseq,readrandom]
```

Numbers are **budgets, not trophies** (rule 9): a result without its
methodology header does not get quoted, and published tables ride the
regression ledger, not commit messages.
