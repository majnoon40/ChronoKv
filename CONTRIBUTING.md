# Contributing to ChronoKV

The canonical process rules (1–14) live in `docs/ROADMAP.md`. This file is the
short form for a first contribution.

## Before you write code
1. **Measure first.** No performance claim without its methodology header
   (hardware, kernel, build flags, geometry, seed, durability class). A number
   without its header does not get quoted.
2. **Reproduce first.** Every fix lands with a regression test that FAILS on
   the unfixed tree. Record the failing output in the commit message.

## With the fix
3. **Docs in the same commit.** README/CHANGELOG/ROADMAP and the invariant
   matrix (`docs/INVARIANTS.md`) are updated in the commit that changes
   the behavior — never a follow-up.
4. **Name the invariant.** New contracts get a canonical ID (R/I/D/E/P/B/T)
   and a row in `docs/INVARIANTS.md`: enforcement → checker → vehicle → status.
5. **Mutation controls.** State which scratch mutation reddens which control,
   and the isolation claim. If a mutant is recorded but not executed, say so
   explicitly — the matrix distinguishes PROVEN from CHECKED for that reason.
6. **Replay handles.** Concurrency/crash failures must print
   (seed, op-count, last point) so they replay from the log alone.

## Bands and gates
7. **Never widen a band in flight.** Bands are recomputed only in a dedicated
   commit citing the engine change that moved them, from >= 7 post-change
   nights. A red gate re-runs ONCE; two consecutive REGRESSIONs are real.
8. **Geometry and workload-set changes are ledger-format changes.** They owe a
   recalibration commit that re-promotes the reference week.

## Build and test
    make release asan tsan stress
    make smoke_off_release smoke_off_asan smoke_off_tsan smoke_off_stress
    make lint

Sanitizer matrix and the DST/crash-fuzz regimes are the judges, not the local
release run. See README "Build and test".
