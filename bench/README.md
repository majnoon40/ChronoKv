# bench/ — the ChronoKV benchmark arena (v29 M1)

Per `docs/ROADMAP.md` v29 M1: the arena is built **before** the rewrites it
measures (load-bearing constraint 2 — *measure before predict*). Everything
here is a hooks-OFF consumer build: the public API only, exactly what an
embedder compiles.

## Status: M1 COMPLETE (2026-10-17) — workloads + adapters + pins + the green week (7/7) + calibrated noise bands + the regression gate

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

Workloads: `fillseq`, `fillrandom`, `readrandom`, `overwrite`,
`ycsb_a/b/c/d/e/f` — the full arena set. **Step 3 is COMPLETE (D/E/F
shipped 2026-10-10)**: an `Engine::scan` primitive that MATERIALIZES
rows (the arena's range_scan returns pairs; a count-only scan would do
strictly less work — fairness), D's geometric(0.001) read-latest tail
over the shared insert counter, E's zipfian-start / len-1..100 scans
(the SQLite adapter resets its scan statement before returning — the
WAL read-snapshot trap, now live in E's scan+insert alternation on one
connection), and F's combined read+write rmw latency. Version pins live
in `bench/third_party/versions.txt` (sqlite 3.45.1, lmdb 0.9.31,
rocksdb 8.9.1 — cross-checked against the ledger's methodology headers,
sha256-verified by `bench/third_party/fetch.sh`); CI keeps installing
distro packages during calibration (per-run versions ride the headers).
The nightly ledger vehicle is ONE job again — same-hardware fairness:
the parallel split ran the two legs on different CPUs (#75's headers
caught it), so the green-week clock restarts on the merged vehicle.
Remaining step-4 work: the green week, noise bands, gate enablement.

Validation quirks the adapters hit and fixed (each a fairness issue, not a
flake): LMDB requires `mdb_dbi_open` priming or every dbi-0 op fails EINVAL;
RocksDB `create_if_missing` does not create missing PARENT directories (and
the factory now propagates constructor failures instead of returning a
hollow engine); a SQLite SELECT left sitting on its result row keeps the
connection's WAL read snapshot open, so the next `BEGIN IMMEDIATE` on the
SAME connection returns SQLITE_BUSY immediately WITHOUT invoking the busy
handler — reset-after-read is mandatory in mixed read/update workloads.

## Noise bands and the regression gate (v29 M1 items 4–5, landed 2026-10-17)

The green week completed **7/7** on the re-merged single-job vehicle
(nightly runs #82/#88/#91/#96/#97/#98/#99, 2026-10-11..17, all green;
every TSV row carries its own rule-9 methodology header). The seven
nights' rows are promoted in-tree at
`bench/results/promoted/green-week-2026-10-11_17/` — the permanent,
citable evidence for the acceptance line "the nightly ledger shows one
full green week".

**The design fact calibration measured:** the nightly runner CPU differs
night to night even on the single-job vehicle — four CPUs across the
seven nights (EPYC 9V45 ×3, Xeon Platinum 8573C, EPYC 7763 ×2, EPYC
9V74). Same-night cross-engine numbers are fair by construction (one
job, one machine — the fairness protocol's whole product); cross-night
RAW numbers are a hardware lottery (ratio spreads up to ~63% on
readrandom). So:

* **PRIMARY bands are ratio-normalized** — ChronoKV ops/s ÷ the geometric
  mean of the same night's five baseline configs, per workload. The
  ratio's cross-night spread IS the noise band; a drop below
  `min × (1 − tolerance)` (tolerance 0.15) is an engine regression, not
  a runner-lottery outcome.
* **Raw bands are secondary**, grouped by (cpu, engine, workload) — with
  ≤3 nights per CPU they are honestly stamped PROVISIONAL and are
  reference material, not gate material.
* **RSS bands cross nights directly** (hardware-independent enough); the
  `deletechurn` RSS delta is P1's before-column and carries a
  ratchet-watch (WARN above band max × 1.25) that M3's acceptance
  ("stabilizes instead of ratcheting") tightens into a hard gate.

`bench/arena_bands.json` is the committed calibration — 7 nights ⇒
CALIBRATED (≥ `--min-nights 7`; anything less stamps PROVISIONAL:
reportable, not gateable). Regenerate with:

```sh
python3 scripts/arena_bands.py band --nights <dir-per-night> [...] --out bench/arena_bands.json
```

**The gate** (ci.yml `arena` job, nightly/dispatch only): a ledger
completeness step (all seven engines must produce data rows — a crashed
leg writes stderr into its TSV, so "file exists" is not "rows exist"),
then `scripts/arena_bands.py check` against the committed bands
(exit 1 = REGRESSION, exit 2 = geometry mismatch — mixed geometries
refuse to band). `arena` sits in `ci-passed`'s needs, skip-tolerated on
push/PR where it does not run: a flaky shared runner never blocks a PR,
and the gate has teeth on the nightly.

**Policies (explicit, per M1 item 4):**
* **Re-run:** a red gate re-runs ONCE via `workflow_dispatch` on the same
  commit (ratio bands normalize the CPU lottery, not transient
  contention); TWO consecutive REGRESSION verdicts are a real
  regression — bisect, do not re-run.
* **Band updates:** recomputed only in a dedicated commit citing the
  engine change that moved them (M5's writer thread is the planned one;
  recompute from ≥7 post-change nights), never widened in-flight to
  silence a red night — the ratchet rule applies to bands.
* **Ledger promotion (item 5's decision):** raw nightly rows live as run
  artifacts (90-day retention); whenever a milestone CITES ledger rows,
  that night's TSVs are committed under `bench/results/promoted/` in the
  citing commit — citations stay verifiable forever, no bot and no
  nightly commit churn.
* **Arena-side build caching: REJECTED as measured low-value** — the
  nightly's ~57–77 min is dominated by the measured runs themselves;
  apt install + baseline build is a few minutes of that, and caching
  built baseline binaries across nights risks binary↔recorded-version
  provenance drift (the fairness protocol's core asset). Revisit only if
  nightly wall-time approaches the 120-min cap. The test-suite-side
  caching question was already answered by `build-shared` (one build,
  artifact reuse across the consuming jobs; ccache rejection recorded in
  ci.yml).

## Usage

```sh
make arena                       # -O2 default; small boxes: make arena ARENA_FLAGS="-O1"
./build/arena/arena --keys 1000000 --valsize 100 --threads 4 \
    --durability group --dir /tmp/ckv_arena [--workloads fillseq,readrandom]
```

Numbers are **budgets, not trophies** (rule 9): a result without its
methodology header does not get quoted, and published tables ride the
regression ledger, not commit messages.
