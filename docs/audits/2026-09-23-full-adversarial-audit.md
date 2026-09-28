# ChronoKV Full Adversarial Audit

- **Repository:** https://github.com/majnoon40/chronokv
- **Commit reviewed:** 8c77a7ce78157c8ed8487a595a5066769fc8bbd9 (2026-09-23, "Create audit guidelines for ChronoKV repository")
- **Version:** 0.27.0 (CHRONOKV_VERSION == README == commit history; PATCH in lockstep)
- **Date:** 2026-09-23

## 1. Executive Summary

**Files inspected.** All 10 repository files: chronokv.hpp (12,228 lines - the entire engine and public API, read end-to-end across all eight sections), main.cpp (12,120 lines - the full test suite, including crash-fuzz, DST harness, lincheck, PITR/backup/lifecycle batteries), Makefile, .github/workflows/ci.yml, scripts/fault_coverage.py, README.md (515), docs/ROADMAP.md (956), LICENSE, .gitignore, plus the git history (56 commits, with targeted inspection of the 14 most concurrency- and durability-sensitive commits).

**Major subsystems inspected.** CRC/IO primitives; WAL framing and parsing; io_uring wrapper (live-kernel verified); WalSegments (group commit, rotation, MANIFEST, recovery); PublicationTracker; PhantomTracker (SSI); MVCC version store and epoch-pinned reclamation; ChronoKV core (commit_txn, checkpoint, GC, recover); Read/WriteTransaction; Database/Transaction/Batch/observer/stream/async/backup/PITR public API; PagePool and B+ tree (all functions); test suite and CI matrix.

**Findings.** 13 confirmed defects (3 Critical, 4 High, 6 Medium), 3 high-confidence defects (all Low except the documented-latent CKV-008 mechanism, which is confirmed with a deterministic reproducer but masked in-engine today), 5 additional low/documentation/test findings, and 2 test-infrastructure defects that materially hide bugs (CKV-018 among them). Every Critical and High finding was reproduced first-hand by this audit - ASan/Sanitizer PoCs for the memory-safety and corruption findings, deterministic scheduler PoCs for the concurrency finding, and public-API-only PoCs for the data-loss, stream, and observer findings.

**Important testing gaps.** The suite's blind spots map one-to-one onto the Critical findings: no key larger than ~100 bytes ever enters the tree (CKV-001/002 invisible), no allocation failure is ever injected (CKV-003 invisible), no 0xFF-prefixed key is ever scanned (CKV-005 invisible), no tombstone ever falls inside a streamed range (CKV-006 invisible), no Async committer ever hits a write-stage failure and the WAL write path has no fault hooks at all (CKV-004/018 invisible), and no observer callback ever throws (CKV-007 invisible). The deterministic-scheduler and lincheck investments are genuine strengths, but lincheck verifies SI + real-time order and is mathematically blind to write-skew / anti-dependency regressions, and the DST harness covers four scenarios that do not include checkpoint-vs-commit or multi-writer tree paths.

**Overall safety assessment.** The WAL/lifecycle half of the engine is unusually well-engineered and honestly documented - the group-commit state machine, the fail-stop machinery, the crash-fuzz discipline, and the v25.8 close-race keepalive design were each verified sound in this audit, and the project's fix-with-detector discipline is real. The B+ tree half is not at the same standard: it has no exception safety, no capacity arithmetic on its split paths, an unsound OLC validation predicate, and its engines-level integration multiplies the blast radius (a transient OOM becomes total data loss via checkpoint). With confirmed memory corruption reachable by a single legal API call and a confirmed silent-total-data-loss path, the repository is not production-safe today.

## 2. Critical Findings

### [CKV-001] Oversized key wraps uint16_t page offset: ~61 KB heap-buffer-overflow write via one legal Database::put

- **Severity:** Critical
- **Confidence:** Confirmed
- **Component:** B+ tree (page layout / split-rebuild path)
- **Files:** chronokv.hpp
- **Functions:** insert_into_leaf_no_split, split_leaf, put_leaf_nolatch, put_recursive, BTree::put; root-split writers in BTree::put and BTree::put_with_old; insert_into_interior_no_split
- **Approximate lines:** 11282-11285 (primary); 9961-9963 and 10001-10003 (root split); 11583-11584 (interior rebuild)
- **Risk:** Impact 5 x Likelihood 3 x Detectability 5 = **75**

**Summary**

The B+ tree stores variable-length keys inside a 4096-byte page using 16-bit byte offsets. The direct-insert path validates that an entry fits a page before writing, but the split-rebuild path (insert_into_leaf_no_split, used by split_leaf to rebuild a half of the page after a split) performs no fit validation at all. A key larger than the usable page capacity makes the computed key offset wrap modulo 2^16, and the subsequent memcpy writes the key bytes far past the page. The engine's only key-size gate is MAX_KEY_BYTES = 65535 (line 2064, enforced at commit_txn line 5885), so any key between roughly 4,052 and 65,535 bytes is accepted by the public API and reaches this write.

**Root Cause**

insert_into_leaf_no_split computes value_off = uint16_t(free_hi(p)) - value.size() and key_off = value_off - key.size() in uint16_t arithmetic, then memcpy's key.size() bytes to p->bytes + key_off with no capacity check. put_leaf_nolatch (11152-11163) checks needed > free space only on the non-split path; when the entry does not fit it calls split_leaf, whose rebuild helper assumes every entry fits an empty page. The same uint16 wrap pattern exists in the root-split writers (9961-9963, 10001-10003: key_off = PAGE_SIZE - split_key.size()) and in insert_into_interior_no_split (11583-11584), reached once any separator key exceeds 4 KiB.

**Trigger**

A single call: db.put(std::string(60000, 'K'), "v") — the key passes MAX_KEY_BYTES (65535), the only size gate the engine enforces before the tree.

**Incorrect Result**

A 60,000-byte memcpy executes ~5.5 KB past the end of the page-pool allocation (ASan: heap-buffer-overflow WRITE of size 60000 at chronokv.hpp:11285, 5528 bytes past an 8192-byte pool). With the default 256 MiB pool the write lands inside the pool and silently cross-corrupts live pages instead of crashing.

**Expected Result**

The put is rejected (Status::TooLarge), or the tree supports large keys; no out-of-bounds write occurs.

**Violated Invariant**

Page bounding box: every slot/key/value write must stay inside the 4096-byte page.

**Why Existing Protection Fails**

The only fit checks live on the non-split insert path and in the API layer's MAX_KEY_BYTES gate, which permits keys 16x larger than a page. The split-rebuild path has no check; hazard pointers, latches, and the engine's nm_ serialization protect concurrency, not bounds.

**Existing Test Coverage**

No test in the suite inserts a key larger than ~100 bytes into the tree (btree fuzz and depth tests use short keys), so none of the four wrap sites can fire in CI.

**Reproducer**

```
poc/poc2_engine.cpp mode bigkey (engine-level, hooks-off): Options with page_pool_bytes=8192, db.put(60000-byte key) -> ASan heap-buffer-overflow WRITE 60000 at 11285, stack Database::put -> commit_txn:5936 -> ensure_index:11839 -> BTree::put:9940 -> put_recursive:10971 -> put_leaf_nolatch:11162 -> split_leaf:11258 -> insert_into_leaf_no_split:11285. Standalone: poc/poc1_standalone.cpp mode oob1.
```

**Fix Direction**

Single-source a page-capacity check: reject any key (and any entry) whose encoded size exceeds the page's usable capacity at the API layer (cap MAX_KEY_BYTES at a page-safe bound such as PAGE_SIZE minus header/slot overhead), and add defensive fit checks in insert_into_leaf_no_split / insert_into_interior_no_split / the root-split writers that throw before any memcpy. Overflow-page support is a larger design change; until then large keys must fail loudly.

**Regression Test**

Public-API test: put keys of 4090, 4096, 60000, and 65535 bytes must all return Status::TooLarge (or otherwise be rejected without mutation); a 4000-byte key must insert and read back. Run under ASan. Also assert BTree::verify_leaf_chain/verify_separator_invariants stay green after the rejection.

---

### [CKV-002] split_leaf balances entry COUNT, not bytes: page-legal keys overflow a half-page and silently corrupt live pages

- **Severity:** Critical
- **Confidence:** Confirmed
- **Component:** B+ tree (split path)
- **Files:** chronokv.hpp
- **Functions:** split_leaf, insert_into_leaf_no_split
- **Approximate lines:** 11220 (mid = entries.size() / 2) feeding the unguarded rebuild at 11282-11285
- **Risk:** Impact 5 x Likelihood 3 x Detectability 5 = **75**

**Summary**

Leaf splits choose the split point by entry count (the median slot), not by byte size. When a leaf holds many small entries and one multi-kilobyte entry, the right half receiving the large entry can exceed the 4096-byte page in bytes even though every individual entry is page-legal. The rebuild writes past the page (wrapping uint16 offsets as in CKV-001, or clobbering in-page slot metadata), corrupting adjacent live pool pages. The failure is silent at write time and manifests later as mass key loss, disordered leaf chains, and crashes.

**Root Cause**

split_leaf computes mid = entries.size()/2 with no byte-weighted split point and no post-check that each half fits a page. insert_into_leaf_no_split, which materializes the right half, has no capacity math at all (same missing check as CKV-001, different trigger).

**Trigger**

Fill a leaf region with ~130-270 small keys, then insert a 3,100-3,900-byte key that sorts into it. The large key alone inserts cleanly into an empty tree (page-legal), so this is independent of the oversized-key gate that would stop CKV-001.

**Incorrect Result**

PoC (poc1 mode oob4, ASan): after 15,000 small keys plus one 3,900-byte key, thousands of keys return get() == false and the run ends in SEGV inside verify_leaf_chain (chronokv.hpp:10370) - the wrapped write clobbered ~15 live pool pages, leaving wild next_leaf_id values. Earlier stages show in-page corruption: LeafSlot arrays overwritten with key bytes while no ASan trip occurs yet.

**Expected Result**

Either the split is byte-aware (each half provably fits a page) or oversized entries are rejected before reaching the split; the tree must never write outside a page.

**Violated Invariant**

Page bounding box (as CKV-001); leaf slot count and free-space accounting must stay consistent.

**Why Existing Protection Fails**

The space check exists only on the non-split insert path. Nothing validates that the halves a split produces fit their pages, and nothing detects the corruption afterwards (the in-suite verifiers are test-only and the engine keeps serving).

**Existing Test Coverage**

The btree fuzz uses ~7-byte keys; the depth test uses uniform short keys. No test mixes entry sizes at page-boundary scale, so a count-based split never overflows in CI.

**Reproducer**

```
poc/poc1_standalone.cpp mode oob4 (ASan): 15,000 small keys then one 3,900-byte key -> thousands of lost keys + SEGV in verify_leaf_chain. Mode overlap shows the in-page slot-corruption variant.
```

**Fix Direction**

Make the split point byte-aware (scan slots accumulating encoded sizes until the half-page budget is reached) and reject/overflow-page entries that cannot fit any half; combined with the CKV-001 fit checks this closes the class.

**Regression Test**

Fuzz test with mixed key sizes drawn from [1, 4000] bytes at page-boundary distributions; after every insert assert verify_leaf_chain, verify_separator_invariants, verify_chain_completeness, and that every previously inserted key remains reachable via get(). Run under ASan.

---

### [CKV-003] Transient bad_alloc mid-split corrupts the tree, the engine does not fail-stop, and the next checkpoint silently destroys all data

- **Severity:** Critical
- **Confidence:** Confirmed
- **Component:** B+ tree + engine exception safety + checkpoint
- **Files:** chronokv.hpp
- **Functions:** PagePool::alloc, BTree::put/split path, ChronoKV::commit_txn (ensure_index), ChronoKV::checkpoint_locked, WalSegments::rotate_after_checkpoint
- **Approximate lines:** 9609-9610 (throw); 11812-11841 (ensure_index); 6156-6340 (checkpoint_locked, full scan at 6224); 6337 (rotation)
- **Risk:** Impact 5 x Likelihood 2 x Detectability 5 = **50**

**Summary**

std::bad_alloc from PagePool::alloc escapes through the mid-mutation split path (the B+ tree has no exception safety: pages are modified in place before all allocations succeed). The tree is left structurally inconsistent - point lookups still work while cursor-based range scans return wrong results. The engine has no fail-stop latch for index-side failures (the WAL fail-stop D3 machinery covers only WAL errors), so it keeps accepting successful writes on the corrupted tree. Because checkpoint_locked snapshots state through the same corrupted cursor scan, the next checkpoint writes a structurally valid base containing the WRONG (empty) view and then rotates the WAL away, destroying the last durable copy of the real data. One transient OOM thus converts into permanent, silent, total data loss.

**Root Cause**

Three compounding defects: (a) the B+ tree mutation path is not exception-safe - allocations happen between in-place page mutations, so a throwing alloc leaves half-linked pages; (b) commit_txn's burn-on-throw protects the publication prefix only - nothing latches the engine into a failed state when the index itself breaks; (c) checkpoint_locked trusts the tree scan it walks (tree_scan("", 255 x 0xFF)) with no consistency cross-check against, e.g., the version store or the WAL, and rotation then deletes the superseded WAL segments.

**Trigger**

Any allocation failure while the page pool is exhausted (or under memory pressure with the default 256 MiB pool) during a split: fill the pool, keep committing. Reached deterministically in the PoC after 17,047 commits against a 1 MiB pool.

**Incorrect Result**

PoC sequence (audit/probe13/14): (1) 17,047 successful commits of key "w"; (2) bad_alloc escapes commit; (3) db.get("w") still returns "17046" but db.range_scan(...) returns 0 keys - the tree is corrupted; (4) db.put("after-crash", ...) returns Status::OK - no fail-stop; (5) checkpoint() SUCCEEDS on the corrupted tree; (6) after close and reopen, get("w") is PERMANENTLY LOST and the scan is empty. The acknowledged, durable writes of 17,047 commits are gone.

**Expected Result**

A basic exception guarantee at minimum: the tree remains structurally valid (or the engine refuses further use), the failed commit returns an error, and a later checkpoint cannot persist a view that contradicts the durable WAL without at least failing loudly.

**Violated Invariant**

Checkpoint completeness (the checkpoint base must contain every live key visible at its cts); engine fail-stop on unrecoverable internal corruption; basic exception safety of the index.

**Why Existing Protection Fails**

D3 fail-stop latches only WalSegments::failed_. The publication tracker burn keeps the prefix moving. checkpoint_locked has no cross-validation of the scan against dirty_since_ckpt_ or the WAL, so a truncated scan produces a well-formed, CRC-valid, silently wrong base. rotate_after_checkpoint then unlinks the covered segments - by design, and now destructively.

**Existing Test Coverage**

No test injects allocation failure into the tree path; the fault-injection kinds cover I/O operations (open/write/fsync/rename) but not bad_alloc. The crash-fuzz matrix kills the process at I/O boundaries, not at pool exhaustion. Nothing asserts a checkpoint equals the WAL-replayed state.

**Reproducer**

```
audit/probe14_oom_ckpt_loss.cpp: 1 MiB pool, Sync durability; loop { txn reads an absent key, puts "w" } until bad_alloc; then checkpoint(); close; reopen -> get("w") == absent (PERMANENTLY LOST). probe13_postexception.cpp demonstrates the intermediate corrupted state (get works, scan empty, further puts return OK).
```

**Fix Direction**

(1) Make BTree mutations exception-safe: pre-allocate all pages a split will need before modifying anything (or journal-and-undo); (2) add an engine-level fail-stop latch (like the WAL's failed_) that commits set when the index throws, refusing further writes/checkpoints; (3) cross-validate checkpoints (e.g., assert the scanned key count is consistent with the dirty-map/version store, or refuse to rotate the WAL when the checkpoint skipped keys that the WAL still covers).

**Regression Test**

Inject bad_alloc at PagePool::alloc with a charge budget (fault:: infrastructure) at every split step; after each injection assert: every acknowledged key is still readable via get AND range_scan, verify_leaf_chain/verify_separator_invariants hold, and if the engine latched failed, checkpoint() throws instead of writing a base. Also: checkpoint-vs-WAL differential assert - reopen after checkpoint and compare the recovered state to a WAL-only replay.

---

## 3. High Findings

### [CKV-004] Async-mode write-stage failure resurrects transactions that reported WalFailure - documented invariant D2 violated

- **Severity:** High
- **Confidence:** Confirmed
- **Component:** WAL group commit (leader rollback policy)
- **Files:** chronokv.hpp
- **Functions:** WalSegments::group_append (rollback section), commit_txn (caller handling)
- **Approximate lines:** 3991-4001 (rollback gated on batch->has_async); 3747-3748 (async waiter predicate); 3932-3934 (written publish); 6067-6071 (caller burns cts, returns WalFailure)
- **Risk:** Impact 4 x Likelihood 2 x Detectability 5 = **40**

**Summary**

When a group-commit batch fails, the rollback truncation is skipped for any batch containing async-durability records, on the theory that async committers already received Committed. That theory only holds for fsync-stage failures, where batch->written was published and async waiters were indeed released with success. On a WRITE-stage failure (pwrite/io_uring write fails partway), written is never set, so every waiter of the batch - async ones included - exits with WalFailure and burns its cts. The partially or fully written batch nevertheless remains in the WAL file, and recovery replays it: transactions whose callers observed WalFailure silently resurrect after reopen. This directly violates invariant D2 ("a batch for which any caller observed WalFailure is absent from the WAL after any crash").

**Root Cause**

The rollback gate tests batch->has_async, which distinguishes durability CLASS, not failure STAGE. The correct predicate is whether written was already published (fsync-stage failure: async callers acknowledged; keep the no-truncate contract) versus not (write-stage failure: nobody acknowledged; truncate like the sync path).

**Trigger**

Async durability mode + any mid-batch write failure (ENOSPC/EFBIG/EIO) with at least two async committers in the batch. No crash and no power loss required - the divergence is deterministic on reopen.

**Concurrent Schedule**

```
T1..T8 (Async mode) commit concurrently; group-commit batches them; the leader's write fails partway (RLIMIT_FSIZE). All waiters see batch->failed, return WalFailure, burn cts. The written prefix of the batch stays in wal_000001.log. Next open: recover_all replays the CRC-valid prefix frames; keys whose callers observed WalFailure are present.
```

**Incorrect Result**

PoC (poc/poc4_async_resurrect.cpp): 8 async-committing threads under a file-size limit -> WalFailure=8 with 1-3 keys present after reopen ("RESURRECTED (caller saw WalFailure, key present after reopen)"). Sync-mode control under identical conditions: 0 resurrected - proving the asymmetry is exactly the has_async gate. The async_committed_then_lost counter is also mis-debited for records that were never acknowledged.

**Expected Result**

D2: after any failure, the WAL contains no batch for which any caller observed WalFailure. Async waiters of a write-stage-failed batch should either be truncated away (like sync) or the contract documented to exclude write-stage failures - it currently claims the former.

**Violated Invariant**

D2 (WAL rollback completeness); Visible = Durable-or-rejected for every acknowledged outcome.

**Why Existing Protection Fails**

The rollback truncation that enforces D2 is precisely the code the has_async gate skips. Recovery replays every CRC-valid frame; the cts-contiguity check passes because the batch prefix's cts are contiguous; the publication-tracker burn is in-memory only.

**Existing Test Coverage**

The D2 tests (D2a/D2b) exercise fsync-stage failures with Sync/Group committers only. No test drives Async durability into a WRITE-stage failure; the write path itself has no fault hooks at all (see CKV-018), so this divergence is unreachable in CI.

**Reproducer**

```
poc/poc4_async_resurrect.cpp - 8 threads, DurabilityMode::Async, RLIMIT_FSIZE-sized WAL, reopen and diff caller outcomes vs recovered state. Prints resurrected key list; sync control prints 0.
```

**Fix Direction**

Gate the no-truncate branch on batch->written (already-published async acks) rather than has_async: if !written, truncate the failed batch exactly like the sync path (nobody could have acknowledged it), and only debit async_committed_then_lost for records whose waiters actually returned Committed.

**Regression Test**

Extend D2a: mixed Async committers + injected WriteFail/short pwrite (requires pwrite_all fault hooks) -> every key whose put returned WalFailure must be absent after reopen; every key present after reopen must have returned Committed/OK. Run the assertion for both fsync-stage and write-stage failures.

---

### [CKV-005] All engine full-scans use hi = 255 x 0xFF as an upper sentinel - keys above the sentinel are invisible to GC, checkpoint, and teardown

- **Severity:** High
- **Confidence:** Confirmed
- **Component:** Engine scans (gc_once, free_all, verify_full, verify_version_chains, checkpoint full scan)
- **Files:** chronokv.hpp
- **Functions:** gc_once, free_all, verify_full, verify_version_chains, checkpoint_locked
- **Approximate lines:** 5053, 5164, 5220, 5601, 6224 (the five tree_scan("", std::string(255, '\xFF')) sites)
- **Risk:** Impact 5 x Likelihood 2 x Detectability 5 = **50**

**Summary**

Every full-keyspace walk in the engine bounds the scan with the literal string of 255 0xFF bytes. Any key whose first 255 bytes are 0xFF and whose length is at least 256 compares GREATER than the sentinel and is therefore skipped by every full scan. A checkpoint's base omits such keys while rotate_after_checkpoint deletes the WAL segments covering their commits, so after reopen the key is permanently gone even though every neighbor survives. GC never visits the key (its versions are never anchored or retired - unbounded chain growth), and free_all leaks its KeyEntry (confirmed by LeakSanitizer).

**Root Cause**

The sentinel is not a true upper bound for a keyspace whose keys can be up to 65,535 bytes (MAX_KEY_BYTES). The correct bound is an unbounded scan (hi = the 65,535-byte 0xFF string) or a sentinel-free full-walk API.

**Trigger**

db.put(std::string(256, '\xFF'), "v"); db.checkpoint(); db.close(); reopen -> the key is absent. The key is legal (length 256 <= 65535) and round-trips correctly through the WAL.

**Incorrect Result**

PoC (poc2 mode ckpt): put aaa, a 256-byte 0xFF key, zzz; checkpoint; reopen -> aaa=1, zzz=3, lostkey=ABSENT - "DATA LOSS CONFIRMED - 256-byte 0xFF key vanished after checkpoint+reopen". LeakSanitizer in the same run reports the leaked 56-byte KeyEntry from free_all.

**Expected Result**

The key survives checkpoint+reopen exactly like any other key; GC reclaims its old versions; free_all frees it.

**Violated Invariant**

Checkpoint completeness; GC reachability of every live key; teardown completeness.

**Why Existing Protection Fails**

The tree's range_scan correctly implements [lo, hi] inclusive - the bug is the engine's choice of hi. No layer cross-checks that the number of keys checkpointed equals the number of live entries.

**Existing Test Coverage**

No test uses keys with 0xFF prefixes or lengths >= 256 anywhere in the suite, so the sentinel miss cannot be observed.

**Reproducer**

```
poc/poc2_engine.cpp mode ckpt (public API, WAL + checkpoint, reopen assertion).
```

**Fix Direction**

Replace the 255-byte sentinel with a true upper bound (std::string(65535, '\xFF')) or add an explicit scan_all() that walks the leaf chain without a hi bound; add a checkpoint assertion that scanned live-key count matches the tree's size().

**Regression Test**

Put keys: normal, 255 x 0xFF, 256 x 0xFF, 300 x 0xFF + suffix, then checkpoint+reopen and assert all present; run gc passes and assert retired counters move for those keys; assert no leaks under LSan at close.

---

### [CKV-006] RangeScanStream stops at the first tombstone - every later key in the range is silently dropped

- **Severity:** High
- **Confidence:** Confirmed
- **Component:** Public API (RangeScanStream / cursor state)
- **Files:** chronokv.hpp
- **Functions:** ChronoKVRangeScanCursorState::advance, stream_has_next (read_at_idx returning nullopt for deletes)
- **Approximate lines:** 11908-11918 (advance caches one entry); 11927-11929 (has_next treats cached_val == nullopt as end-of-stream)
- **Risk:** Impact 4 x Likelihood 4 x Detectability 4 = **64**

**Summary**

The streaming cursor caches exactly one (key, value) pair. read_at_idx returns nullopt for a key that is deleted at the read timestamp (a tombstone) or otherwise invisible. has_next() returns cached_valid && cached_val.has_value(), so a tombstone is interpreted as end-of-stream: iteration stops and every subsequent key in [lo, hi] is silently omitted. Because tree_->erase is never called (no page reclamation), tombstones accumulate forever in the tree, so any range containing a deleted key truncates the stream at that key. The vector-based range_scan handles the same case correctly by skipping invisible keys and continuing.

**Root Cause**

advance() leaves the cached entry in a (key present, value nullopt) state when the key is deleted at the read ts, instead of skipping to the next visible entry. has_next() conflates 'no value' with 'no more keys'.

**Trigger**

put k01..k05; erase k03; open RangeScanStream("k01", "k05") -> the stream yields k01, k02 and stops. The vector range_scan over the same range yields 4 keys.

**Incorrect Result**

PoC (poc2 mode stream): vector range_scan yields 4 keys (correct); RangeScanStream yields 2 keys, last = k02 - "TRUNCATION CONFIRMED - stream stopped at the tombstone; later keys dropped".

**Expected Result**

The stream yields exactly the visible keys in [lo, hi] at the scan snapshot: k01, k02, k04, k05.

**Violated Invariant**

Scan completeness: a stream over [lo,hi] returns every key visible at its snapshot (README: per-page snapshot consistency).

**Why Existing Protection Fails**

No layer below the stream reorders or filters tombstones; the cursor state machine itself has the bug. The existing stream test (main.cpp Test 14) uses only live keys, so it cannot fail.

**Existing Test Coverage**

RangeScanStream tests never include a tombstone in the scanned range.

**Reproducer**

```
poc/poc2_engine.cpp mode stream (hooks-off public API).
```

**Fix Direction**

In advance(), loop: if the cached key has no value at the read ts, advance to the next entry instead of stopping; has_next() then only reflects cursor exhaustion, not value presence.

**Regression Test**

Differential test: random key sets with random erases; for each [lo,hi] assert RangeScanStream output == vector range_scan output == expected visible set at the snapshot; include ranges whose first key is deleted and ranges entirely of tombstones.

---

### [CKV-007] A throwing observer callback converts committed, durable writes into reported failures (and the async futures rethrow)

- **Severity:** High
- **Confidence:** Confirmed
- **Component:** Public API (observers / put / erase / Batch::commit / Transaction::commit / put_async / erase_async)
- **Files:** chronokv.hpp
- **Functions:** notify_observers and every call site; Database::put, Database::erase, Batch::commit, Transaction::commit, put_async, erase_async
- **Approximate lines:** 9185-9199 (no exception boundary around obs.callback); 8524 + 8540-8542 (put catch converts to thrown Error); 9040, 9471/9478-9479; 8863-8889, 8941-8966 (async legs)
- **Risk:** Impact 4 x Likelihood 2 x Detectability 3 = **24**

**Summary**

User observer callbacks run inline under observer_mu_ with no exception isolation. When a callback throws, the exception escapes into the commit machinery at different points on different paths: Database::put catches it and throws Error("put failed: ...") AFTER the commit is durable - the caller is told the write failed while it is committed and on disk; Transaction::commit() propagates the raw exception after the transaction already committed (the caller cannot learn the outcome); put_async/erase_async futures rethrow instead of returning Status, contradicting the documented async error contract. Additionally, callbacks registered before the throwing one fire while later ones and later write-set entries are silently skipped (partial notification), and a standard retry-on-failure caller double-writes.

**Root Cause**

The observer invocation has no try/catch boundary, and the commit paths fire observers at points where the commit is already irrevocable. The outcome-reporting contract (Status for recoverable conditions) is inverted for a user-callback exception.

**Trigger**

Register an observer whose callback throws; perform any matching put/erase/batch/txn commit/async write.

**Incorrect Result**

PoC (audit/probe8.cpp): observer throws; db.put("w:k", "durable") throws Error("put failed: cb fail"); close + reopen -> w:k == "durable" - "CONFIRMED: put() reported failure but write is DURABLE on disk".

**Expected Result**

Either the commit succeeds and the callback exception is delivered out-of-band (Status/exception distinct from the commit outcome), or the whole path is documented as callback-must-not-throw - with the current API contract ("recoverable conditions return Status") the observed behavior is a defect.

**Violated Invariant**

Outcome correctness: a reported failure must not correspond to a committed write (the client-side face of D2).

**Why Existing Protection Fails**

observer_mu_ protects the registry, not the callbacks' effects; no path isolates or defers callback exceptions; the async lambdas lack the catch-all that get_async has (8905-8927).

**Existing Test Coverage**

Observer tests cover firing/ordering/unregister and the reentrancy deadlock, but never a throwing callback; the async tests never inject exceptions.

**Reproducer**

```
audit/probe8.cpp (put leg); audit/api_probe.cpp sections B/C/D (txn and async legs).
```

**Fix Direction**

Wrap each callback invocation in try/catch inside notify_observers: record the first exception, complete the notification loop, then deliver the failure via a dedicated channel (e.g., an observer_error handler or a Status on the API path AFTER accurate commit reporting). At minimum, put/erase/batch/txn must report the true commit outcome and document callback exceptions separately; add the missing catch-all to put_async/erase_async.

**Regression Test**

For each write surface (put, erase, batch, transaction, put_async, erase_async) with a throwing observer: assert the reported outcome matches durability after reopen (reported failure => absent; reported OK => present), assert every observer either fired or the failure is surfaced, and assert the async futures yield Status::Failed rather than rethrowing.

---

## 4. Medium Findings

### [CKV-008] fence_unchanged misses count-preserving splits: the BTree OLC concurrency contract is broken (latent in-engine today)

- **Severity:** Medium (High for standalone BTree use)
- **Confidence:** Confirmed
- **Component:** B+ tree (optimistic lock coupling retry)
- **Files:** chronokv.hpp
- **Functions:** fence_unchanged; put_recursive / erase_recursive gap re-check
- **Approximate lines:** 11683-11693 (fence compares only key_count, min_key, max_key); 10958-10969 and 11767-11773 (gap + re-check)
- **Risk:** Impact 4 x Likelihood 1 x Detectability 5 = **20**

**Summary**

The optimistic-lock-coupling retry validates that a leaf did not change during the shared-to-exclusive latch gap by comparing (key_count, min_key, max_key) before and after. A split whose left half preserves all three fields passes the re-check - which happens whenever the leaf held a single entry (count 1 -> 1, min = max = the sole key) or was empty. The retrying writer then mutates a stale page snapshot: it inserts into the left leaf although the separator already routes keys at/above the split key to the new right sibling. Result: a key present in the tree but unreachable by descent, a disordered leaf chain, and separator-invariant violations. In the shipped engine this is masked because every tree mutation is serialized under nm_ (ensure_index) and tree_->erase / put_with_old have no callers - but the BTree's own documented contract ("the fence re-check below is the guard") is false, and any second mutation path exposes it.

**Root Cause**

The fence triple is a content hash of the leaf, not a version/epoch counter; it is not invariant under splits that keep the left half's boundary keys and count unchanged.

**Trigger**

Two concurrent BTree::put writers on a leaf holding one entry; writer A parks in the latch gap after capturing the fence, writer B splits (its new key does not fit), A acquires the exclusive latch, re-check passes, A inserts into the wrong (left) leaf.

**Concurrent Schedule**

```
T1: begin put(K3); descend; shared-latch leaf L {K1}; capture fence {count=1, min=K1, max=K1}; release shared.
T2: put(K2) does not fit; split L into L{K1} + R{K2}; install separator K2; L's fence is still {1, K1, K1}.
T1: acquire exclusive on L; fence_unchanged -> true; insert K3 into L (fits).
Result: L = {K1, K3}, parent routes >= K2 to R. get(K3) by descent -> not found; leaf chain out of order.
```

**Incorrect Result**

PoC (poc/poc3_race.cpp, built with -DCHRONOKV_STRESS using the repo's own dst scheduler at the code's own tree_leaf_latch_gap stress point, seed 12, reproduced 3/3 runs): put(K3) returns true, get(K3) returns false; verify_leaf_chain reports "leaf chain out of order"; verify_separator_invariants reports "leaf max key K3 >= parent's upper bound K2"; full scan order is a, c, b. The erase path has the same hole (returns not-found for a key that moved to the sibling).

**Expected Result**

The retry must detect the split and restart the descent; no insert may land in a page the separator no longer routes to.

**Violated Invariant**

OLC validation soundness: the re-check must reject any concurrent structural change it raced with.

**Why Existing Protection Fails**

The fence triple is the only validation, and it is not a version: two different tree states share it. The hazard pointers protect page lifetime, not logical staleness.

**Existing Test Coverage**

No test in any CI configuration runs two concurrent BTree mutators: the engine serializes mutations under nm_, the fuzz/depth tests are single-threaded, the concurrent-cursor test uses one writer, and the dst S4 scenario is one writer + two scanners. The retry path's logic is therefore never exercised concurrently - neither its bugs nor any TSan races on it can be observed.

**Reproducer**

```
poc/poc3_race.cpp (deterministic, seed 12, uses the repo's dst scheduler and its own stress point).
```

**Fix Direction**

Add a per-page mutation version counter (epoch) bumped on every structural change, and validate version equality in the retry; or hold the parent's latch across the gap so a split of the child cannot complete unnoticed.

**Regression Test**

A standalone BTree concurrency test with N >= 2 concurrent putters over overlapping key sets (the dst scheduler already has the tree_leaf_latch_gap point); after the run assert all keys reachable, chain order, and separator invariants. Also run under TSan once the path is actually concurrent.

---

### [CKV-009] Transactional reads of absent keys permanently materialize invisible empty entries - unbounded pool exhaustion on negative-lookup workloads

- **Severity:** Medium
- **Confidence:** Confirmed
- **Component:** Transaction engine (read-set handling in commit_txn)
- **Files:** chronokv.hpp
- **Functions:** commit_txn (rs handling), ensure_index, ReadWriteTransaction::read
- **Approximate lines:** 5945-5948 (ensure_index for every rs key); 11812-11841 (ensure_index creates KeyEntry + tree node); 5873 (ws.empty() early-return masks read-only txns)
- **Risk:** Impact 3 x Likelihood 4 x Detectability 5 = **60**

**Summary**

commit_txn calls ensure_index(k) for every key in the transaction's read set. ensure_index CREATES a KeyEntry and inserts it into the B+ tree when the key is absent. A transaction that reads an absent key (a negative lookup) and also writes anything therefore leaves behind a permanent, invisible empty entry: no version chain, invisible to every range scan and to get(), never reclaimed by GC (empty chains produce nothing to retire), never erased (tree_->erase has no callers). Read-only transactions are masked by the ws.empty() early return, but any mixed workload - e.g., an existence-check + conditional-insert pattern, or cache-miss probing - grows the index without bound. 17,047 such transactions exhausted a 1 MiB page pool in the PoC while the database contained exactly ONE visible key; the terminal symptom is std::bad_alloc (and, combined with CKV-003, worse).

**Root Cause**

Read-set keys are materialized through the same ensure_index path as write-set keys in order to lock and validate their KeyEntry::commit_mu. For keys that never existed, this creates permanent state whose only purpose is one commit's validation.

**Trigger**

Loop { txn.get("absent-" + i); txn.put("w", v); txn.commit(); } - after ~17k iterations against a 1 MiB pool (default 256 MiB scales linearly: ~4.3M entries) the pool throws std::bad_alloc with one visible key.

**Incorrect Result**

PoC (audit/probe12_neglook_write.cpp): committed=17047 failed=0 exception=std::bad_alloc; visible keys in db: 0 scans show nothing; the 17k empty entries are invisible to users and to range_scan.

**Expected Result**

Negative lookups must not permanently consume index space; empty entries created for validation must be removable.

**Violated Invariant**

Index size proportional to live keyspace, not to the history of read keys.

**Why Existing Protection Fails**

GC's sweep only processes version chains (an empty chain yields nothing); the B+ tree has no delete path wired; nothing distinguishes an entry created for read-validation from one holding data.

**Existing Test Coverage**

No test measures index growth against absent-key reads; the GC 'memory boundedness' test asserts only that latest values read back (vacuous for growth - see section 15).

**Reproducer**

```
audit/probe12_neglook_write.cpp (pool exhaustion) and probe10/probe11 (entry accounting).
```

**Fix Direction**

Mark entries created solely for read-set validation (e.g., an empty-chain bit or a refcount) and reclaim them at commit/abort when still empty; or validate absent-key reads via the phantom tracker (a point read of key k is the range [k, k]) instead of materializing a KeyEntry.

**Regression Test**

Loop N transactional negative lookups + one put; assert pool.allocated() stays bounded (does not grow with N beyond a small constant) and page_pool_bytes is not exhausted; assert range_scan output unchanged; assert no KeyEntry leak under LSan at close.

---

### [CKV-010] restore_pitr into a non-empty destination silently merges stale foreign state (or fails with a misleading corruption error)

- **Severity:** Medium
- **Confidence:** Confirmed
- **Component:** PITR (Database::restore_pitr)
- **Files:** chronokv.hpp
- **Functions:** Database::restore_pitr
- **Approximate lines:** 8682-8725 (create_directories at 8702 with no emptiness check; final open at 8719-8724)
- **Risk:** Impact 3 x Likelihood 2 x Detectability 4 = **24**

**Summary**

restore_pitr(src_wal, src_ckpt, dest_dir, as_of) documents dest_dir as a fresh directory but never checks or cleans it. If dest_dir holds a previous restore's artifacts, the new export lands on top: stale WAL records from the earlier restore replay over the fresh checkpoint export whenever they are cts-contiguous with it, silently corrupting the as-of state (writes made to the PREVIOUS restore appear in the NEW one); when the stale records are not contiguous, open throws a misleading CorruptionError ("interior gap ... WAL corruption detected") although nothing is corrupt - the destination was dirty.

**Root Cause**

The final Database::open points at dest_dir/wal which still contains the prior restore's WAL; recovery's contiguity validation cannot distinguish a dirty destination from real corruption.

**Trigger**

(a) restore at as_of W into dest, write to the restored DB, close; restore again at the same W into the same dest -> the foreign 'post' key from the first restore is present in the new as-of view. (b) restore at a lower as_of into the same dirty dest -> CorruptionError with an interior-gap diagnosis.

**Incorrect Result**

PoCs: audit/probe6.cpp (H3e) - "STALE foreign write from previous restore leaks into as-of state"; audit/probe5.cpp - second restore at lower as_of terminates with CorruptionError ("recovery failed: interior gap expected_ts=3 found_ts=4 - WAL corruption detected").

**Expected Result**

restore_pitr either refuses a non-empty destination loudly or atomically replaces its contents; the as-of view must contain only the source's state at as_of.

**Violated Invariant**

restore_pitr contract: dest holds the source's state as of as_of_cts (chronokv.hpp 8661-8663).

**Why Existing Protection Fails**

Nothing validates the destination; recovery's gap detection catches only the non-contiguous case, and its error message names WAL corruption rather than a dirty destination.

**Existing Test Coverage**

PITR tests always pass a fresh dest_dir; no test restores into a used destination.

**Reproducer**

```
audit/probe5.cpp (misleading error) and audit/probe6.cpp (silent wrong state).
```

**Fix Direction**

At entry, require dest_dir to be empty or absent (throw with the remedy), or write into a temp sibling directory and atomically swap; alternatively clear dest's wal/ and ckpt before exporting.

**Regression Test**

restore twice into the same dest at the same and at different as_of boundaries; assert the second restore either throws a destination-not-empty error or produces exactly the source's as-of state (diff against a fresh-dest restore).

---

### [CKV-011] A PITR open mutates the source directory - the README's "does not modify the source directory at all" is false twice

- **Severity:** Medium
- **Confidence:** Confirmed
- **Component:** PITR / recovery (read-only promise)
- **Files:** chronokv.hpp
- **Functions:** recover_with_checkpoint (orphaned .tmp cleanup), WalSegments::WalSegments / open_segment (torn-tail repair)
- **Approximate lines:** 6712-6742 (unconditional orphan .tmp deletion - not gated on as_of_cts, unlike the stale-delta deletion at 6953); 3446-3453 (ftruncate of the active segment's torn tail)
- **Risk:** Impact 2 x Likelihood 4 x Detectability 4 = **32**

**Summary**

README section "PITR opens are read-only" states the open "does not modify the source directory at all (not even stale-delta cleanup)". In reality a PITR open (1) deletes orphaned checkpoint .tmp files it finds in the source directory (the cleanup loop at 6712-6742 runs regardless of the as-of boundary), and (2) truncates a torn tail on the source's active WAL segment during WalSegments construction. Both mutations are benign for correctness of the as-of view, but they break the documented non-destructive promise - which matters because PITR's documented purpose includes forensic inspection of a damaged directory.

**Root Cause**

The .tmp cleanup was written for the normal-open path and never gated on pitr_as_of_; the torn-tail repair is unconditional in the WalSegments constructor.

**Trigger**

Plant ckpt.tmp / ckpt.delta.7.tmp files and append 9 garbage bytes to the active WAL segment of a PITR source directory; open with pitr_as_of_cts; all three mutations occur.

**Incorrect Result**

PoC (audit/api_probe2.cpp): after a PITR open, ckpt.tmp is gone, ckpt.delta.7.tmp is gone, and the active segment shrank from 83 to 74 bytes ("README violated" x3).

**Expected Result**

A PITR open leaves the source directory byte-identical.

**Violated Invariant**

PITR read-only source contract (README + the in-code comment at 8666-8669, which itself only acknowledges the torn-tail repair).

**Why Existing Protection Fails**

The stale-delta deletion was correctly gated on as_of (6953); the .tmp cleanup and torn-tail repair were simply never audited against the same promise.

**Existing Test Coverage**

PITR tests assert future deltas/records are untouched, but never that .tmp files or torn tails survive.

**Reproducer**

```
audit/api_probe2.cpp sections E1/E2/F1.
```

**Fix Direction**

Gate the .tmp cleanup on !pitr_as_of_ and defer torn-tail repair on PITR opens (parse without truncating, or copy-aside); update the README either way.

**Regression Test**

Hash every file in a source directory before and after a PITR open (including planted .tmp files and a torn tail); assert the directory tree and byte contents are identical.

---

### [CKV-012] group_append contains no burn-on-throw: an exception before the leader state flip orphans the cts and wedges every later commit at the publication barrier

- **Severity:** Medium
- **Confidence:** Confirmed mechanism / Plausible trigger (OOM-class only today)
- **Component:** WAL group commit + publication barrier
- **Files:** chronokv.hpp
- **Functions:** WalSegments::group_append, commit_txn (comment claims the burn), PublicationTracker::await_published
- **Approximate lines:** 6009-6017 (the comment asserting group_append burns - no burn exists; pub_.burn appears only in commit_txn at 6055/6062/6068/6102); 3679-3743 (reservation window before the leader try/catch); 4495-4498 + 6120 (the barrier)
- **Risk:** Impact 4 x Likelihood 1 x Detectability 5 = **20**

**Summary**

commit_txn's error handling relies on a documented contract: "group_append's internal catch guarantees any ts it reserved was already burned." That catch exists only around the leader's unlocked I/O section. An exception thrown between the cts reservation (clock.fetch_add under batch_mu_, line 3679) and the leader election - e.g. wal_ser's throw (closed by the API pre-checks), records.push_back / make_shared<Batch> bad_alloc, or an on_reserve re-throw - escapes with the cts reserved but neither completed nor burned. The published prefix then stalls below that cts forever, and because the v27 strict-serializability barrier (await_published) blocks every later commit until the prefix covers its own cts, every subsequent commit hangs - a permanent, silent engine wedge while holding its KeyEntry commit mutexes.

**Root Cause**

The burn-on-throw contract is documented at the call site but never implemented for the pre-leader window of group_append.

**Trigger**

Transient std::bad_alloc during records.push_back or Batch allocation under memory pressure (the size-throw path is closed by commit_txn's pre-check formulas at 5884-5921, which replicate wal_ser's limits exactly).

**Concurrent Schedule**

```
T1: group_append; cts=N reserved; records.push_back throws bad_alloc; exception propagates (no burn).
T2..Tm: later commits reserve cts>N, reach await_published(cts), block forever (prefix stuck at N-1).
All committers hold their commit_mu keys; database is wedged with no error surfaced.
```

**Incorrect Result**

Not executable without OOM injection (no hook exists); mechanism verified by code reading: grep confirms pub_.burn sites only in commit_txn, and the pre-leader window has no try/catch. With the barrier, the historical failure mode changes from 'silently stale prefix' to 'every later commit hangs'.

**Expected Result**

Any exception escaping group_append after reservation must burn the reserved cts (or complete it) before propagating.

**Violated Invariant**

Publication-prefix progress: every reserved cts eventually completes or burns (the precondition of the v27 M1 barrier).

**Why Existing Protection Fails**

The leader-section catch handles only exceptions after election; the reservation-to-election window is unprotected, and the call-site comment that claims otherwise prevents the gap from being noticed.

**Existing Test Coverage**

No test injects exceptions between reservation and leader election; leader_post_unlock_hook_ exists for the C1 class but nothing hooks the earlier window.

**Reproducer**

```
Code-path demonstration only: the window is lines 3679-3750 minus the try at 3813. A fault hook at records.push_back (or an OOM limit) with a single committer reproduces the wedge.
```

**Fix Direction**

Wrap the reservation-to-enqueue section of group_append in try/catch that calls a new burn path (pub_ must be reachable - e.g., pass a burn callback alongside on_reserve) and rethrows; correct the stale comment at 6009-6017.

**Regression Test**

Inject an exception at the enqueue step (new fault kind or hook) under a timeout: the committing thread must receive WalFailure, and a subsequent commit on another thread must complete (prefix advanced past the burned hole).

---

## 5. Low Findings

### [CKV-013] io_uring registered-files feature is dead code, and its fd-number staleness check is a data-destruction trap if ever enabled

- **Severity:** Low (latent Critical if enabled)
- **Confidence:** Confirmed
- **Component:** io_uring wrapper
- **Files:** chronokv.hpp
- **Functions:** IoUring constructor ladder, ensure_file_registered, submit_write / submit_fsync
- **Approximate lines:** 2663 (fixed_files_ = false); 2485-2487, 2517-2518 (only ever set false - no activation site); 2865-2874 (ensure_file_registered compares fd NUMBERS); describe() prints fixed_files=lazy
- **Risk:** Impact 2 x Likelihood 1 x Detectability 5 = **10**

**Summary**

fixed_files_ is initialized false and never set true anywhere in the header; ensure_file_registered is only called under if (fixed_files_ && ...), so the registered-files feature advertised by the constructor comment and describe() ("fixed_files=lazy") never activates - every SQE uses a plain fd (benign today: no perf win, misleading diagnostics). The latent trap: ensure_file_registered decides "already registered" by comparing the fd NUMBER. Segment rotation closes the old fd and opens the new segment, and the kernel typically reuses the number (verified: closed fd 4, new segment got fd 4). Registered files pin the old struct file, so enabling the feature as designed would write every post-rotation batch into the sealed OLD segment at the new segment's offsets - with full-success CQEs - destroying the WAL. A one-line "fix" of the dead flag activates this.

**Root Cause**

Feature never wired to an activation site; identity check by fd number instead of file identity (inode/dev).

**Trigger**

Dead today. Enabling fixed_files_ (one line) + one segment rotation.

**Incorrect Result**

PoC premise verified live (poc/poc5a_iouring_stale_fd.cpp): fd 4 closed and immediately reused by the new segment; ring feature summary shows fixed_files=lazy; the stale-slot write target demonstrated on two files.

**Expected Result**

Either remove the feature and its diagnostics, or track registered-file identity (unregister on rotation / compare inode) before enabling.

**Violated Invariant**

SQE targets must reference the intended file across fd reuse.

**Why Existing Protection Fails**

The guard never runs (dead flag); when it would run, fd number is not file identity.

**Existing Test Coverage**

io_uring tests exercise real-kernel writes but the registered-files path is unreachable in every configuration.

**Reproducer**

```
poc/poc5a_iouring_stale_fd.cpp (fd-reuse premise + dead-flag proof).
```

**Fix Direction**

Delete the dead path or implement identity tracking; correct describe()'s output.

**Regression Test**

If enabled: rotate a segment under io_uring and assert batches land in the NEW segment (read back and CRC-verify both files).

---

### [CKV-014] Batch accepts duplicate keys and is silently consumed on failed commit; inconsistent with Transaction's last-wins semantics

- **Severity:** Low
- **Confidence:** Confirmed
- **Component:** Public API (Batch)
- **Files:** chronokv.hpp
- **Functions:** Batch::put / Batch::commit
- **Approximate lines:** 8980-8985 (append without dedup); 9008 (entries_.clear() on every non-throw return, including Conflict/TooLarge/WalFailure/InvalidTransaction)
- **Risk:** Impact 2 x Likelihood 3 x Detectability 2 = **12**

**Summary**

Batch::put appends entries without deduplication; the engine's duplicate-key rejection (commit_txn 5928-5934, InvalidTransaction) then rejects the whole commit, and entries_.clear() wipes the batch on every non-throwing return path - so a batch that hit Conflict/TooLarge/WalFailure cannot be retried either. ReadWriteTransaction::write dedups via std::map with last-wins; two public write surfaces thus have different duplicate-key semantics, and neither is documented. A batch that fails via an exception, by contrast, is NOT cleared - inconsistent retry semantics across failure modes.

**Root Cause**

Batch::commit's cleanup runs unconditionally on the Status-return paths; no dedup at insert time.

**Trigger**

batch.put("k","v1"); batch.put("k","v2"); batch.commit() -> Status::InvalidTransaction, batch.size()==0, nothing committed.

**Incorrect Result**

Runtime-verified (audit probe): InvalidTransaction + empty batch after commit; same for injected WalFailure.

**Expected Result**

Documented, consistent semantics: either dedup last-wins (match Transaction) or reject at put() time; failed batches remain retryable or are documented as consumed.

**Violated Invariant**

API consistency between the two write surfaces.

**Why Existing Protection Fails**

No layer above the engine dedups; the engine rejects; the batch destroys itself.

**Existing Test Coverage**

Engine-level dup rejection is tested (main.cpp 5106-5117); the public Batch behavior is not.

**Reproducer**

```
audit/api_probe.cpp section A.
```

**Fix Direction**

Dedup in Batch::put (map, last-wins) or reject duplicates at put(); clear entries_ only on success; document.

**Regression Test**

Batch with duplicate keys: assert commit() == OK and final value == last; assert a Conflict/WalFailure commit leaves the batch intact for retry (or documents consumption).

---

### [CKV-015] Abandoned Transaction after close() leaks its reader slot and phantom registration on a live engine

- **Severity:** Low
- **Confidence:** High-confidence
- **Component:** Transaction lifecycle / GC pinning
- **Files:** chronokv.hpp
- **Functions:** ~ReadWriteTransaction, Transaction keepalive members
- **Approximate lines:** 7924-7929 (cleanup skipped when engine_live() is false); 9231 (engine_keepalive_ declaration)
- **Risk:** Impact 2 x Likelihood 2 x Detectability 3 = **12**

**Summary**

~ReadWriteTransaction skips release_slot / deregister_phantom_reader when engine_live() returns false. In the single-keepalive case member destruction order guarantees the engine is already gone, so the skip is right. But when ANOTHER keepalive holder exists (a second transaction, a stream, an in-flight async op), the engine outlives the abandoned transaction, engine_live() is true - the leak does not fire - EXCEPT the inverse case: a transaction abandoned while the Database was closed but the engine kept alive by other handles leaves its slot and phantom registration pinned on a LIVE engine when those conditions combine, pinning the GC retirement floor (deferred version reclamation and memory growth) until teardown. The window is narrow (requires close() + abandonment + another keepalive), and the mechanism is certain from member-declaration-order analysis.

**Root Cause**

The liveness check treats 'Database closed' as 'engine dead', which the v25.8 keepalive design decoupled.

**Trigger**

close() with a live transaction + at least one other engine keepalive; destroy the transaction afterwards.

**Incorrect Result**

Code-read: pin leak mechanism verified; no runtime repro attempted (narrow window).

**Expected Result**

Slot and phantom registration are released whenever the ENGINE is alive, regardless of Database closure.

**Violated Invariant**

Reader slots and phantom registrations are always paired with acquisition.

**Why Existing Protection Fails**

engine_live() consults the Database liveness flag rather than the engine keepalive it holds.

**Existing Test Coverage**

Lifecycle tests cover close-races and handle outliving, not slot bookkeeping after abandonment.

**Reproducer**

```
Deterministic repro is possible with two transactions + close(); not built for this audit.
```

**Fix Direction**

Release the slot whenever engine_keepalive_ (the member) is non-null at destruction, not based on the Database flag.

**Regression Test**

open db; t1 = begin; t2 = begin; close(); destroy t1 (abort first is impossible - it aborts by contract? no: abandonment after close must not abort); assert gc_stats().oldest_active_pin_epoch advances and retired_pending drains after t2 dies.

---

### [CKV-016] put_async / erase_async futures rethrow worker exceptions instead of returning Status - the documented async error contract

- **Severity:** Low
- **Confidence:** Confirmed
- **Component:** Public API (async)
- **Files:** chronokv.hpp
- **Functions:** put_async lambda, erase_async lambda (get_async has the boundary)
- **Approximate lines:** 8863-8889 (put_async, no try/catch); 8941-8966 (erase_async, no try/catch); contrast 8905-8927 (get_async wraps catch(...) -> Status::Failed)
- **Risk:** Impact 3 x Likelihood 2 x Detectability 3 = **18**

**Summary**

get_async wraps its whole body in catch (...) -> Status::Failed; put_async and erase_async do not. Any exception escaping the async write path (a throwing observer callback - proven live; bad_alloc building the WriteSet) is stored in the future and rethrown by .get(). README: "Async ... errors via Result<T> / Status". This is the async leg of CKV-007, reported separately because it is an independent missing boundary with its own contract mismatch.

**Root Cause**

Missing catch-all in two of the three async lambdas.

**Trigger**

Throwing observer + put_async(...).get() -> rethrows the callback's exception.

**Incorrect Result**

Runtime-verified in audit/api_probe.cpp section D: put_async().get() rethrows.

**Expected Result**

Future yields Status::Failed (or the documented async error surface), not an exception.

**Violated Invariant**

Async error contract (README API overview).

**Why Existing Protection Fails**

No boundary exists on these two paths.

**Existing Test Coverage**

Async tests never inject exceptions.

**Reproducer**

```
audit/api_probe.cpp section D.
```

**Fix Direction**

Add the same catch(...) -> Status::Failed wrapper used by get_async.

**Regression Test**

Throwing observer + put_async/erase_async: assert .get() returns Status::Failed and the commit's durability matches the reported outcome.

---

### [CKV-017] Cross-segment LSN contiguity is not validated on recovery; README's "strict LSN gap/duplicate detection" overstates

- **Severity:** Low
- **Confidence:** Confirmed
- **Component:** WAL recovery / docs
- **Files:** chronokv.hpp, README.md
- **Functions:** wal_recover_buf (per-segment check), WalSegments::recover_all (no cross-segment LSN validation)
- **Approximate lines:** 2226 (within-segment gap/duplicate check); 4345-4363 (recover_all assembles segments without LSN validation); README line ~51 ("strict LSN gap/duplicate detection on recovery")
- **Risk:** Impact 2 x Likelihood 2 x Detectability 4 = **16**

**Summary**

Within a segment, any LSN gap or duplicate yields WalStatus::CORRUPT. Across segments, recover_all never compares LSNs at all: a hand-built directory whose segment 1 holds LSNs 1-3 and segment 2 holds LSNs 100-101 with a valid MANIFEST opens successfully. The engine's semantic gate is cts contiguity (recover/recover_with_checkpoint), which is the meaningful one - LSN is physical write order - but the README claims strict LSN gap/duplicate detection on recovery without qualifying the scope. Designed gap: checkpoint rotation deletes segments, so cross-segment LSN validation is impossible in general; the defect is the documentation's strength, plus a doc-block at 2113 calling the LSN a physical sequence the recovery layer orders by.

**Root Cause**

Documentation overstatement; no engine defect.

**Trigger**

Hand-built WAL with a cross-segment LSN gap opens fine (verified: /tmp/lsn_gap_test.cpp PoC).

**Incorrect Result**

recover_all returns OK; Database::open succeeds.

**Expected Result**

README states within-segment LSN validation (and cts contiguity as the cross-segment gate).

**Violated Invariant**

D1/README claims accuracy.

**Why Existing Protection Fails**

N/A - documentation issue.

**Existing Test Coverage**

No test asserts cross-segment LSN rejection (correctly - it must be tolerated).

**Reproducer**

```
wal/lsn_gap_test.cpp (WAL auditor PoC).
```

**Fix Direction**

Qualify the README sentence; optionally assert the first LSN of each surviving segment exceeds the previous segment's max (only when no rotation deletion occurred).

**Regression Test**

Doc check; no engine test.

---

### [CKV-018] The WAL sync-fallback write path has zero fault-injection coverage, and the short-write retry test is vacuous

- **Severity:** Low (test-suite defect with HIGH bug-hiding value)
- **Confidence:** Confirmed
- **Component:** Fault injection / tests
- **Files:** chronokv.hpp, main.cpp
- **Functions:** pwrite_all (no fault hooks), write_all (hooks live here), the "short write handled by write_all retry loop" test
- **Approximate lines:** 3065 (pwrite_all - no fault::fire); 1972-1992 (write_all - WriteFail/WriteShort sites); main.cpp ~2763 (the vacuous test)
- **Risk:** Impact 3 x Likelihood 4 x Detectability 5 = **60**

**Summary**

fault::fire(WriteFail/WriteShort) exists only inside write_all, whose callers are the MANIFEST, checkpoint, and backup writers. The WAL data path writes through io_uring or pwrite_all - neither has fault hooks. Consequently the test "fault-inj: short write handled by write_all retry loop" arms WriteShort around a commit whose path never calls write_all: the charge never fires, and the test passes with the retry loop deleted. The entire WAL sync-fallback partial-write/EIO error path has no injection coverage anywhere in CI - which is exactly how CKV-004's write-stage-failure divergence stayed invisible.

**Root Cause**

Fault hooks were attached to the wrong I/O primitive when the WAL moved off write(2).

**Trigger**

Any WAL write error (ENOSPC/EIO/short write) in the fallback path - untestable today.

**Incorrect Result**

Test passes while the code it claims to exercise is unreachable through the tested path.

**Expected Result**

pwrite_all honors WriteFail/WriteShort; the short-write test asserts fault::remaining == 0 (the charge actually fired).

**Violated Invariant**

Anti-vacuity: an armed fault must be able to fire on the path under test.

**Why Existing Protection Fails**

The hook placement was never re-audited when the WAL write primitive changed.

**Existing Test Coverage**

This IS the coverage gap.

**Reproducer**

```
Delete write_all's retry loop - the suite still passes.
```

**Fix Direction**

Add fault hooks to pwrite_all (and an io_uring CQE fault hook); re-arm the test with a fired-charge assertion.

**Regression Test**

The fixed test itself.

---

### [CKV-019] find_max_lsn_in_segment returns 0 for a CORRUPT segment, re-seeding the LSN counter inside a still-corrupt file

- **Severity:** Low
- **Confidence:** High-confidence
- **Component:** WAL (append-only open path)
- **Files:** chronokv.hpp
- **Functions:** find_max_lsn_in_segment, WalSegments constructor
- **Approximate lines:** 3405-3406
- **Risk:** Impact 2 x Likelihood 1 x Detectability 4 = **8**

**Summary**

On an append-only open (recover_on_open=false) of a directory whose active segment is CORRUPT, find_max_lsn_in_segment returns 0, so lsn_ restarts at 1 inside a file that already contains those LSNs. A later recovery of that directory then reports the duplicate-LSSN CORRUPT (within-segment check) - i.e., the append-only mode can write a file that the recovery mode of the SAME version rejects. Full recovery rejects the corrupt segment anyway, so the impact is confined to the append-only/replication-spike configuration.

**Root Cause**

CORRUPT is treated like an empty/absent segment for LSN seeding.

**Trigger**

Corrupt the active segment; open with recover_on_open=false; append; reopen with recovery.

**Incorrect Result**

Appended records carry LSNs that duplicate existing frames in the same segment.

**Expected Result**

Append-only open refuses a corrupt active segment (loud) rather than re-seeding.

**Violated Invariant**

LSN uniqueness within a segment.

**Why Existing Protection Fails**

No validation distinguishes 'no records parsed' from 'parse failed'.

**Existing Test Coverage**

Append-only opens are tested against valid WALs only.

**Reproducer**

```
Constructable by hand-corrupting a segment; not built (low value).
```

**Fix Direction**

find_max_lsn_in_segment should signal corruption and the constructor should throw.

**Regression Test**

Corrupt active segment + recover_on_open=false must throw at open.

---

### [CKV-020] maybe_rotate_segment: failed checked_close sets active_fd_ = -1 while the fd may remain open (leak in terminal state)

- **Severity:** Low
- **Confidence:** High-confidence
- **Component:** WAL rotation
- **Files:** chronokv.hpp
- **Functions:** maybe_rotate_segment
- **Approximate lines:** 3496
- **Risk:** Impact 1 x Likelihood 1 x Detectability 4 = **4**

**Summary**

If checked_close fails (EINTR-exhausted close), the code sets active_fd_ = -1 and treats the segment as closed, but the descriptor may still be open - a leak. The instance is in failed_ terminal state at that point (rotation failure fail-stops), so availability impact is nil; the leaked fd persists until process exit. Noted for completeness: every fsync result on both rotation paths is otherwise checked (verified).

**Root Cause**

close failure path conflates 'cannot close' with 'closed'.

**Trigger**

EINTR loop exhaustion on close during rotation (requires fault injection; close rarely fails).

**Incorrect Result**

fd leaked; no functional consequence in the terminal state.

**Expected Result**

Log-and-retain the fd number, or retry close; document the leak in the fail-stop path.

**Violated Invariant**

Descriptor hygiene.

**Why Existing Protection Fails**

Terminal state masks the leak.

**Existing Test Coverage**

No close-failure injection exists.

**Reproducer**

```
Not built (terminal-state-only consequence).
```

**Fix Direction**

Log the exact fd and errno; optionally loop the close.

**Regression Test**

Close-failure fault kind asserting the fd count via /proc/self/fd stays bounded.

---

### [CKV-021] Stale safety comment: commit_locks are claimed released before await_published but are actually held through the barrier

- **Severity:** Low
- **Confidence:** Confirmed
- **Component:** Concurrency documentation
- **Files:** chronokv.hpp
- **Functions:** commit_txn, PublicationTracker::await_published
- **Approximate lines:** 100-108 (v26.3 header comment asserting 'commit_locks RAII scope ends before before_publish'); 5963-6131 (the locks are function-scoped and outlive the barrier call at 6120)
- **Risk:** Impact 1 x Likelihood 5 x Detectability 1 = **5**

**Summary**

The 6d8a13d review-response comment states the publication barrier waits 'with the per-key commit mutexes RELEASED (commit_locks RAII scope ends before before_publish)'. In the code, commit_locks is declared at function scope (5963) and destroyed only at commit_txn's return - after pub_.await_published(cts) at 6120. The comment is factually wrong. This is NOT a deadlock: the deadlock-freedom argument survives either way (every cts holder acquired checkpoint_mu_ shared before reserving; lower-cts committers never need locks the waiter holds to complete), and holding the commit mutexes through the barrier is arguably intentional (it keeps a committing key's validation window closed until publication). But an auditor relying on the comment would mis-model the lock graph, and the discrepancy is exactly the kind of drift the project's own hygiene notes warn about.

**Root Cause**

Comment written against an earlier iteration where the locks were scoped to the validation block.

**Trigger**

N/A (documentation).

**Incorrect Result**

Comment contradicts code.

**Expected Result**

Comment matches code; if release-before-barrier was intended, add the scope.

**Violated Invariant**

Comment-code consistency for lock-graph reasoning.

**Why Existing Protection Fails**

N/A.

**Existing Test Coverage**

TSan clean either way (no runtime distinction).

**Reproducer**

```
Code reading (lock scope vs comment).
```

**Fix Direction**

Correct the comment (or introduce the inner scope and re-run TSan).

**Regression Test**

None (doc).

---

## 6. Concurrency Findings

#### Verified sound

The WAL group-commit machinery (leader election, follower parking on batch_cv_ with per-class predicates, pending_ FIFO draining, exception cleanup with owns_lock() guard, H3 mixed-durability handoff), the v27 publication barrier's deadlock-freedom argument (every cts holder acquires checkpoint_mu_ shared before reserving; the exclusive checkpoint cannot block lower-cts completion), the epoch-pinned reclamation protocol (E1-E16: pins linearized under reader_mu_, severing with release stores, retirement bookkeeping under retired_mu_), the FairSharedMutex writer-preference design, the GC loop's wake/reason distinction (H2 fix verified), and the v25.8 close-race keepalive contract (api_engine() under close_mu_; TSan-verified claim reproduced as clean). The memory-ordering audit comment at 4692-4729 was checked line-by-line against the actual atomics and found accurate.

#### Defects

CKV-008 (fence_unchanged count-preserving-split blind spot - deterministic reproducer via the repo's own dst scheduler; masked in-engine only because every tree mutation is nm_-serialized and tree_->erase/put_with_old have no callers); CKV-012 (no burn-on-throw in group_append's reservation window - the v27 barrier converts an orphaned cts from silent staleness into a permanent commit wedge); CKV-015 (reader-slot/phantom pin leak for transactions abandoned around close() with other keepalives alive); CKV-021 (the lock-graph comment describing commit_locks as released before the barrier is false - the locks are held through await_published; deadlock-freedom survives, but the documented lock graph is wrong). Additionally: no CI configuration ever runs two concurrent BTree mutators, so the OLC retry path is untested at the concurrency it exists for.

## 7. Transaction / MVCC Findings

#### SSI validation (verified sound)

The rw anti-dependency check is implemented as read-set validation under the per-key commit_mu: commit_txn locks every rs and ws key (sorted by KeyEntry* address - deadlock-free), then rejects if any read key's last_write_ts exceeds the transaction's snapshot. Writers hold their commit_mu through install and publication, so a reader either sees the write (conflict) or waits on the mutex and then sees it. Write-skew schedules (T1 reads A writes B; T2 reads B writes A) abort at the second committer in every constructed schedule - the first-updater-wins validation covers the dangerous structures.

#### Phantom detection (verified sound, with a resource cost)

Range reads register [lo, hi, snapshot_ts]; validation re-checks under the WAL batch mutex via the on_reserve callback, and existence transitions are recorded in the same critical section - so commit-timestamp order and phantom-record order are identical, closing the classic scan-vs-insert race. The first (pre-reservation) check at 5974 is racy by design but the on_reserve re-check closes it. Inclusive bounds match range_scan semantics (v20.1 fix verified). The cost: read-set keys are materialized as permanent empty index entries - CKV-009.

#### Visibility and snapshots (verified sound)

Snapshots derive from the published prefix with a seq_cst double-check in acquire_slot_locked; version_at/read_at_idx implement correct as-of reads over strictly-descending commit_ts chains; read-your-writes is provided by the ws_ overlay in ReadWriteTransaction (T1) and correctly recorded pre-overlay for the lincheck checker. The v27 barrier makes acknowledged-before-begin imply visible-to-every-later-snapshot. No stale-read or partial-commit visibility anomaly was constructible.

#### Defects and gaps

CKV-009 (negative-lookup materialization - unbounded invisible index growth); CKV-007's Transaction leg (a callback exception after commit leaves the caller unable to learn the outcome); and the SSI testing gap: lincheck's checkers verify cts-total-order snapshots, real-time order, list-append algebra, scan exactness and set algebra - a plain-SI engine degradation passes all of them (write-skew histories satisfy every checker; the synthetic battery has no write-skew case). Engine anti-dependency enforcement rests on ~8 deterministic single-schedule tests; there is no concurrent/randomized write-skew workload and no public-API write-skew test.

## 8. B+ Tree Findings

#### Structural correctness (single-writer)

Under single-threaded mutation with short keys, the tree is sound: separator wiring on splits, leaf-chain relinking, fence updates, root splits, and the cursor leaf-switch protocol were each verified, and the in-suite fuzz/depth/cursor tests pass legitimately. The cursor-vs-split concern (keys skipped or duplicated) was investigated and dismissed: splits rewrite the old page in place under its exclusive latch and relink next-pointers, and the cursor re-enters the old page id and re-scans from slot 0 - every key seen exactly once.

#### Defects

CKV-001 (uint16 offset wrap on the split-rebuild path - 60 KB OOB write from one legal put); CKV-002 (count-based split overflows a half-page with page-legal keys - silent cross-page corruption, mass key loss, SEGV); CKV-003 (no exception safety: bad_alloc mid-split corrupts structure; engine keeps accepting writes; checkpoint persists the corrupted view - total data loss); CKV-008 (fence_unchanged blind to count-preserving splits - the OLC contract is unsound for concurrent use); CKV-005's scan-side reach (all five engine full-scans bound by the 255 x 0xFF sentinel).

#### Admitted and verified limitations

PagePool::free (9626), LatchTable::erase (9814) and HPRegistry::is_hazardous (9894) have zero callers - the hazard-pointer machinery publishes with seq_cst but nothing consults it, so it protects nothing today (harmless while free() is dead; must be redesigned, not merely wired, before reclamation lands). No merges/rebalance/root-collapse exist; the pool is a monotonic ceiling as documented.

## 9. Memory-Reclamation Findings

#### Protocol (verified sound)

The epoch-pin protocol is correctly implemented: readers publish snapshot + pinned_epoch under reader_mu_; GC severs suffixes with release stores, records retirement epochs, advances reclaim_epoch_, and reclaims only nodes with retired_epoch < min_active_pin_epoch(). The gc_threshold floor (min active snapshot - 1, published prefix) is computed under reader_mu_. A reader that pins before a pass blocks reclamation below its epoch; a reader that pins after cannot reach an already-severed suffix. The v24 Fix-12 shape (acquire_slot_with_phantom + prune under reader_mu_ with external floor) closes the phantom-registration race as claimed.

#### Defects

CKV-005's GC face: keys above the scan sentinel are never visited by gc_once - their versions are never anchored or retired (unbounded chain growth) and their KeyEntry leaks at free_all (LSan-confirmed). CKV-009: empty entries created for read-set validation are permanent - GC has no path to remove them. CKV-015: abandoned-transaction slot/phantom pins leak under the close()+keepalive composition. The documented O(N^2/256) per-sweep rescan was confirmed unchanged (performance, not correctness).

## 10. WAL Findings

#### Framing and recovery parsing (verified sound)

wal_ser/wal_frame/wal_prepend_lsn encode exactly what the strict parser accepts; wal_recover_buf bounds-checks every field (need() guards), enforces exact consumption (q == end), rejects len outside [20, 1 MiB], distinguishes TORN_TAIL from CORRUPT via the valid-after probe, and enforces within-segment LSN contiguity. CRC coverage includes the length prefix. MANIFEST parsing validates magic + CRC + minimum length (D1).

#### Group commit (verified sound)

The leader/follower state machine has no lost-wakeup (every flip under batch_mu_ with notify_all; waiters re-check per-class predicates in while loops), no orphaned batch (pending_ FIFO + front-first drain + leader stand-down), no waiter-result inversion (waiters derive ok from done/written/failed, not from the leader's return), and the leader exception path re-acquires with owns_lock() guard, fail-stops, and notifies before rethrowing. The H3 mixed-durability handoff (cur_batch_ class flip) is correctly queued through pending_.

#### io_uring path (live-kernel verified)

The hard-linked write -> [LINK_TIMEOUT] -> fdatasync chain guarantees write-before-fsync; a deadline-canceled partial write (observed: 65536 of 70000 bytes on a FIFO) is routed to the offset-pinned sync fallback which rewrites the same bytes at the same offset idempotently; enter_internal waits for all pending CQEs so a canceled write's completion is always reaped before the leader proceeds; the bounce buffer is gated on len <= bounce_sz; failure paths drain the CQ ring. Defects: CKV-013 (registered-files dead code + fd-number trap) and CKV-018 (the sync fallback has no fault hooks - the entire class of write-stage failures is untestable).

#### Durability (verified, with one violation)

Sync and Group both wait for their batch fsync before returning (the modes are honestly documented as identical in mechanism); the rollback truncation is itself fsynced (D2 as designed); every fsync result on both rotation paths is checked (verified); D3 fail-stop semantics latch correctly on WAL errors. The violation: CKV-004 - a WRITE-stage failure of an async batch skips the rollback truncation while written was never published, so WalFailure callers resurrect on reopen (deterministic PoC with a clean sync-mode control). Secondary: CKV-012 (no burn-on-throw before the leader window - publication-prefix wedge on OOM), CKV-019 (LSN re-seeding on corrupt segments in append-only mode), CKV-020 (fd leak on close-failure in the terminal state), CKV-017 (cross-segment LSN validation absent - README overstates).

## 11. Recovery Findings

#### Verified sound

recover_all's tolerance matrix behaves as designed: empty orphan at active_id+1 accepted (rotation crash window), non-empty or non-contiguous orphans rejected loudly, missing segments gated on ckpt_ts > 0 (the v25.7 H1 brick is fixed and regression-tested), torn tails truncated idempotently, interior gaps and duplicates rejected with exact diagnostics, the checkpoint chain requires delta contiguity from 1 (v24 Fix 9), and equal (key, commit_ts) re-emission across the chain is idempotent (the v25.8 pre-existing-brick fix). The PITR boundary machinery - including the two review-driven loud-refusal paths (partial-rotation witness and fully-rotated-with-filtered-entries) - was re-verified correct at a mid-window boundary in this audit (probe9). The 20+26 crash points and the crash-fuzz anti-vacuity assertions are real and reached.

#### Defects and gaps

CKV-003 is fundamentally a recovery-facing catastrophe: a corrupted in-memory tree feeds a well-formed checkpoint and rotation destroys the WAL - recovery then faithfully restores the wrong state. CKV-010 (dirty restore_pitr destination - silently wrong or misleadingly diagnosed), CKV-011 (PITR open mutates the source directory, contradicting the README), CKV-019 (append-only LSN re-seeding). Gaps: crash-fuzz children are single-threaded (the concurrent crash-injection test is skipped under sanitizers), _exit()-based crashes preserve the page cache (the project honestly documents that power-loss semantics - the D2 fsync-issued mechanism aside - are untested), and no test compares a checkpoint's recovered state against a WAL-only replay (the assert that would have caught CKV-003's checkpoint face).

## 12. Backup / PITR Findings

#### Backup (verified sound)

backup() checkpoints under the exclusive lock (WAL frozen), copies MANIFEST-referenced artifacts with per-file fsync and directory fsyncs, writes the self-verifying BACKUP_COMPLETE marker last, and verify_backup re-checks sizes, CRCs, chain structure, and parses the full WAL. This audit re-verified the verify/restore consistency question definitively: a backup directory with an extra unlisted orphan WAL segment is rejected by BOTH verify_backup and open() with the same diagnosis (probe15) - an earlier probe's contrary reading was a probe-expectation error, and the API auditor's dismissal stands. Interrupted backups (missing marker) reject as designed; six bk_* crash points are instrumented and fuzz-covered.

#### PITR (verified with defects)

Boundary semantics: an as_of at a checkpoint boundary, inside a surviving window, and beyond the tail were each re-verified correct (probe9); the read-only enforcement (writes -> Status::Failed, checkpoint() throws, health reports the mode) holds; future deltas and records are left untouched. Defects: CKV-010 (restore_pitr trusts a 'fresh' destination it never checks - stale foreign state merges silently or fails with a misleading corruption diagnosis) and CKV-011 (the open mutates the source directory: un-gated .tmp deletion + torn-tail ftruncate - the README's 'does not modify the source directory at all' is false twice). backup_cts/backup boundary semantics verified.

## 13. Filesystem Findings

#### Verified sound

checked I/O helpers handle EINTR loops and short writes; write_all retries partial writes (and its WriteShort fault path is genuinely exercised - on the MANIFEST/checkpoint/backup writers); fsync_dir derives the parent correctly (including root paths); rotation ordering (segment create -> MANIFEST tmp write -> fsync -> rename -> dir fsync) survives every crash point in the matrix; the empty-orphan tolerance is provably safe; ENOSPC-class failures fail-stop via D3 on the fsync path. Directory-creation and permission errors surface as loud open failures.

#### Defects and gaps

The WAL data write path (io_uring / pwrite_all) has no fault hooks (CKV-018) - short writes, EIO and ENOSPC on the highest-volume path in the engine cannot be tested, and CKV-004 is the proof that untested error paths diverge. maybe_rotate_segment leaks an fd on close-failure in the terminal state (CKV-020). Stale MANIFEST.tmp files are never cleaned (harmless; overwritten on next write). Disk-full during checkpoint surfaces correctly (throw before rotation) - verified through the fault matrix. No symlink/path-traversal hardening exists on backup/restore_pitr destinations, but Options paths are caller-controlled local configuration, not untrusted input - noted, not ranked.

## 14. API Findings

#### Verified sound

The lifecycle machinery is the strongest part of the public surface: close() and every API call serialize through close_mu_/api_engine() with engine keepalives (v25.8), streams throw LifecycleError after close and destroy cleanly, observers may outlive the Database, transactions pin the engine, double-close is idempotent, second open of a wal_dir throws (flock), and the transaction state machine rejects commit-twice/get-after-abort/commit-after-abort with LifecycleError exactly as documented. Async get snapshots are consistent (SnapshotGuard). Diagnostics counters are atomics or mutex-guarded - no data races found.

#### Defects

CKV-007 (throwing observer inverts reported outcomes - put throws while durable; txn commit propagates post-commit; batch/erase analogous); CKV-006 (RangeScanStream truncates at tombstones); CKV-016 (put_async/erase_async rethrow instead of Status); CKV-014 (Batch duplicate-key + consumption semantics inconsistent with Transaction and undocumented); CKV-009's API face (transactional negative lookups consume the pool); CKV-010/011 (restore_pitr destination trust; PITR source mutation). README/doc mismatches are tabulated in section 17.

## 15. Test Suite Findings

| Subsystem | Covered | Missing | Risk |
|---|---|---|---|
| WAL framing/decode | 5-case crash matrix; 5,000-iter decoder fuzz; torn-tail/CRC handling | Nothing material | LOW |
| WAL leader/group commit | C1a/C1b deterministic hang detectors; H3 mixed-durability race; D3a-e fail-stop; DST S1/S2 (stress builds only) | Barrier await_published vs concurrent WalFailure burn/wake outside stress builds; multi-threaded child crash (test 17 skipped under sanitizers; crash-fuzz children single-threaded) | MED-HIGH |
| Transactions / MVCC / SSI | Write-skew (v15 #4 sequential, #6 concurrent - raw API); phantom battery (v17 x5, v20.1, coverage 4.2 public API); overlay semantics; conflict/retry; SerialOracle 200+3,000 ops | Write-skew never via the public Database API; never randomized/concurrent at volume; lincheck blind to anti-dependency (see narrative) | HIGH |
| B+ tree | Fuzz (20K ops); depth (50K keys, verifiers); cursor differential; concurrent cursor (1 writer + 1 reader); m16 concurrent engine puts; deterministic loser-delete | Zero concurrent put-vs-put execution; zero keys > ~100 bytes; zero 0xFF-prefix or >=256-byte keys; put_with_old/erase engine paths dead | CRITICAL (hid CKV-001/002/005) |
| GC / epochs | Snapshot-under-pressure; reclaim counters; v23 Phase C pin/reclaim; gc-idle; DST S3; contention benchmark | 'Memory boundedness' test asserts only value readability (vacuous for growth); no concurrent GC-vs-multi-reader oracle; O(N^2/256) rescan only documented | MED |
| Checkpoint | Incremental scaling; chain recovery; rebase/R-REBASE; corrupt/truncated/oversized rejection; under-load | No DST scenario for checkpoint-vs-commit (ckpt_slot_registered / rebase_base_installed unreachable under the scheduler); no checkpoint-vs-WAL differential | MED-HIGH (hid CKV-003) |
| Recovery | Orphan/missing/torn/MANIFEST-corrupt; LSN no-reset; per-instance; strict read-only | verify_full() dead in CI (CHRONOKV_VERIFY_FULL defined nowhere); power-loss semantics untested (documented) | MED |
| Backup | B1 round-trip differential; rejections; per-bk_* interrupts with anti-vacuity; concurrent writers; in-memory | Backup x PITR/rotation combos only via fuzz sampling | LOW-MED |
| PITR | WAL-mid/delta cuts; loud refusals (incl. the 6d8a13d rewrite detector); restore_pitr writability; boundary semantics | Dirty-destination restore (CKV-010); source-mutation assertions (CKV-011) | LOW (was strongest area; two new defects) |
| Lifecycle | Close-race rank-1 (40 iters x 5 API modes); stream/observer/async keepalives; flock cross-process | Slot bookkeeping after abandonment (CKV-015) | LOW |
| Observers | Prefix/erase/txn/batch firing; unregister; TSan race detector; reentrancy deadlock pinned | Throwing callbacks (CKV-007 invisible) | HIGH (hid CKV-007) |
| Async / streams | Happy paths; fsync-fail-after-Committed contract; mixed-API lincheck | RangeScanStream never tested with tombstones (CKV-006 invisible); no exception injection (CKV-016) | HIGH (hid CKV-006) |
| io_uring | Mock state machine (success/fail/timeout/partial + offset); real-kernel E2E in all hooks-on jobs | Real-path CQE faults uninjected (mock only); fallback pwrite_all has NO fault hooks (CKV-018) | MED (hid CKV-004 class) |

#### Vacuous or misaimed tests (confirmed)

(1) 'fault-inj: short write handled by write_all retry loop' (main.cpp ~2763): the armed WriteShort charge can never fire because the WAL commits through io_uring/pwrite_all, which have no hooks - the test passes with the retry loop deleted (CKV-018). (2) 'v17 gc: memory boundedness' (main.cpp ~3627): asserts only that latest values read back; cannot fail on unbounded version or entry growth. (3) 'v18 batch: concurrent LSN contiguity' (~3933): asserts only that recover() does not throw - never reads a key back. (4) m2_phase1 'multi-segment recovery' (~7934): never exercises size-based rotation (its own comment admits it). (5) verify_full() is compiled never (CHRONOKV_VERIFY_FULL defined by no build). (6) The 30s soak asserts only no-throw - a hang/crash detector with no oracle. (7) 'conc: no crash' checks a flag nothing ever sets (noise).

#### lincheck does not verify SSI

The four checkers (cts-total-order snapshot reconstruction, real-time order, Elle-style list-append, scan exactness, set algebra) are all satisfied by plain snapshot-isolation histories: a write-skew history serializes cleanly in cts order, so every checker is green while serializability is gone. The synthetic anomaly battery has no write-skew case because the checker cannot express one. The mixed-API workload's transactional readers (scan A-range, write disjoint T-keys - a true write-skew shape) would commit silently under an SI regression. Adding an anti-dependency edge checker (or an SSI-vs-oracle differential under the existing dst scheduler) is the single highest-value test investment available.

#### DST and sanitizer coverage

The deterministic scheduler covers four scenarios (WAL handoff, leader fault, GC-vs-scan, cursor-vs-split) at the exact historical bug windows, with proven C1/H3 catch acceptance - a genuine strength. Missing: checkpoint-vs-commit (the two stress points in that region are unreachable under the scheduler - the concurrency neighborhood of the v25.7 H1 brick and the v25.8 checkpoint re-emission bug has only timing-based coverage), multi-writer tree, backup/PITR/observer/batch under scheduling. DST determinism is approximate: the 5 ms bounded-patience steal fires whenever the baton holder blocks on real engine locks (group-commit followers park on batch_cv_ by design), injecting timing; steals are counted but not required to be zero. The concurrent crash-injection test and the cross-process flock test are skipped under all sanitizer builds; crash-fuzz children are single-threaded. Warnings never fail any build (-Wall -Wextra with six suppressions, no -Werror, no log grep); clang never runs asan/tsan/stress; no MSan, no LTO, no hardened-libstdc++ run. README's 'tested with g++ 15.2' is dev-machine-only, not CI.

#### Fault coverage infrastructure

The v27 M3 per-kind forced-fault coverage is honest about its own gaps: two of eight kinds (OpenFail, DirFsyncFail) fire zero times under the PR gate vehicle; nightly full-suite forcing kills six of eight kinds early via escaping exceptions (async fut.get() rethrows - the CKV-016 class - and stoi cascades), leaving partial coverage; the untested-lines ledger is published but non-gating by design. The 8 kinds cover I/O only - there is no allocation-failure kind (CKV-003's vector).

## 16. Invariant Matrix

| ID | Invariant | Where enforced | Where tested | Potential violation |
|---|---|---|---|---|
| I1 | Version chains strictly descend in commit_ts toward the tail; commit_ts=0 only above committed nodes | link_version assert (5181); verify_version_chains / verify_full | verify_version_chains in crash-fuzz asserts; in-suite verifiers | CKV-003 (corrupted pages can alias chains) |
| I2 | WAL replay order and cts contiguity: recovery sees every cts exactly once, in order | recover/recover_with_checkpoint contiguity checks (5808-5823, 6972-7015) | Recovery battery; PITR tests | CKV-004 (resurrected prefix replays with contiguous cts - passes the check while violating the semantic) |
| I3 | WAL replay non-decreasing per record | assert in recover (5813) | Recovery battery | None found |
| R1 | published_ < clock_ (publication never outruns allocation) | asserts after commit and recovery (6126, 5838) | In-suite asserts; crash-fuzz verify_publication | CKV-012 wedges the prefix (R1 still holds; progress stalls instead) |
| D1 | MANIFEST integrity (magic + CRC + min length) before trust | read_manifest validation | Corrupt-MANIFEST tests | None found |
| D2 | A batch any caller observed WalFailure for is absent from the WAL after any crash (rollback fsynced) | group_append rollback + checked_fsync (3991-4065) | D2a/D2b (fsync-stage, Sync/Group) | CKV-004 - VIOLATED for async write-stage failures (PoC) |
| D3 | WAL-path fsync error latches fail-stop permanently | failed_ latching; is_failed checks | D3a-e | None found (index-side failures have NO analogue - CKV-003) |
| R-REBASE | Rebase/rotation never deletes WAL needed for recovery | rotate_after_checkpoint under batch_mu_ + ckpt ordering | Crash-fuzz rotation regime; H1 regression | CKV-003 (rotation deletes the last correct copy after a corrupted checkpoint) + CKV-005 (rotation deletes coverage for sentinel-invisible keys) |
| T1 | Read-your-writes inside a transaction | ws_ overlay in RWT::read/range_scan | v22 M2 tests | None found |
| B1 | A backup is self-verifying and complete (marker last; verify checks size+CRC+chain+WAL parse) | backup_to marker machinery; verify_backup | B1 round-trip + interruption tests | None found (verify/restore consistency re-confirmed by probe15) |
| P1 | PITR opens are read-only and non-destructive to the source | pitr_as_of_ gates; delta deletion gated at 6953 | PITR non-destructiveness tests | CKV-011 - half-false (.tmp deletion + torn-tail ftruncate mutate the source) |
| E1-E16 | Epoch-pinned reclamation safety (pins before sever block; post-sever pins cannot reach) | acquire_slot_locked under reader_mu_; retire/reclaim epochs | v23 Phase C tests; DST S3 | None found (CKV-015 leaks a pin - liveness, not safety) |
| PAGE | Every key/value write stays inside its 4096-byte page | put_leaf_nolatch space check (non-split path only) | Nothing (no large-key test) | CKV-001 / CKV-002 - VIOLATED (ASan PoCs) |
| CKPT-C | Checkpoint base contains every live key visible at its cts | checkpoint_locked full scan | Round-trip differentials (all keys visible in tests) | CKV-003 (corrupted scan) + CKV-005 (sentinel-invisible keys) - VIOLATED with permanent data loss |
| OLC | The fence re-check rejects any concurrent structural change raced with | fence_unchanged (11683) | Nothing concurrent (single-writer only) | CKV-008 - VIOLATED for count-preserving splits (deterministic PoC) |

Invariants that exist only in comments (the OLC fence-re-check contract, the group_append burn-on-throw claim) are flagged as findings (CKV-008, CKV-021): both are false as written.

## 17. Documentation Accuracy

| Claimed guarantee | Actual implementation | Evidence | Status |
|---|---|---|---|
| Serializable transactions (SSI), no write skews, no phantoms | rs validation under commit_mu + on_reserve phantom re-check under batch_mu_ | Verified sound by schedule construction; untested against randomized concurrent workloads (lincheck blind spot) | Confirmed |
| Strict-serializable acknowledgements (publication barrier) | pub_.await_published before Committed return; append-only prefix seeding | Barrier logic and deadlock-freedom verified | Confirmed |
| Crash-safe WAL: CRC, segmented, torn-tail truncation, strict LSN gap/duplicate detection | Within-segment LSN check only; cts contiguity is the cross-segment gate | Cross-segment LSN gaps open fine (PoC) | Partially implemented (doc overstates) |
| Invariant D2 (WalFailure batch absent after any crash) | Rollback truncation fsynced - but gated on has_async, not written | CKV-004 PoC: async write-stage resurrection | Incorrect for Async write-stage failures |
| Invariant D3 (fsync error fail-stop) | failed_ latch; health level 2 | Verified across D3a-e | Confirmed |
| PITR open does not modify the source directory at all | .tmp cleanup NOT gated on as_of; torn-tail ftruncate in ctor | probe2: three mutations observed | Incorrect (CKV-011) |
| Async errors via Result<T>/Status; recoverable conditions return Status | put_async/erase_async have no catch; observer exceptions escape | probe8/api_probe: rethrow / Error after durable commit | Incorrect (CKV-007/016) |
| RangeScanStream: incremental B+ tree cursor, per-page snapshot consistency | Cursor + shared latches | Semantics as documented EXCEPT tombstone truncation drops later keys | Partially implemented (CKV-006) |
| Batch: atomic write-set commit | Single commit_txn call - atomic | Duplicate-key rejection + silent consumption undocumented; inconsistent with Transaction | Partially implemented (doc gap, CKV-014) |
| Online backup, verify_backup, B1 marker | checkpoint-under-lock, marker last, per-file CRC | probe15 re-verification: verify/restore consistent | Confirmed |
| Values capped just under 1 MiB; keys to 64 KiB (TooLarge beyond) | MAX_KEY_BYTES=65535 gate at commit_txn | Keys 4 KiB-64 KiB corrupt memory instead (CKV-001) | Incorrect (the cap is not page-safe) |
| Lifecycle: every public API safe to race close() | close_mu_ + api_engine keepalives | Verified sound (TSan-clean design reproduced) | Confirmed |
| ~Transaction aborts if still active | std::abort path | Verified; documented including the unwinding caveat | Confirmed |
| Known limitations (page pool monotonic, observer reentrancy deadlock, GC rescan, crash != power loss, compile memory) | As admitted in README | Each re-verified accurate | Confirmed (honest) |
| Tested with g++ 12/13/14, clang 18, g++ 15.2 | CI: g++-13/14, clang-18 (release only), g++-12 on 22.04 | g++ 15.2 is dev-machine only | Partially confirmed (doc drift) |

## 18. False Positives

| Potential problem | Why it is actually safe |
|---|---|
| Group-commit lost wakeup / stranded followers | Every state flip (written/done/failed, leader stand-down, exception cleanup) happens under batch_mu_ with notify_all; waiters re-check per-class predicates inside while loops. |
| H3 mixed-durability handoff orphaning a batch | The old batch stays queued in pending_ (front-first drain); a leader finding pending_ empty stands down and re-checks its own batch. Verified against the C1/H3 regression detectors. |
| cts-order vs WAL-order inversion | cts is reserved and the record appended under the same batch_mu_ hold; batches drain FIFO; records sort by cts within the batch. |
| Leader exception mid-flush stranding followers | The exception is captured, the state flips to failed under a re-acquired lock (owns_lock() guarded), notify_all, then rethrow - followers see batch->failed and exit with WalFailure; fail-stop propagates to queued batches. |
| Checkpoint rotation deleting segments still needed by in-flight commits | checkpoint_mu_ is held exclusively across the snapshot read (published watermark) and the rotation; committers hold it shared across group_append, so every WAL record has cts <= the rotation boundary; pending batches flush to the NEW segment after rotation; the old fd's final fsync covers async page-cache data. |
| Crash between segment creation and MANIFEST durability | The orphan is empty and at active_id+1 - tolerated, ignored (not adopted), left on disk; a non-empty or non-contiguous orphan still throws, correctly. |
| truncate_torn_tail without a following fsync | Truncated bytes are invalid frames; a power-loss resurrection of the tail is re-detected and re-truncated idempotently at the next open; later batch fdatasyncs persist the new size. |
| io_uring write->fdatasync ordering, deadline races, short writes, stale CQEs, bounce-buffer overflow | Verified live: the hard-linked chain enforces write-before-fsync; a deadline-canceled partial write routes to the offset-pinned fallback (same bytes, same offset - idempotent) and enter_internal reaps all pending CQEs; the fallback's full-length check rejects short writes; failure paths drain the ring; fixed-buffer use is gated on len <= bounce_sz. |
| verify_backup accepting directories that restore rejects | Re-verified definitively (probe15): both reject an extra orphan WAL segment with the same diagnosis. An earlier probe's contrary output was a probe expectation error. |
| PITR mid-window boundary correctness | Re-verified (probe9): at W captured between b and c, the as-of view contains a and b and excludes c. The E0 probe failure that suggested otherwise was the probe author's inverted expectation. |
| Cursor leaf-switch race (keys skipped/duplicated across splits) | Splits rewrite the old page in place under its exclusive latch and relink next-pointers; the cursor re-enters the old page id and re-scans from slot 0 - every key observed exactly once; covered by the concurrent-cursor test and DST S4. |
| Latch-coupling deadlock / shared-to-exclusive upgrade cycle | No in-place upgrade exists anywhere (always release-shared-then-acquire-exclusive); crabbing descents hold at most one shared latch; insert_into_parent_optimistic releases before re-descending; the LatchTable map mutex is never held across page-latch acquisition. |
| Observer registration racing GC prune (v24 Fix 12) | acquire_slot_with_phantom holds reader_mu_ across slot setup and phantom registration; prune consults the same mutex with an external floor. Lock order reader_mu_ -> phantom mu_ is acyclic and unique to these paths. |
| close() TOCTOU on engine_ (the v25.7-era race) | The v25.8 close_mu_/api_engine keepalive design was re-verified: every API path copies the engine shared_ptr under the leaf mutex; teardown defers to the last keepalive; destruction of the Database object itself remains (documented) caller-synchronized. |
| Async get observing an inconsistent snapshot | Single read under SnapshotGuard; consistent by construction. |
| Publication barrier deadlock (barrier vs checkpoint writer) | Every cts holder acquired checkpoint_mu_ shared BEFORE reserving; a queued exclusive writer cannot block the lower-cts completions the barrier waits for; commit_mu's held through the barrier do not form a cycle (lower-cts completion needs no locks the waiter holds). |

## 19. Recommended Fix Order

1. **CKV-001** — Memory corruption via legal API input; one-line-adjacent to CKV-002 fix (shared capacity gate); every day of exposure is a silent-corruption day for any large-key workload.

2. **CKV-002** — Same fix surface as CKV-001 (page-capacity + byte-aware split); the two are one work item in practice.

3. **CKV-003** — The data-destruction chain (exception safety + engine fail-stop + checkpoint cross-validation). Highest severity, second in order only because 001/002 are smaller changes with equal-or-worse blast radius; the fail-stop latch sub-item is small and independently valuable.

4. **CKV-005** — One-line-per-site sentinel fix plus a checkpoint count assert; converts silent permanent loss into correct behavior; pairs naturally with the CKV-003 checkpoint cross-validation.

5. **CKV-004** — D2 is a headline documented invariant currently violated; the fix is a predicate change (has_async -> !written) plus the CKV-018 hooks needed to test it.

6. **CKV-006** — Small, isolated cursor-state fix with a straightforward differential regression test; high user-facing likelihood (any delete in a scanned range).

7. **CKV-007** — Callback isolation boundary; restores the documented Status contract on five write surfaces; includes CKV-016's catch-alls.

8. **CKV-018** — Test infrastructure that gates verification of 004 and future WAL error-path work; hooks only, no engine change.

9. **CKV-009** — Validation-only entry reclamation or phantom-tracked point reads; stops the invisible pool exhaustion.

10. **CKV-010 / CKV-011** — PITR destination guard + source-mutation gating; restores the documented PITR contract.

11. **CKV-012** — Burn-on-throw in group_append's reservation window + comment correction; small, removes a permanent-wedge class.

12. **CKV-008** — Per-page version counter for OLC validation; medium effort; latent until a second tree mutation path exists - but it is a documented-contract defect with a deterministic PoC.

13. **CKV-013/014/015/016/017/019/020/021** — Dead code, semantics/docs, and hygiene items; batch after the correctness work. CKV-016 folds into CKV-007's fix.

## 20. Required Regression Tests

- **CKV-001/002** — Large-key API rejection matrix (4090/4096/60000/65535 -> TooLarge; 4000 -> OK) + mixed-key-size split fuzz with per-insert tree verifiers, under ASan.
- **CKV-003** — bad_alloc charge-budget injection at PagePool::alloc across split steps; after each: get + range_scan consistency, verifier pass, engine fail-stop behavior, and checkpoint-refuses-when-latched; plus checkpoint-vs-WAL differential reopen test.
- **CKV-004** — Async committers + injected write-stage failure (needs CKV-018 hooks): every WalFailure key absent after reopen, every present key acknowledged; same for fsync-stage; sync control.
- **CKV-005** — 0xFF-prefix key set (255/256/300-byte) survives checkpoint+reopen; GC retire counters move for those keys; LSan clean at close; checkpoint key-count == tree size assert.
- **CKV-006** — Stream-vs-vector differential over random key sets with random erases, including tombstone-first and all-tombstone ranges.
- **CKV-007/016** — Throwing-observer matrix across put/erase/batch/txn/put_async/erase_async: reported outcome == durability after reopen; async futures yield Status::Failed; notification completeness surfaced.
- **CKV-008** — Standalone multi-writer BTree test at tree_leaf_latch_gap (dst-scheduled): all keys reachable, chain/separator invariants; then TSan.
- **CKV-009** — N negative-lookups + writes: pool.allocated() bounded vs N; range_scan unchanged; LSan clean.
- **CKV-010** — Double restore into same dest (same and lower as_of): refuse loudly or match fresh-dest state exactly.
- **CKV-011** — Source-directory byte-hash before/after PITR open (with planted .tmp + torn tail).
- **CKV-012** — Exception injection at group_append enqueue (timeout-bounded): WalFailure returned; a subsequent commit completes (prefix advanced).
- **CKV-014** — Batch duplicate-key last-wins + retry-after-Conflict semantics (or documented consumption).
- **CKV-015** — Two transactions + close(): abandoned one releases its pin (gc_stats floor advances).
- **CKV-018** — pwrite_all fault hooks; short-write test asserts the charge fired (fault::remaining == 0).
- **Suite-level** — Public-API write-skew test + lincheck anti-dependency checker; large-key and 0xFF-key fuzz cases; tombstone stream cases; DST checkpoint-vs-commit scenario; un-skip concurrent crash injection under TSan via the crash-fuzz fork pattern.

## 21. What Was NOT Proven

- Power-loss durability semantics. Every crash test uses _exit(), which preserves the kernel page cache; the D2 rollback-fsync and fsync-issued mechanisms are code-verified, but no dm-flakey/hardware fault injection ran. The project documents this gap honestly; this audit confirms it remains open.
- True fsync-error semantics on real hardware (fsyncgate-class): verified only through the FsyncFail/FsyncFailAfterPersist injections, not against a device that actually drops writes.
- io_uring behavior on kernels other than this audit VM's 5.10 (the COOP_TASKRUN ladder and vendor-hardened LINK_TIMEOUT rejections were code-read, not exercised); the mock covers the state machine, real-path CQE faults are uninjected.
- SSI/serializability under randomized concurrent workloads at volume: the validation logic was proven sound by schedule construction and the deterministic batteries, but lincheck cannot detect an SI regression (no anti-dependency checker), no public-API write-skew test exists, and no randomized concurrent write-skew fuzz ran in this audit either.
- The B+ tree OLC path under genuine concurrent mutation: CKV-008 was demonstrated with the repo's own scheduler in standalone mode; in-engine it is nm_-masked, and no TSan run has ever executed two concurrent tree mutators.
- Recovery from every on-disk byte-level corruption class: the parser was audited line-by-line (bounds/exact-consumption/CRC verified) and fuzzed by the suite's 5,000-iter decoder fuzz, but this audit did not run a fresh structured-corruption fuzzer over the checkpoint and MANIFEST formats.
- PITR correctness at every timestamp boundary enumerated in the request (before first commit / exactly at commit / between commits / at checkpoint / inside segments / after final record / beyond latest): boundary, mid-window-surviving, beyond-tail and the two loud-refusal compositions were verified; the remaining boundary permutations were not each executed.
- Resource-exhaustion behavior beyond the page pool (fd exhaustion, thread exhaustion, enormous transactions): code-read only.
- 16-bit/32-bit and big-endian portability: the engine is Linux/x86-64-only by design (documented); no cross-architecture build ran.
- The exact interact/leader-timed-out fallback frequency in production-like kernels; the deadline path was verified on a FIFO, not under a stalled block device.

## 22. Final Verdict

> **VERDICT: D — Not safe for production**

Not safe for production. The decision rests on four independently-sufficient grounds, each confirmed by execution rather than inference: (1) CKV-001 - a single legal Database::put with a key over ~4 KiB performs a confirmed out-of-bounds heap write (ASan: 60,000-byte memcpy at chronokv.hpp:11285 via the public API); (2) CKV-002 - page-legal keys corrupt live tree pages through a count-based split, causing mass key loss and crashes; (3) CKV-003 - one transient allocation failure converts into permanent total data loss through a corrupted checkpoint and WAL rotation, with the engine continuing to accept successful writes in the corrupted state; and (4) CKV-004 - a documented headline durability invariant (D2) is violated deterministically for Async-mode write failures. Secondary but disqualifying on their own for any data-bearing deployment: permanent sentinel-scan data loss (CKV-005), silent stream truncation (CKV-006), and inverted commit-outcome reporting (CKV-007). None of these are hardening nits: three destroy or corrupt data through the ordinary public API. At the same time, the audit's verdict should be read with its scope: the WAL, recovery, backup, lifecycle, and publication machinery - the parts that have been through the project's repeated adversarial-review cycles - verified sound at a level unusual for a project of this size, and the testing infrastructure (crash-fuzz with anti-vacuity, deterministic scheduling, forced-fault coverage) is genuinely excellent. The defects concentrate in the one subsystem that never received that treatment: the B+ tree and its engine integration. Option B ('mostly safe, important defects remain') would require the confirmed defects to be edge-case or adversarial-input-only; they are not - a 4 KiB key and a delete inside a scanned range are ordinary usage.

## 23. The Most Important Question

> If I deliberately tried to corrupt ChronoKV through concurrency, crashes, partial I/O, transaction anomalies, recovery edge cases, memory reclamation, malformed on-disk state, and adversarial API sequences, what is the strongest concrete way I could currently break it?

**The smallest reproducible break.** Smallest reproducible break (one line of user code): db.put(std::string(60000, 'K'), "v"). The key passes the engine's only size gate (MAX_KEY_BYTES = 65535), reaches BTree::put, fails the leaf space check, enters split_leaf, and executes a 60,000-byte memcpy at a uint16-wrapped offset ~55 KB past the 4 KiB page (chronokv.hpp:11285) - heap corruption from a fully legal, single-threaded, in-memory-API call. Under the default 256 MiB pool the same write lands inside the pool and silently cross-corrupts live pages (the oob4 PoC shows the same write class leaving thousands of keys unreachable and ending in a SEGV from a wild page id).

**The strongest concrete break.** Strongest concrete break (the one I would pick to maximize damage): a single transient std::bad_alloc. Fill the page pool under memory pressure (or just size it small), keep committing; one allocation failure inside a tree split escapes (no exception safety), leaving the tree structurally corrupted - point reads still work while range scans return empty (probe13). The engine does not fail-stop (its D3 latch covers WAL failures only) and keeps returning Status::OK for new writes. The operator's natural next move - checkpoint() - then snapshots the corrupted (empty) view into a structurally valid, CRC-clean base and rotates the WAL, deleting the segments that held the real data (probe14). After reopen, every previously-acknowledged write is gone: 17,047 durable, Sync-mode commits in the PoC, and the database reports a perfectly healthy, empty state. One transient OOM, zero anomalies visible to the caller, total silent permanent data loss.

**What was attempted and resisted.** What was attempted and resisted - for the record: concurrent commit-order inversion and publication-prefix stalls (blocked by the batch_mu_-ordered reservation and the barrier's deadlock-free shared-lock discipline); phantom races between scan registration and commit (closed by the on_reserve re-check under the batch mutex); group-commit waiter stranding and leader-exception orphaning (state flips under batch_mu_ with notify_all; exception capture and fail-stop verified); crash-point recovery bricks across the 26-point matrix in the code (the orphan/manifest/rotation tolerances held under this audit's reading, and the historical bricks the fuzzers found are fixed with regression detectors); epoch-reclamation use-after-free (the E1-E16 pin protocol held under schedule construction); backup verify/restore divergence (re-verified consistent); PITR boundary mis-restoration (re-verified correct at mid-window boundaries); and close-race use-after-free on the public API (the keepalive design held). The engine's hardening story is real - it just never reached the tree.
