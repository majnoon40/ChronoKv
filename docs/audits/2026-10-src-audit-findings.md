# ChronoKV SRC audit (2026-10) — three findings, verified by execution

- **Round shape:** an external SOURCE-ONLY audit of `27bcbd8` (0.28.1 +
  TU split) — the auditor read code and never executed anything. Per the
  standing rules, every claim was re-verified HERE by execution before
  any fix landed; this document records the executed verdicts, the
  fail-first evidence, and the one contract question left open.
- **Findings:** A (MANIFEST open failure conflated with absence) —
  CONFIRMED, FIXED. B (D2 vs rollback failure) — CONFIRMED as a contract
  gap, reproduced, options proposed, **decision pending — no engine
  behavior changed**. C (test hygiene ×2) — CONFIRMED, FIXED.
- **Fault-injection infrastructure added for the round** (all
  `CHRONOKV_FAULT_INJECTION`-gated, zero production impact):
  `ManifestOpenFail` (all three MANIFEST readers), `TruncateFail` (the
  rollback ftruncate), `RotFinalDirFsyncFail` (the post-unlink dir fsync
  ONLY — DirFsyncFail cannot target it; earlier charges are consumed by
  the checkpoint and manifest dir fsyncs). Kind names added to
  `kind_from_name` and the `CKV_COVERAGE_FAULT` help list.

## Finding A — MANIFEST open failure treated as "missing" (Medium) — CONFIRMED, FIXED

**Claim (source-only):** `read_manifest()`'s `if (!f) return {1, 0};`
conflates ENOENT with every other open failure (EACCES/EMFILE/EIO),
contradicting its own comment ("ONLY 'file does not exist' is
legitimately {1,0}"); `read_manifest_dir()` and `recover_all()`'s
read-only open share the class; `db_has_data` uses `exists()` (a stat),
so it can report "has data" while the open failed.

**Execution verdict — confirmed, and worse than "wrong segment":**
with a faithful open-failure injection (failbit + errno=EIO on an
EXISTING MANIFEST) over the audit's prescribed shape (two non-empty
segments from a size rotation, MANIFEST naming segment 2):

    OPEN SUCCEEDED (silent path)
    put(d) -> 0          (write ACCEPTED)
    health level=0       (reported HEALTHY)
    reopen: a=1 b=1 c=1 d=1

The constructor reseeded `active_id_` to 1 and appended the next commit
to the STALE segment while the MANIFEST named segment 2 — accepted
writes landing in the wrong file behind a healthy facade. (With a
checkpoint-deleted segment 1 the same conflation instead bricks the
open via the missing-active-segment guard — loud, but misdiagnosed and
an availability failure.) Injection fidelity note, recorded because the
fail-first run caught it: a first-cut injector that merely `close()`d
the stream produced a MISDIAGNOSED loud failure (a closed-but-good
stream slips past `!f` into the truncation throw); a real failed open
sets failbit at construction. The injector now sets failbit explicitly.

**Fix:** all three readers — only a confirmed-ABSENT path
(`std::filesystem::exists`, which stats and needs no fd, so it answers
even under EMFILE) returns the sentinel; an existing path that fails to
open throws (D1) with errno. A genuinely absent MANIFEST is unchanged.
`db_has_data`'s `exists()` needs no change: post-fix, an unopenable
MANIFEST throws before that line is reached, and `exists()` correctly
answers the only question that site asks.

**Fail-first:** SRC-A battery checks FAILED pre-fix ("open SUCCEEDED
despite failed manifest open"), PASS post-fix; post-fault reopen
asserts the exact pre-test key set (a,b,c present; d absent).

## Finding B — D2 vs rollback failure (contract gap) — CONFIRMED, DECISION PENDING

**Execution verdict (probe `docs/audits/2026-10-src-b-probe.cpp`, output in the round
record):**
- **B1 — rollback fsync fails after a SUCCESSFUL ftruncate:**
  `put -> WalFailure(5)`, health level 2 "wal fail-stop mode active",
  FATAL logged, `truncate_fails=1`, subsequent writes `Failed(6)`,
  reads of durable data work; **clean restart: rejected key ABSENT**
  (truncation rode the page cache). Residual exposure: power loss only
  — the previously documented window.
- **B2 — the ftruncate ITSELF fails (injected):** identical caller-
  visible outcomes (WalFailure, fail-stop, WARNING, counter) BUT
  **clean restart: RESURRECTED(rejected)=1** — the rejected batch's
  CRC-valid frame stayed in the file and recovery replayed it. No
  power loss involved. The audit's "understated" case is real.
  Modeling limits stated: injected faults + clean restarts only; no
  power-loss claim is made or tested (dm-flakey remains v30 M3).

**Why it survives the reopen:** recovery has no way to distinguish the
poisoned tail from legitimate data — the frame is CRC-valid and
LSN-contiguous; the WalFailure existed only in the dead instance's
memory. README D2 wording has been narrowed to state the window
(allowed pre-decision); no engine behavior changed.

**Options for the maintainer (tradeoffs, no recommendation smuggled in):**
1. **Wording-only** (landed): D2 documents the window; operators treat
   `truncate_fails > 0` + WARNING/FATAL as "directory poisoned".
   Cost: the poison is silent across reopens — nothing on disk marks it.
2. **Persisted poison marker**: on rollback-truncate/fsync failure,
   best-effort write+fsync a POISON marker beside the WAL; reopen with
   a marker refuses (or opens read-only) until operator action.
   Cost: new on-disk artifact; the marker write can ITSELF fail
   (narrows, does not close, the window); operator burden.
3. **Bounded truncate retry** before giving up: narrows transient-
   errno cases; does not close persistent failures; trivially
   composable with 1 or 2.
4. **Distinct Status/health signal at failure time** (e.g. health
   reason "rollback-indeterminate"): cheap, improves observability,
   changes no on-disk state; still silent across reopens unless
   combined with 2.
(Refusing recovery of the poisoned tail WITHOUT a marker is not
constructible: the tail is byte-identical to legitimate data.)

## Finding C — test hygiene (×2) — CONFIRMED, FIXED

**C1: `st != Status::OK` in CKV-004R was too loose.** `map_txn_result`
maps WalFailure to `Status::WalFailure` (5) while the catch branch sets
`Status::Failed` (6) — distinguishable outcomes, and the loose check
accepted both (a CKV-016 rethrow regression would have passed as
"not OK"). Tightened to `!rethrew && st == Status::WalFailure`.
**Mutation evidence:** mapping WalFailure→Failed (scratch mutation M1)
FAILS the tightened check (the loose form would not); reverting the
CKV-004 stage gate to the class gate (M2) FAILS the sibling
resurrection check — both mutations caught, tree restored
byte-identical after.
**C2: README D3's "every fsync on the WAL path fail-stops" overclaimed
one deliberate exception.** The post-unlink directory fsync finishing a
checkpoint rotation is counted-and-warned, NOT fail-stop (the code
comment says so; the README did not). README narrowed to name the
exception and its soundness argument (MANIFEST rename already durable;
recovery dedupes resurrected covered segments ≤ ckpt_ts). New SRC-C
test injects exactly that site (dedicated `RotFinalDirFsyncFail` kind —
DirFsyncFail charges are consumed by earlier sites) and pins the full
contract: checkpoint succeeds, health < 2, counter moves, writes keep
working, reopen clean with the full key set. **Mutation evidence:**
making the site fail-stop (M3) FAILS SRC-C.

## Verification matrix for this round's changes

See the round's commit message for the executed 4-config results
(Release full suite; Stress build + DST; ASan+UBSan; TSan) and the
environmental caveats recorded in the delta-confirmation report.
