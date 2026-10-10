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

---

## Addendum (2026-10-10, post-decision): option 4 shipped; zero-fill probe executed

### Option 4 — SHIPPED (maintainer decision item 1)

Both rollback-failure branches (ftruncate failure; post-truncate fsync
failure) now set an in-memory `rollback_indeterminate_` latch on
WalSegments, surfaced through `health()` as a second reason beside the
D3 fail-stop:

    rollback-indeterminate: a rejected batch's rollback (ftruncate or its
    fsync) failed — its CRC-valid frames may persist in this WAL and replay
    on a future open (SRC-B window; in-memory signal, not on disk)

No on-disk change, no new public Status. Fail-first: the SRC-D battery
check (both branches, exact `Status::WalFailure` + level 2 + reason
presence + charge accounting) FAILED pre-implementation
(`reason_present=0`), PASSES post. The SRC-A2 control check pins the
genuinely-absent-MANIFEST fresh-database path (static `{0,0}` contract,
instance `{1,0}` fresh open, recover_all filename inference on reopen)
so the finding-A throw cannot regress it. SRC-A nit applied: errno is
captured into a local at all three throw sites BEFORE
`filesystem::exists()` runs, and the local is what the message carries.

### Option 3 (bounded ftruncate retry) — SKIPPED, with reasons

Per the decision's condition ("only if you can show it is safe"):
safety is trivial (bounded loop, latch untouched, falls through to the
existing failure path), but the VALUE cannot be shown: the only errno
where retrying helps beyond what the zero-fill candidate already covers
is EINTR; device-class EIO fails the retry identically; EBADF/EINVAL/
EROFS are non-transient. Skipping avoids adding a delay to an
already-failing path for no demonstrated gain. If zero-fill lands,
EINTR is subsumed (the fill's pwrite succeeds after an interrupted
truncate).

### Zero-fill neutralization candidate — PROBE RESULTS (decision item 3; NOT implemented)

Probe source: `docs/audits/2026-10-src-b-probe.cpp` (the B1/B2 repro) +
the zero-fill probe re-run below (filler verified as true NUL bytes,
`char(0)`; an initial run accidentally used the character '0' — both
fills classify identically, and the NUL run is the one quoted).

**(a) How does recovery treat a zero-filled tail at EOF?**

    == (a1) classifier: valid frames + ZERO TAIL at EOF
       status=1 (0=OK 1=TORN_TAIL 2=CORRUPT) records=2
    == (a2) classifier: ZERO FILL in the INTERIOR (valid frame after)
       status=2 records=1
    == (a3) engine-level: real db, zeros appended at EOF, reopen
       reopen OK: k1=1 k2=1  size_after=78 (zero tail truncated)

A zero tail at EOF is a TORN TAIL: `wal_recover_buf`'s parse fails at
the first zeroed frame (len=0 < 20), the WAL-1 offset scan finds no
CRC-valid continuation, classification is TORN_TAIL, and the existing
torn-tail repair truncates it — engine-level reopen keeps the full key
set. Zero-fill in the INTERIOR (a valid frame after it) is CORRUPT —
loud, untouched (WAL-1/CKV-019 semantics). The candidate is therefore
only sound while the fill sits at EOF — which the fail-stop guarantees:
no batch is ever appended after a failed rollback.

**(b) With TruncateFail injected, does the rejected key stay ABSENT
after a clean reopen?** (external zero-fill of `[batch_start, EOF)` +
fdatasync, engine untouched):

    put(rejected)=5 (5=WalFailure)  S0=41 S1=85 frame=44  health=2
    zero-fill pwrite=44/44 fdatasync=0
    reopen: acked=1  REJECTED-PRESENT=0  size_after=41

YES — the fill converts the poisoned tail into the ordinary torn-tail
shape; the rejected key is absent, the acked key survives, recovery
truncates the zeros (85 → 41).

**(c) Is "pwrite still works" realistic where ftruncate returned EIO?
Honest errno-class analysis:**

| ftruncate errno | does pwrite+fdatasync still work? | verdict |
| --- | --- | --- |
| EINTR | yes (transient signal) | zero-fill helps; retry would too |
| EIO — storage/device class | pwrite lands in page cache, but its **fdatasync likely fails the same way** → neutralization not durable | does NOT reliably help; the flag must stay set |
| EIO — setattr-path class (NFS SETATTR vs WRITE are different RPCs; fs-specific truncate bugs) | plausibly yes — data path and metadata path genuinely differ | helps (narrow but real) |
| EBADF / EINVAL | no — dead fd / programming error | does not help |
| EROFS (read-only remount) | no — pwrite fails too | does not help |
| ENOSPC | shrink-truncate essentially never fails with it; zero-fill writes into already-allocated space (no allocation) | n/a |
| EMFILE/ENFILE | not applicable to either call (fd already open) | n/a |

So the candidate's real coverage is EINTR plus the setattr-broken-but-
data-path-alive class; in the device-death class it fails harmlessly
(its own fdatasync error keeps `rollback-indeterminate` set — the
fallback must be implemented as best-effort with the flag cleared ONLY
on fill+fdatasync success). Cost ≈ 15 lines in one branch; no format
change; composes with fail-stop and option 4.

**Recommendation (for the decision, not implemented):** implement the
zero-fill as best-effort neutralization inside the truncate-failure
branch (fill → fdatasync → clear/keep the flag on the fdatasync
result), because it converts the confirmed clean-restart resurrection
into an already-tested recovery shape at trivial cost and with a
fail-harmlessly profile. The persisted poison marker (option 2) remains
the ONLY mechanism that survives the reopen itself — if the zero-fill's
fdatasync also fails, or the process dies before the fill, the next
instance still cannot tell. Decision deferred to the maintainer as
instructed; nothing beyond option 4 was implemented.
