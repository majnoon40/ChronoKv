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


---

## SRC-B DECISION (maintainer, 2026-10-10): zero-fill IMPLEMENTED; persisted marker DEFERRED

Decision: implement best-effort zero-fill neutralization in the rollback-ftruncate-FAILURE
branch only; DEFER the persisted poison marker (revisit after v30 M3 power-loss infra). The
rollback-fsync-failure branch is untouched. The window is documented as NARROWED, not closed.

What shipped (engine, ~60 lines in the `trc != 0` branch):

1. Extent: the FULL attempted batch extent `[batch_start, batch_start + batch_bytes)` — every
   record of a multi-record group-commit batch — zeroed via a chunked `pwrite` loop that
   retries short writes and `EINTR` (k<0 && EINTR → retry; k==0 → EIO fail).
2. Then `fdatasync`. `rollback_indeterminate_` is cleared ONLY if fill AND fdatasync both
   succeed. `failed_` stays latched regardless (D3/fail-stop untouched).
3. Fill or fdatasync failure: flag stays, WARNING logged, no throw out of the failure path, no
   new Status. Allocation failure inside the fill is caught (`fill_ok = false`).
4. Append-prevention after the zero region (code citations): `group_append` entry
   `if (failed_) return {1, 0};` (no new batch enqueues); leader drain
   `if (failed_) { leader_threw = true; throw ...; }` BEFORE any batch I/O (an already-queued
   batch fails, never writes); `commit_txn` gate `if (wal_ && wal_->is_failed()) return
   TxnResult::DatabaseFailed;`. SRC-E1 asserts this empirically (post-failure put → Failed,
   file size frozen).

Test seams added (no production behavior): `wal_zerofill_fail_write_for_test()` /
`wal_zerofill_fail_sync_for_test()` one-shot hooks with consumption witnesses (anti-vacuity),
and `leader_pre_drain_hook_` (pauses the elected leader in the election→drain window — makes
multi-record batches DETERMINISTIC in release builds; without it every batch on the 2-core box
was single-record: 16/16 attempts split, `batch_bytes=8230 expected=8350`).

Tests: SRC-D B2 flipped to the neutralization contract (reason CLEARED + rejected ABSENT after
clean reopen); SRC-E1 (4-record batch — 3 small + 1 spanning >1 page (8 KiB) — byte-exact
extent assertion, all rejected absent, acked prefix intact, reopen size == batch_start, open
does NOT throw (no interior-CORRUPT), no append after); SRC-E2 (fill-write fails → flag stays,
reason present, hook consumed); SRC-E3 (fill-sync fails → flag stays, reason present, hook
consumed). Fail-first: all four FAIL without the fill block (E1 detail: `batch_bytes=8350
expected=8350 ... reason_gone=0` + resurrection on reopen), PASS with it. Mutations: M4
(extent shortened by one record) → SRC-E1 FAILS with `reopen threw: recovery failed: WAL
corruption in segment 1` (the surviving CRC-valid frame after zeros classifies interior CORRUPT
— loud, exactly as the probe predicted); M5 (flag cleared unconditionally) → SRC-E2+E3 FAIL,
nothing else. Header restored byte-identical after each.

Residual window (documented in README D2): (i) the fill's fdatasync failing; (ii) process death
before the fill completes; (iii) errno classes where pwrite fails too (device EIO, EROFS,
EBADF); (iv) the untouched rollback-fsync-failure branch (power-loss-only; SRC-D B1 pins the
clean-restart behavior). The persisted poison marker remains the only mechanism covering
reopen-crossing cases — deferred to v30 M3 as decided. Modeling limit unchanged: crash/model-
level only, no power-loss claim.


---

## PROVENANCE NOTE (maintainer review round 2, 2026-10-10)

**The fee2789-turn verification numbers — "make lint clean / ratchet 33=33" and "FULL SUITE
508 PASS / 0 FAIL" (both recorded in the fee2789 commit message) — are superseded by the
5d037ba run** (four lint gates exit 0; ratchet 33=33; release -O1 full suite 511 PASS / 0
FAIL; CI run #80 success, 23/23 executed jobs, arena nightly skipped).

Provenance, re-derived by execution on 2026-10-10 — the fee2789-era sandbox logs were
overwritten by the 5d037ba verification window (every file in the log directory postdates
it), so the original runs could not be re-inspected, only re-executed:

* `scripts/lint.sh` — cited by earlier chat reports as the lint runner — has NEVER existed
  in this lineage: `git log --all -- scripts/lint.sh` is empty; neither stale sandbox
  working copy (`chronokv/`, `chronokv-work/`) contains it; neither superseded patch
  iteration (`chronokv-fixes-v3-with-ci.diff`, `chronokv-audit2-plus-ci.patch.txt`) creates
  it. The real gates are the Makefile `lint` aggregate (Makefile:207) and its four
  sub-targets (`lint-conversion` runs `scripts/lint_wconversion.sh` against
  `ci/wconversion.baseline`). Report lines citing `scripts/lint.sh` are void.
* The 508 PASS / 33=33 numbers themselves were NOT stale-tree artifacts. A fresh
  `git worktree add "$ARENA_WORKSPACE"/wt-fee fee2789` of the REAL tree, rebuilt and re-run on
  2026-10-10, reproduces both exactly:

      build_exit=0
      run_exit=0            (6m05s wall)
      PASS_count=508
      FAIL_count=0
      ChronoKV 0.28.1 - ALL TESTS PASSED
      -Wconversion warnings: 33 (baseline 33, g++)

  The 5d037ba suite adds exactly three checks (SRC-E1/E2/E3): 508 + 3 = 511, matching the
  authoritative 5d037ba run (`full_release.log`: 511 PASS / 0 FAIL).

---

## SRC-E4 — PARTIAL-FILL GAP (maintainer review round 2, 2026-10-10): finding, fix, evidence

**Finding (maintainer):** SRC-E2/E3 arm the fill-write failure against a single-record
44-byte batch, so the failed fill writes ZERO bytes. The shape where SOME zeros land and
THEN the fill dies (batch spanning more than one 64 KiB chunk, Nth fill pwrite fails) was
untested; the incomplete-fill WARNING carried none of the facts an operator needs.

**Shipped:**

1. Test hooks (`CHRONOKV_TEST_HOOKS`-gated, one-shot, consumption witnesses):
   `wal_zerofill_fail_pwrite_nth_for_test` (N>0: the N-th fill pwrite ATTEMPT dies EIO; the
   slot decrements per attempt), `wal_zerofill_short_write_for_test` (the next fill pwrite
   performs a GENUINE short write — half the remaining chunk, min 1 byte — real bytes on
   disk; the loop must CONTINUE, not fail), `wal_zerofill_pwrite_attempts_for_test`
   (per-run attempt counter, reset at fill start), `wal_zerofill_{nth,short}_fired_for_test`
   (witnesses), and `wal_zerofill_last_run_for_test()` — a run record {segment path,
   batch_start, extent, bytes zeroed, fill_ok, sync_ok}: the same facts the WARNING prints,
   which makes the repair procedure testable. Database seam:
   `rollback_indeterminate_for_test()` (the raw latch, not the health string).
2. The incomplete-fill WARNING (the round's ONLY production change) now names the segment,
   the batch offset, the zeroed-of-attempted byte counts, and says the WAL may refuse to
   open until the segment is manually truncated to that offset.
3. SRC-E4a/E4b: 4-record group-commit batch of 40 KiB values (`'P'`-filled, so survivors
   are visible) = 163,992 B = three 64 KiB fill chunks, collected deterministically via the
   SRC-E1 `leader_pre_drain_hook_` seam; the full batch lands (16 MiB pwrite budget), the
   rollback ftruncate fails (armed `TruncateFail`), fill attempt 1 short-writes 32,768 B,
   attempt 2 completes chunk 1, attempt 3 dies EIO (nth=3). Asserted: exactly 65,536 of
   163,992 bytes zeroed; `[batch_start, +64K)` reads back ALL ZERO from the file with
   surviving frame data at `+64K`; `rollback_indeterminate_` STAYS set (raw latch + health
   reason); instance fail-stopped (level 2, next put `Failed`, size frozen); reopen THROWS
   (loud interior-CORRUPT refusal — no silent replay is possible because the database does
   not open); operator repair — copy aside, truncate at the offset, restart — recovers
   (clean open, acked keys intact with correct values, ALL rejected keys absent, writes
   work again).
4. README D2: partial-fill paragraph + the repair procedure; "Pinned by" extended with
   SRC-E4. CHANGELOG bullet added.

**Why the refusal (not a silent repair) is the correct pinned behavior:** the zeros are
INTERIOR — surviving CRC-valid frames of the rejected batch follow them — and
`wal_recover_buf`'s WAL-1 scan (`valid_after`) classifies that CORRUPT by design: an
interior hole must never be auto-truncated, because in general valid acked frames can live
after it. Here the fail-stop guarantees the tail is only the rejected batch, so the manual
truncate-at-offset is safe — and it is a human decision, taken with the WARNING's facts.

**Fail-first:** with hooks + test in place and the OLD warning text, SRC-E4b FAILED while
SRC-E4a passed — the refusal/repair behavior is pre-existing (WAL-1), the observability was
the gap:

    SRC-E4a partial fill: ... :  PASS
    SRC-E4b incomplete-fill WARNING names segment, offset, zeroed count, and says the WAL may refuse to open:  FAIL
       (INCOMPLETE=1 may_refuse=0 segment=0 offset=0 counts=0 attempts=3 nth_slot=0 zeroed=65536 extent=163992 fill_ok=0 sync_ok=0 path_ok=1)
    SRC-AUDIT REGRESSION FAILURES: 1

After the WARNING upgrade both PASS (remediation battery 90/90, exit 0).

**Raw output (release -O1, this sandbox, 2026-10-10; the test captures the leader's stderr
during the failed rollback and prints it):**

    WARNING: ftruncate failed after WAL durability failure (errno=5) — non-durable record may persist in the WAL file
    WARNING: rollback zero-fill INCOMPLETE (fill=0 fdatasync=0 errno=5; 65536 of 163992 bytes zeroed at offset 86 of /tmp/ckv_src_e4.LQAZg2/wal/wal_000001.log) — rollback-indeterminate stays set; the rejected batch's surviving frames may persist, and the WAL may refuse to open (interior-CORRUPT) until the segment is manually truncated to offset 86 (README D2 repair procedure)
    [witnesses] attempts=3 nth_slot=0 short_fired=1 nth_fired=1 zeroed=65536/163992 fill_ok=0 sync_ok=0 batch_start=86 segment=/tmp/ckv_src_e4.LQAZg2/wal/wal_000001.log
    [reopen] threw=1 refused=1 what="recovery failed: Recovery failed: WAL corruption in segment 1"
    [repair] copy aside + truncate -s 86 /tmp/ckv_src_e4.LQAZg2/wal/wal_000001.log + restart -> reopen_ok=1 acked_intact_rejected_absent=1 writes_work=1
    SRC-E4a partial fill: zeros land then fill dies — flag stays, fail-stop, LOUD corrupt refusal, truncate-at-offset repair recovers:  PASS
    SRC-E4b incomplete-fill WARNING names segment, offset, zeroed count, and says the WAL may refuse to open:  PASS

**Mutation M6 (anti-vacuity for the refusal):** classify a zeroed length field as TORN_TAIL
even when CRC-valid frames survive after the zeros (`valid_after && !m6_zeros`) → SRC-E4a
FAILS with `refused=0` as the ONLY flipped bit — the partial-fill reopen silently
auto-truncates instead of refusing — and nothing else in the remediation battery moves:

    [reopen] threw=0 refused=0 what=""
    SRC-E4a ...:  FAIL
       (zero_prefix=1 survivor_nonzero=1 flag_set=1 reason=1 level=2 post=6 size_frozen=1 refused=0 [] repair_ok=1 keys_ok=1 write_ok=1)
    SRC-E4b ...:  PASS
    SRC-AUDIT REGRESSION FAILURES: 1

Header restored byte-identical after the mutation (sha256
`c081efad931e72a6bbd1b127b53415dc3c6a13cc1a092461219f3da41a38b24e`, verified) and the
battery re-run green (90/90).

**Operator procedure (documented in README D2, executed by SRC-E4):** on
`rollback-indeterminate` with an INCOMPLETE zero-fill WARNING — (1) the instance is already
fail-stopped: no further writes can extend the damage; (2) stop the process cleanly; (3)
copy the named segment aside; (4) `truncate -s <offset> <segment>` using the offset from the
WARNING — everything at/after it belongs to the rejected batch (the fail-stop guarantees
nothing appended after it; every acked record precedes it); (5) restart: recovery opens
clean, acked data intact, rejected batch absent. If the WARNING instead reports a full
zero-fill failure (0 bytes zeroed) the segment tail may still hold CRC-valid rejected
frames — restore from backup or apply the same truncate-at-offset repair after verifying
the offset.
