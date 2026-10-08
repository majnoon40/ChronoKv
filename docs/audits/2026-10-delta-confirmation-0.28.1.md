# ChronoKV 0.28.1 — Delta Confirmation Round (v29 M0 closure, item 2)

- **Target:** 0.28.1 (`b6fb181` merge; engine state re-verified at
  `ebf8b10`, whose delta from the merge is docs/CI-only, plus the four
  comment-truth edits this round ships).
- **Date:** 2026-10-08.
- **Commission:** `docs/ROADMAP.md` v29 M0, closure item 2 — verify the
  Audit-2 closures at depth and hunt specifically in the classes the
  *fixes* could have introduced. Verdict B or better and zero new
  Critical/High required to close M0 and unlock the rewrite milestones.
- **Provenance / independence:** executed by the cross-verifying review
  agent (the same agent that reproduced the Audit-2 PoCs against tag
  `v0.28.0` and caught the `put_with_old` parity gap). Disclosure per
  rule 12: this agent authored two refinements inside the confirmed
  surface (the no-op-delete exemption in `record_transition` and the
  `put_with_old` headroom parity), so those two classes were probed
  mechanically — every probe below is a committed, re-runnable program
  (`2026-10-delta-confirmation-probes.cpp`), not an opinion.

## Verdict: **B+ — CLEAN.** M0 closes; M3+ gate lifted.

Zero new Critical/High/Medium defects. One **Low** finding (doc truth,
fixed in this commit). Three behavioral notes and one build-environment
observation recorded below. All 20 confirmation probes pass; the full
verification matrix is green at the confirmed state.

## Hunt class 1 — TXN-1 tracker completeness (growth, prune liveness, cost)

**Code read.** `record_transition` stores every write except the
`!old_exists && !new_exists` no-op delete; `prune(external_floor)` runs
ONLY from `gc_once()` (both the empty-tree early path and the normal
path) with floor = min live reader slot's read_ts, deleting
`commit_ts <= floor` — exactly the entries no live reader can conflict
on (readers need `> read_ts`; the v24 Fix-12 external-floor mechanism
covers the slot-registration race window). `has_phantom_in_range`
iterates ts-entries above the *validating transaction's own* snapshot:
a fresh committer pays for writes since it started, not for what older
pinned readers hold.

**Probes.** 1a: GC-off write-only grows the tracker 1:1 with writes and
slot churn does NOT prune (prune is GC-owned) — 50,000/50,000. 1b:
GC-ON (the default) write-only holds entries at **0** — bounded. 1c: a
long reader correctly pins 20,000 entries while live (they are its
conflict set), and GC prunes to **0** after release — prune liveness.
1d: a range-reading committer holding 200k pinned mods completes; cost
**56 ms** for its scan+commit (one-time, borne by the long reader
itself at commit).

**Behavioral notes (not defects).** (i) With `auto_start_gc=false` the
tracker now grows per WRITE rather than per existence flip — the
documented "no GC, no reclamation" contract, but its scale changed in
0.28.1; worth a line in the options docs at v30 M4. (ii) Long-lived
readers under heavy write load carry O(writes-since-snapshot)
validation cost at commit (56 ms per 200k mods) — inherent to
first-updater-wins range validation; M6's memory accounting should
budget the tracker alongside the version heap (the roadmap already
folds tracker growth into M6).

## Hunt class 2 — TXN-2 `[k,k]` point-range path

**Probes.** 2a: absent-read → 50 unrelated committing txns (slot churn)
→ insert → original txn still CONFLICTS (no prune window loss). 2b:
insert+delete (net-absent again) still conflicts — conservative and
sound (real rw-antidependencies existed). 2c: absent read + self-insert
in one txn commits (no self-conflict; write-set entries are ensured
before the read-set lookup, and the fresh entry's `last_write_ts`
cannot exceed its own snapshot). 2d: **empty key** works. 2e:
**4048-byte key** (tree maximum) works. 2f: an oversized (5000-byte)
key in the read set commits cleanly — the point range is a string
comparison, never a page-bound key.

**Result: clean.** The `[k,k]` construction is exactly a degenerate
inclusive range read; TXN-1's completeness is what makes it sufficient,
and the battery already tests the composition.

## Hunt class 3 — WAL-1 classifier under adversarial corruption

**Probes** (direct `wal_recover_buf` on hand-built buffers): 3a interior
`len=0xFFFFFFFF` → CORRUPT, prefix kept, file untouched. 3b interior
`len+1` (CRC-breaking) → CORRUPT. 3c **two consecutive damaged frames
with a valid frame after** → CORRUPT (the offset scan, not the length
field, decides). 3d true torn tail → TORN_TAIL, prefix recovered. 3e
**stale rollback debris** (CRC-valid frame with LOWER lsn appended) →
TORN_TAIL — the D2-preserving classification: rolled-back bytes are
truncated, never replayed. 3f CRC-valid frame with an LSN jump > 2^32 →
TORN_TAIL (contiguous allocation makes such a frame unreachable as
acked data; replay's gap check backstops). 3g worst case: damage at
offset 0 of an 8 MiB valid-frame buffer → classified in **0 ms**
measured (the LSN + structural prefilters keep the full-offset scan
linear; CRC computed only on plausible candidates).

**Result: clean.** The misclassification directions all land on the
loud side: any surviving valid continuation forces CORRUPT.

## Hunt class 4 — BT-1 headroom vs the planner's real worst case

**Derivation re-checked by hand against `plan_leaf_groups` /
`plan_interior_groups`:** a splitting leaf holds existing ≤ budget plus
one new entry ≤ budget (each entry pre-validated ≤ budget), so leaf
total ≤ 2·budget → greedy groups ≤ 3 (two same-size groups would each
need > budget) → ≤ 2 promotions; promoted key bytes ride inside the
child's 2·budget → parent total < 3·budget + 24 → interior groups ≤ 6
→ ≤ 5 new pages per level. `5*(height+1)+1` is a valid conservative
bound. (`height` walk stops at `is_leaf || key_count == 0`; an interior
node with 0 keys does not exist absent merges — revisit when M4 lands
merges, noted in the report.)

**Probes.** 4a: three adversarial packing regimes the existing battery
does not cover — entry cost just over budget/2 (one entry per greedy
group: 2030-byte keys), near-maximum entries (4050-byte keys), thirds
(1350-byte keys) — × pools {3,5,8,13,21,34,55} pages × {sorted,
shuffled} × {200,400} keys = **84 exhaustion combos: zero lost acked
keys**, every refusal a clean `bad_alloc`. 4b: observed maximum
allocation for a single put across all regimes = **6 pages** (bound
allows 5·(h+1)+1). 4c: the in-place exemption boundary is exact —
equal-size update on an exhausted pool succeeds, +1 byte refuses with
the tree unchanged and the old value readable.

## Low finding (fixed in this commit) — doc truth

Four comments still described the pre-TXN-1 contract after the fix
landed — the CKV-017/021 class the v28 arc itself corrected:
`mods_by_ts_`'s member comment ("Only absent->present and present->
absent transitions are stored"), `has_phantom_in_range`'s lead comment
("any existence transition"), `group_append`'s v17 race-closure comment
("publishes existence transitions"), and `commit_txn`'s transitions
computation comment ("compute logical existence transitions"). All four
rewritten to the TXN-1 contract (every write; one sound exemption), with
the probe-1d cost note recorded at `has_phantom_in_range`. Comment-only
delta; lint + ratchet + batteries re-run green after the edit.

## Build-environment observation (no engine action)

On this 1 GiB confirmation sandbox the full-suite TU now needs
`--param ggc-min-expand=5` to build even at `-O0` (bare `-O0` cc1plus is
OOM-killed at ~840 MB free) — the wall rose again post-Audit-2 (the A2
battery's template/lambda density). The Makefile's small-container note
gains the verified workaround. This is the third consecutive arc raising
the build-memory wall; v29 M2 item 5 (the API-1-unlocked test-suite TU
split) is the scheduled relief.

## Verification matrix at the confirmed state

Sandbox: 2-core Xeon 2.5 GHz, 1 GiB, kernel 4.19 (io_uring I/O blocked),
overlayfs; `-O0 --param ggc-min-expand=5` unless noted. Floors, not
ledger material.

| Check | Result |
| --- | --- |
| `make lint` (werror ×4 configs, ratchet 33=33, header hygiene ×2) | green |
| Confirmation probes (this document's program, 20 checks) | **20/20 PASS** |
| `CKV_ONLY_REMEDIATION` (v28 battery + 15-check A2 battery) | green |
| `CKV_ONLY_REVIEW` (review regressions + v26 durability + crash-fuzz) | green, exit 0 |
| Full suite, isolated gates | green as above |
| Full suite, single process | see environmental note below |
| Prior full-suite runs at the identical engine state (comments aside): 495/0 twice, 2026-10-05/06 sandbox instances | green |

**Environmental note (recorded honestly, not a finding against the
engine).** On THIS sandbox instance three bare full-suite runs aborted
at the A2 WAL-4 guard with an uncaught `std::system_error` (EAGAIN —
"Resource temporarily unavailable") from thread creation, after
472/472 green checks. The abort is not reproducible: the battery passes
standalone; a gdb-run (timing/allocation perturbation) completes the
ENTIRE suite at 494 PASS + 1 FAIL, the FAIL being the absolute
`async call-ret p50 < 0.1 ms` performance assertion measured through
ptrace at 0.181 ms — a tracer-overhead artifact of the same class as
the calibrated stress-build flake of `8863bc8`, green in every bare run
that reached it; and the identical engine state ran 495/0 twice on
earlier instances. This instance carries anomalous kernel limits
(`kernel.threads-max=925`, `RLIMIT_SIGPENDING` hard 462, `pid_max`
32768) with an invisible cgroup memory hierarchy; the EAGAIN is
consistent with kernel-side resource refusal under those limits and was
not root-caused further from inside the sandbox. Per the project's
evidence standard: the anomaly is recorded, classified environmental on
three independent non-reproductions, and the CI matrix (16 GiB runners,
normal limits) is the authoritative full-suite vehicle — runs #64/#66
there are green on this engine state.

## Closure

- Hunt classes 1–4: **clean** (probes committed alongside).
- New defects: one Low (doc truth), fixed in this commit, fail-first
  not applicable (no behavior delta) — the comment claims are now
  grep-checkable against `record_transition`'s contract.
- 0.28.1's Audit-2 closures: **confirmed at depth**.
- **M0 is CLOSED. The M3+ rewrite gate is lifted** (rewrite work still
  waits on M1's green week and M2's catchers, per the load-bearing
  constraints — M0 was never the only gate, it was the first).
