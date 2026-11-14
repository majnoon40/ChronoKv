# Txn/GC ledger series — sequencing decision (finding #6, 2026-11-14)

Landed 2026-11-14: the series themselves (bench/arena.cpp `w_ycsb_txn`,
`w_gc_on`; bench/baselines.cpp `Engine::txn` + `w_ycsb_txn`) and the
dispatch-only calibration vehicle (ci.yml job `txn-gc-calibration`). This
document is the decision record for how they become ledger material.

## What landed and what it deliberately did NOT touch

The four series are OPT-IN (`--workloads ycsb_a.txn,rmw.txn,
overwrite_gc,deletechurn_gc`): the default workload string, the committed
bands (bench/arena_bands.json), and the promoted reference week are all
byte-identical to pre-#6. That is not timidity — the workload-set gate
(arena-gate defect 5) treats any workload-set change as a ledger-format
change, and defect 6 (baseline version identity) applies from the same
calibration boundary. Landing the series opt-in first is what lets the
calibration nights accumulate without a single existing series moving.

## Isolation deviations (printed in every baseline row's note, not implied)

- SQLite: `BEGIN IMMEDIATE` .. SELECT .. INSERT OR REPLACE .. `COMMIT` —
  writer-serialized; contention surfaces as SQLITE_BUSY on BEGIN/COMMIT
  and is counted as the row's `conflicts=` (the analogous signal to
  ChronoKV's `Status::Conflict`). SQLite is not SSI; the row measures
  SQLite's own transactional cost at its own isolation.
- LMDB: single internal writer (MDB_txn serialization) — there is no
  concurrent-conflict signal in this shape; the row's note says so
  instead of printing a fabricated comparison.
- RocksDB: atomic WriteBatch (Get+Put, one durability unit) — no
  isolation, no conflict signal; the note says so. TransactionDB is a
  separate component and deliberately NOT silently swapped in.
- The GC-on series (overwrite_gc / deletechurn_gc) are ChronoKV-only by
  construction — no baseline has ChronoKV's GC model; they are
  ratio-less ledger rows like the chronokv-exclusive family.

## The promotion sequence (the recalibration commit)

1. Dispatch `txn-gc-calibration` (both durability classes + the three
   baseline legs). The anti-vacuity markers gate the run's OWN exit
   code: `VACUOUS:no-conflicts` (SSI path uncontended — threads>1 with
   zipfian(0.99) hot keys makes this structurally improbable) or
   `VACUOUS:gc-did-not-move` (gc_stats frozen — GC never ran) each fail
   the run loudly. Sandbox evidence at 5k keys/4 threads: ChronoKV
   conflicts 79 (ycsb_a.txn) / 176 (rmw.txn), gc_retired_delta 591 /
   1500, gc_reclaimed_delta 538 / 3000 — the series exercise exactly
   what they exist to measure.
2. Collect >= 7 nightly-shape runs post-change (the band rule) via the
   calibration dispatch or the nightly arena extended to the new series
   in the RECALIBRATION commit itself — never before.
3. One dedicated recalibration commit: add the four series to the default
   `--workloads` string, recompute the bands from those nights (only
   for any series admitted to the ratio tier; the GC-on series stay
   ratio-less), re-promote the reference week, and record the
   wall-time budget change (ycsb_a.txn at 200k ops adds ~4-6 s per
   class on the runner class — the calibration artifact carries the
   census).
4. From that commit, defect 5 (workload-set completeness) and defect 6
   (baseline version identity) enforce the new set automatically — no
   extra wiring needed; that is the point of landing the gate defects
   first.

## Why ycsb_a.txn and rmw.txn (the shape decision)

The arena already carries `ycsb_f.rmw` (blind read+put pair, no
transaction). `ycsb_a.txn` is YCSB A's own mix with the read+update pair
inside ONE begin()/commit() — the flagship SSI path at its real
contention level; `rmw.txn` is the pure form (100% transactional
read-modify-write, the maximum conflict surface). The rng family is
deliberately fresh (seed*6007+t vs ycsb_a's seed*7919+t) so the txn
rows can never silently alias the blind-path rows' random streams —
same-methodology means same generators AND distinct, reproducible ones.
