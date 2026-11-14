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
python3 scripts/arena_bands.py check --bands bench/arena_bands.json --night <dir>
python3 scripts/arena_bands.py verify-ledger --night <dir>       # completeness gate
python3 scripts/arena_bands.py loo --nights <dir-per-night> [...] # honest cross-validation
python3 scripts/test_arena_gate.py                              # the gate's own controls
```

**Txn/GC series (finding #6, 2026-11-14) — OPT-IN, not a typo.**
`ycsb_a.txn`, `rmw.txn` (read+update inside ONE begin()/commit() SSI
transaction, conflicts counted — the row fails VACUOUS if conflicts==0
at threads>1), and `overwrite_gc`, `deletechurn_gc` (the existing
workloads with `auto_start_gc=true` and gc_stats deltas in the note —
VACUOUS if retired/reclaimed froze). They are NOT in the default
`--workloads` string; the workload-set gate (defect 5) would refuse them
as a ledger-format change, on purpose. The promotion sequence is
docs/audits/2026-11-14-txn-gc-series-sequencing.md: >= 7 calibration
nights (the dispatch-only `txn-gc-calibration` CI job exists for this),
then ONE dedicated recalibration commit that adds them to the default
set, recomputes the bands, and re-promotes the reference week — from
which point defect-5/6 gate the new set automatically. Baseline parity:
`Engine::txn()` in bench/baselines.cpp (SQLite BEGIN IMMEDIATE shape
with SQLITE_BUSY-as-conflict; LMDB one write txn, no conflict signal
exists — printed in the note; RocksDB atomic batch, no isolation —
printed in the note). The isolation deviation is IN the row's note,
never implied.

**The gate** (ci.yml `arena` job, nightly/dispatch only): a ledger
completeness step (all seven engines must produce data rows — a crashed
leg writes stderr into its TSV, so "file exists" is not "rows exist"),
then `scripts/arena_bands.py check` against the committed bands
(exit 1 = REGRESSION **or MISSING series**, exit 2 = geometry mismatch —
mixed geometries refuse to band). `arena` sits in `ci-passed`'s needs,
skip-tolerated on push/PR where it does not run: a flaky shared runner
never blocks a PR, and the gate has teeth on the nightly.

**Gate hardening (2026-10-17, same day as enablement — four defects;
defect 5 added same-day by the M1 owner, defect 6 added 2026-11-14 —
each fixed behind fail-first controls; the full before/after evidence is
in the CHANGELOG entry and the commit message):**
* **Defect 1 — the completeness step passed green on dead legs.** The
  first cut counted lines with shell `grep -cvE '^#|^$' ... || echo 0`:
  `grep -c` prints `0` AND exits 1 on zero matches, so the `|| echo 0`
  fallback produced `"0\n0"`, `[ "$n" -lt 1 ]` died with "integer
  expression expected" (rc=2), the if-branch never ran — an EMPTY or
  comment-only TSV sailed through; and any non-comment line counted, so
  a TSV holding only crash stderr ("terminate called…") counted as data.
  Reproduced with fixtures before the fix (three dead legs, step exit
  0). Fixed by moving the logic into `arena_bands.py verify-ledger`
  (per engine: file exists, ≥1 row with ≥7 tab fields and numeric
  `ops_per_sec > 0`), unit-tested with those exact fixtures.
* **Defect 2 — `check` ignored missing series.** It iterated the
  night's rows, so a leg dying mid-run just produced fewer verdicts:
  measured — one dropped workload row "29 series checked" exit 0, a
  deleted `chronokv-async.tsv` "15 series" exit 0, a 3-row-truncated
  `chronokv-group.tsv` "18 series" exit 0. Fixed: every band series
  (ratio tier, plus the chronokv RSS tier) absent from the night is a
  MISSING verdict and exit 1. Pristine nights re-checked: 0 MISSING,
  identical verdicts — no false positives.
* **Defect 3 — the "0 REGRESSION on all 7 calibration nights"
  self-consistency claim was vacuous BY CONSTRUCTION.** Each night is a
  sample of its own bands, so its ratio ≥ band min > floor(min × 0.85):
  in-sample REGRESSION cannot fire (worked example recorded in the
  changelog: group/ycsb_c.read band min 0.501 IS N2's own value, floor
  0.426). Replaced with `arena_bands.py loo` — leave-one-out
  cross-validation (bands from the other six nights, `--min-nights 6`,
  held-out night checked). **LOO result on the green week (recorded, not
  acted on):** 6 held-out REGRESSIONs across 2 of 7 nights, all in the
  read- and scan-heavy series — N2 (Xeon 8573C): group/ycsb_c.read
  0.501<0.746, async/ycsb_c.read 0.730<0.806; N6 (EPYC 9V74):
  group/ycsb_e.insert|scan 0.522|0.523<0.826, async/ycsb_e.insert|scan
  0.861<0.869 (the async pair misses the floor by 0.9%). The other five
  nights: 0. Meaning: for those two series the cross-CPU-model spread
  exceeds the tolerance band — the committed bands stay UNCHANGED (no
  widening, per policy) because they include all seven nights and thus
  already encode the observed hardware spread; the LOO numbers are the
  honest generalization bound and a standing band-policy question
  (hardware-conditional gating vs. fleet convergence vs. engine-side
  investigation of the ycsb_c/e variance) for the M1 owner. `loo` is
  informational by design (always exit 0) — an automatic reaction to
  its numbers would be exactly the in-flight widening the policy
  forbids.
* **Defect 4 — no synthetic-regression control existed.** Added:
  chronokv `ops_per_sec` × 0.5 over a promoted night must exit 1 with
  REGRESSION verdicts (measured: 27), × 0.95 must stay exit 0 (WARN
  allowed). 
* **Defect 5 (found by the M1 owner reviewing the hardening itself,
  fixed the same day): a half-dead BASELINE leg wasn't caught.**
  `verify-ledger`'s ≥1-valid-row rule and `check`'s ChronoKV-only
  MISSING verdicts left baseline workloads unguarded, in BOTH
  directions — measured on copies of promoted night N7: dropping ONE
  `sqlite-full` fillseq row passed the whole gate (verify 0, check 0)
  while the geomean denominator fell from five baselines to four and
  `chronokv-group/fillseq` silently shifted **-15.9%** (0.2069 →
  0.1740; dropping a FAST baseline shifts the other way and can MASK a
  real regression); truncating `rocksdb-sync` to one row passed
  verify-ledger and turned the nightly red with **two FALSE
  `fillrandom` REGRESSIONs** — a vehicle failure misattributed to the
  engine. Fixed in two layers: `verify-ledger` now compares each
  engine's valid-workload SET to the calibrated one (reference: the
  most recent committed `bench/results/promoted/` week — a workload
  list change is a ledger-format change and owes a recalibration
  commit, exactly like a geometry change; extras fail too), and
  `check`'s geomean is STRICT: a ratio is REFUSED whenever any of the
  five baseline legs lacks a valid row for that workload — fail-tier
  `BASELINE-GAP` verdict naming the offending engines ("a vehicle
  failure, not an engine regression"), never a computed ratio.
  Ratio-less-by-design workloads (the chronokv-exclusive family) are
  unaffected; the committed bands are untouched (recomputation over
  the seven nights is identical to 1.1e-14 relative — geomean
  summation-order float noise, zero verdict drift on all seven).
* **Defect 6 (2026-11-14): baseline VERSION identity was ungated.** The
  nightly arena installs **distro** baseline packages (`apt-get
  libsqlite3-dev liblmdb-dev librocksdb-dev`) while
  `bench/third_party/versions.txt` pins source recipes, and nothing
  verified that the recorded baseline *versions* match the calibration:
  a library version change moves the same-night geomean **denominator**
  every ratio is normalized by — faking a REGRESSION when it falls,
  masking one when it rises (the same failure class as defect 5's
  half-dead baseline legs, but invisible to row/workload-set counting;
  the nightlies of 2026-11-10..13 failing only the `check` step on an
  unchanged commit are consistent with exactly this drift). Fixed in
  `verify-ledger` (the completeness gate CI runs before `check`):
  each of the five baseline TSVs must record the same library version
  as the calibrated reference — same promoted-week reference, same
  agreement rule (a week must agree across its nights; inconsistency is
  a hard tool error, never a silent pick). Version extraction is driven
  by the real header format (`# engine=…; version=…`, everything after
  the last `version=`, so LMDB's full `mdb_version` string parses
  whole). A baseline version change is a ledger-format change that
  owes a recalibration commit (which re-promotes), never a silent
  mix; ChronoKV's own version is NOT gated — it legitimately changes
  between recalibrations.
* **All of the above are wired into `scripts/test_arena_gate.py`** (24
  controls + 2 meta-controls — 26 checks; the meta pair proves the suite
  never mutates the bands or
  the promoted TSVs it judges), running as the `arena-gate-selftest`
  job on EVERY push/PR (committed fixtures, no runner hardware needed)
  and in `ci-passed`'s needs with no skip tolerance. Red there means
  the arena gate's verdicts are untrustworthy until green again.

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

## The WAL committer-count sweep (v29 M5 slice 3, 2026-11-08)

`bench/writer_sweep.cpp` is the roadmap's own acceptance instrument for
M5's concurrency leg — "a measurable win at 8+ committers" needs the
8/16/32 sweep, and the nightly ledger cannot carry it: the ledger's
geometry is PINNED at 4 threads (a concurrency axis would be a
ledger-format change). So the sweep is an ARENA-ADJACENT ONE-OFF, per
the M5 decision doc §8's own wording, with the same rule-9 methodology
header as the arena workloads and a per-cell `wal_stats` diagnostics
line (drains, records-per-drain, hot-idle spins/hits, lingers, queue
high-water) so every latency number is attributable.

The workload isolates the WAL commit path: an untimed pre-fill, then N
committers × K timed puts over the shared zipfian key space — no reads.
The A/B it exists for is BOTH WAL legs on one machine:

```sh
make sweep sweep-legacy            # writer-thread leg + CKV_WAL_LEGACY_LEADER leg
./build/sweep/sweep       --committers 8 --ops 8000 --keys 50000 --label new_c8 ...
./build/sweep/sweep-legacy --committers 8 --ops 8000 --keys 50000 --label leg_c8 ...
```

`--probe io` (no committers) times raw pwrite vs fdatasync on the
wal_dir's filesystem and prints the depth-2 pipelining upper bound
(`depth2_upper_bound_pct` = the write share of the per-drain critical
path — the most depth-2 could ever remove). Knob overrides
(`--hot-idle-us`, `--linger-us`) exist for sensitivity cells; the
Options defaults are never changed by the harness.

`make arena-legacy` builds the arena's own legacy-leg twin (separate
output path — the two legs can never alias each other's objects) for
after-table A/B runs at the pinned geometry.

The `writer-sweep` CI job (workflow_dispatch only, NOT in ci-passed
needs — a measurement vehicle, not a gate) runs the full matrix on one
runner: both legs × {group, async} × {1,2,4,8,16,32} committers, a
hot-idle-OFF cell, SQPOLL cells, the io probe, and the arena A/B over
the regressed write mixes; the artifact is the record the M5 decision
doc §15.1 cites. Rule 13 applies: the 60-min cap ships generous, to be
tightened after the first completed run publishes per-cell durations.
