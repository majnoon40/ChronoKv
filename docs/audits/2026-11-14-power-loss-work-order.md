# Power-loss validation work order — v30 M3, finding #1 (2026-11-14)

Status: WORK ORDER (agent-executed infrastructure). This document is the
deliverable; no code, CI job, or runner change lands with it. Severity basis:
the durability table's "power loss" cell is a design target, not a measured
property (README, this same date); WAL-3's target half is STRUCTURAL with the
recorded boundary that NO in-process proof of it can exist (`_exit()` leaves
the kernel page cache intact — the page cache survives, so "fsynced" and
"still dirty" are indistinguishable to a process-crash harness).

## Goal

Convert "power loss" from design target to measured property, and close the
one invariant half that cannot be proven in-process: WAL-3's target-directory
correctness (a wrong-target `fsync` is invisible to `_exit()` testing because
the page cache survives — recorded as G-1 in `docs/INVARIANTS.md` §5, closed
there by indexing with exactly this deferral).

## Vehicle

`dm-flakey` (drop all writes for a window, then restore) and `dm-dust`
(selective bad-block write failures) over a loop device holding the WAL
directory. Requires `CAP_SYS_ADMIN` and device-mapper — GitHub-hosted runners
will not do this; use a privileged self-hosted runner or a privileged
container on a controlled host. That runner decision is the blocking item,
exactly as the M4 soak-vehicle decision is (ROADMAP risk C2 already names
the plan B: `scsi-debug`, or a documented local VM rig + published logs +
reduced CI leg). While the rig is being decided, wire the CI skeleton so the
vehicle lands into an existing job shape, not a new one.

## Method

1. Build the WAL on the dm device; run a workload with acknowledged writes.
2. At each named boundary, switch the target to drop-writes, then hard-cut
   the process (`_exit`) AND drop the page cache by destroying the device
   mapping — the point is that unsynced data must genuinely vanish.
3. Restore the target, reopen, assert.

## Boundaries to cover

Map to the EXISTING crash points so the ledger stays comparable (kAll 28
named windows, `engine-tests`/`crashfuzz` vehicles): `wal_after_write` /
`wal_after_fsync`, `seg_before_dir_fsync` / `seg_after_dir_fsync`, MANIFEST
write/rename, checkpoint delta rename, rotation's post-unlink dir fsync (the
SRC-C site), and the rollback `ftruncate` + its fsync.

## Assertions per boundary

No acknowledged write is lost (D1 per durability class); no `WalFailure`'d
batch resurrects (D2); fail-stop latches where required (D3); recovery either
opens correct or refuses loud — never silently wrong.

## The WAL-3-specific test (the one that cannot exist today)

Inject a build where `rotate_after_checkpoint` passes the wrong path to
`fsync_dir` (the pre-fix defect — the fixed call carries the anchor comment
"WAL-3: a file IN dir_, so dir_ itself is synced"). Under `_exit()` testing
this is invisible and the suite stays green — that is the recorded gap
(`docs/INVARIANTS.md` §2.2 WAL-3, notes column). Under dm-flakey it must FAIL
(the MANIFEST rename is not durable). That old-GREEN/new-RED pair is the
break-proof that moves WAL-3's target half from STRUCTURAL to PROVEN.

## Acceptance

- The boundary matrix above green on the fixed tree.
- The WAL-3 wrong-target mutant red under dm-flakey and green under `_exit()`
  (the gap demonstrated, then closed).
- Rows updated in `docs/INVARIANTS.md` §2.2 and §3.
- The README durability table's "Validated today" column upgraded only for
  the classes actually measured.

## Sequencing

M0-calibration gap list (`docs/INVARIANTS.md` §5) and ROADMAP v30 M3 both
point at this leg; nothing here races the probe verdict (finding #10) — the
runner decision can proceed in parallel, the calibration dispatch cannot.
