# ChronoKV Remediation Specification

## Implementation-Ready Fix Plan for CKV-001 through CKV-021

- **Repository:** https://github.com/majnoon40/chronokv
- **Target commit:** 8c77a7ce78157c8ed8487a595a5066769fc8bbd9 (verified unchanged, clean working tree)
- **Version:** 0.27.0 (CHRONOKV_VERSION == README == git history)
- **Date:** 2026-09-24
- **Basis:** Companion volume to the ChronoKV Full Adversarial Audit (2026-09-23, 21 findings). Every finding was re-verified against the current source before this specification was written; all line numbers below were re-checked at commit 8c77a7c.
- **Companion volume:** ChronoKV Full Adversarial Audit (2026-09-23)

---

## Table of Contents

1. Executive Remediation Summary
2. Definitive Finding Status Table
3. Wave 1 — Tree and Data Integrity (CKV-001, 002, 003, 005)
4. Wave 2 — Durability and Failure Handling (CKV-004, 018, 012)
5. Wave 3 — API and Transaction Behavior (CKV-006, 007, 009, 014, 015, 016)
6. Wave 4 — PITR and Filesystem Behavior (CKV-010, 011)
7. Wave 5 — Lower-Severity Correctness and Documentation (CKV-008, 013, 017, 019, 020, 021)
8. Consolidated Regression-Test Plan
9. Invariant-Preservation Matrix
10. Fix Dependency Graph, Commit Sequence, Release Assessment
11. Findings That Must Not Be Fixed
12. IMPLEMENTER MUST NOT GUESS

---

## 1. Executive Remediation Summary

### Purpose and constraints

This document converts the 21 findings of the ChronoKV Full Adversarial Audit into an implementation-ready remediation plan for the engineer (or AI implementer) who will modify the repository. It is a specification, not a refactoring proposal: every change below directly addresses a verified finding or is strictly necessary to make that fix correct. No speculative redesign, no drive-by cleanup, no format changes beyond what a finding demands. Existing behavior and invariants are preserved except where a finding explicitly requires changing a documented contract (three such cases exist and are called out: the key-size limit, Batch duplicate-key semantics, and observer exception containment).

### Source-of-truth re-verification

Before writing this plan, the repository was re-inspected at the audit's target commit 8c77a7c (git status clean, no drift). All 21 findings' source references were re-checked line-by-line against chronokv.hpp; the PoC reproducer sources and the audit's invariant matrix were re-read; the README's documented contracts were re-extracted. Two corrections to the audit's numbers emerged and are incorporated: (1) the true page-safe key bound is 4044 bytes, not approximately 4052 - the B+ tree stores each engine key with an 8-byte encoded KeyEntry pointer as its value (encode_ptr, line 4968), so a leaf entry costs sizeof(PageHeader)=32 + sizeof(LeafSlot)=12 + key + 8 bytes against the 4096-byte page; (2) split_interior (line 11523) selects its split point by entry count exactly as split_leaf does - the audit attributed count-based splitting only to leaves; the interior path has the same defect class and is included in the CKV-002 fix surface.

### Remediation shape

The plan is organized into five waves in strict priority order. Wave 1 (tree and data integrity: CKV-001, 002, 003, 005) receives the deepest specification because it contains all three Critical findings and the permanent-data-loss chain. Wave 2 restores durability contract D2 and the test infrastructure needed to prove it (CKV-004, 018, 012). Wave 3 repairs public-API outcome semantics (CKV-006, 007, 016, 009, 014, 015). Wave 4 restores the documented PITR contracts (CKV-010, 011). Wave 5 handles latent, hygiene, and documentation items (CKV-008, 013, 017, 019, 020, 021). The work decomposes into 20 commits, each independently buildable and testable, each carrying its own fail-first regression test that genuinely fails on the current commit.

### One structural decision the implementer must understand

CKV-003 is not one defect. It is a chain of three independent failures - exception-unsafe tree mutation, no engine fail-stop for index failures, and a checkpoint that trusts a corrupted scan and then rotates the WAL away. The chain is specified as three separately testable sub-fixes (CKV-003a/b/c) because each is valuable alone and because the checkpoint cross-validation (003c) is also the backstop that converts several other silent-loss classes (including CKV-005's checkpoint face) into loud refusals. The engine-level key gate from CKV-001 and the byte-aware split from CKV-002 together close the memory-corruption class at the source; 003b/c ensure that whatever residual failure mode exists can never again convert into silent total data loss.

### Release shape recommendation

The aggregate change is nominally patch-level (no on-disk format change, no WAL format change, no migration, memory-only artifacts), but it changes three documented public contracts: keys above 4044 bytes now return TooLarge instead of corrupting memory, Batch duplicate keys now commit last-wins instead of InvalidTransaction, and observer callback exceptions are now contained instead of inverting commit outcomes. Because the README's documented key limit ('keys to 64 KiB') changes, a MINOR version bump (0.28.0) is recommended rather than a silent patch. The final decision is reserved for the maintainer - see the IMPLEMENTER MUST NOT GUESS section.

---

## 2. Definitive Finding Status Table

| ID | Severity | Status | Root cause (verified at 8c77a7c) | Runtime PoC | Required fix |
|---|---|---|---|---|---|
| CKV-001 | Critical | CONFIRMED | Split-rebuild writers compute uint16 slab offsets with no fit check: insert_into_leaf_no_split 11282-11285, root-split writers 9961-9963 / 10001-10003, insert_into_interior_no_split 11583-11584. Page-safe key bound is 4044 B (32 hdr + 12 slot + 8 ptr value). | poc2 bigkey / poc1 oob1 (ASan OOB WRITE 60000 B at 11285) | Engine key gate at commit_txn (TooLarge) + pre-memset capacity guards in all four rebuild writers |
| CKV-002 | Critical | CONFIRMED | split_leaf 11220 and split_interior 11523 choose the split point by entry count, not bytes; halves can exceed the 4096 B page with page-legal entries. | poc1 oob4 (15,000 small keys + one 3,900 B key: mass loss + SEGV) | Byte-aware split point with both-halves-fit validation and typed refusal when no valid split exists |
| CKV-003 | Critical | CONFIRMED (3 sub-defects) | (a) pool_.alloc() ordered after in-place memset in split_leaf 11230/11246 and split_interior 11527/11539; (b) no engine fail-stop latch for index exceptions; (c) checkpoint_locked 6224 trusts the tree scan, rotation 6337 deletes the WAL. | probe13/14 (get works, scan empty, checkpoint succeeds, reopen = total loss) | 003a pre-allocate before mutation; 003b index_failed_ latch; 003c dirty-coverage + count cross-validation before any checkpoint write |
| CKV-004 | High | CONFIRMED | Rollback gate 3991-4001 tests batch->has_async (durability class) instead of batch->written (failure stage); write-stage failures skip truncation while nobody acknowledged. Stale comment 3791-3804 contradicts the code. | poc4 (8 async committers under RLIMIT_FSIZE: 1-3 resurrected; sync control 0) | Gate truncation on !written; debit async_committed_then_lost only when written was published |
| CKV-005 | High | CONFIRMED | Five full scans bound with hi = string(255, 0xFF): 5053 (gc_once), 5164 (free_all), 5220 (verify_full), 5601 (verify_version_chains), 6224 (checkpoint). Keys >= 256 B of 0xFF prefix sort above it. | poc2 ckpt (256x 0xFF key lost after checkpoint+reopen; LSan KeyEntry leak) | Sentinel-free scan_all (leaf-chain walk, no upper bound) + checkpoint count assert |
| CKV-006 | High | CONFIRMED | ChronoKVRangeScanCursorState::advance 11908-11918 leaves cached_val = nullopt at tombstones; stream_has_next 11927-11929 treats that as end-of-stream. | poc2 stream (5 keys + 1 tombstone: stream yields 2 of 4 visible) | Skip loop in advance(); has_next reflects cursor exhaustion only |
| CKV-007 | High | CONFIRMED | notify_observers 9185-9199 invokes callbacks with no exception boundary; Database::put catch 8540-8542 converts a post-commit callback throw into Error('put failed') after the write is durable; txn/batch/async legs analogous. | probe8 (put reports failure; reopen shows write durable) | Contain exceptions per callback, complete the loop, surface via optional error handler + diag counter; report the true commit Status |
| CKV-008 | Medium (High for standalone BTree) | CONFIRMED (latent in-engine) | fence_unchanged 11683-11693 validates only (key_count, min, max); a count-preserving split (single-entry leaf) passes, so an OLC retry mutates a stale page. Masked in-engine: every tree mutation is nm_-serialized (ensure_index 11829) and tree_->erase / put_with_old have no callers. | poc3 (dst scheduler seed 12, 3/3: put=true then get=false, chain disorder) | Per-page mutation epoch counter compared in the retry (standalone-BTree contract); classify as latent, not production-Critical |
| CKV-009 | Medium | CONFIRMED | commit_txn 5945-5948 calls ensure_index for every read-set key; ensure_index 11812-11841 CREATES a KeyEntry + tree node for absent keys; nothing reclaims empty entries (GC sweeps version chains only; tree erase has no callers). | probe12 (17,047 negative-lookup txns exhausted a 1 MiB pool; 1 visible key) | Route never-existed-key reads through the phantom tracker as point ranges [k,k] instead of materializing entries |
| CKV-010 | Medium | CONFIRMED | restore_pitr 8682-8725 never checks dest_dir emptiness (create_directories 8702); stale WAL from a prior restore merges or triggers a misleading CorruptionError. | probe6 (foreign state leaks into as-of view); probe5 (misleading interior-gap error) | Refuse a non-empty destination loudly at entry |
| CKV-011 | Medium | CONFIRMED | recover_with_checkpoint .tmp cleanup 6712-6742 runs unconditionally (not gated on PITR); open_segment 3453 ftruncates the source's torn tail in the WalSegments constructor. | api_probe2 E1/E2/F1 (three source mutations after a PITR open) | Gate .tmp cleanup on !pitr; defer torn-tail repair on PITR opens; update README |
| CKV-012 | Medium | PARTIALLY CONFIRMED (mechanism certain, trigger OOM-class) | group_append reservation window 3679-3743 (clock.fetch_add, on_reserve, wal_ser, make_shared, push_back) has no burn-on-throw; the leader try/catch starts at 3813. commit_txn comment 6009-6017 claims the burn exists. An escape orphans the cts; await_published 6120 then blocks every later commit. | Code-path demonstration only (no injection hook exists) | on_abandon burn callback wrapping the reservation window; correct the stale comment |
| CKV-013 | Low (latent Critical if enabled) | CONFIRMED | fixed_files_ initialized false (2663), never set true; ensure_file_registered 2865-2874 compares fd NUMBERS, and rotation empirically reuses fd numbers (poc5a: closed fd 4, new segment got fd 4). | poc5a (fd-reuse premise + dead-flag proof) | Remove the dead registered-files machinery and its misleading describe() output |
| CKV-014 | Low | CONFIRMED | Batch::put 8980-8985 appends without dedup; engine rejects duplicates (5932-5934); entries_.clear() at 9008 wipes the batch on every non-throw return including Conflict/TooLarge/WalFailure. | api_probe A (InvalidTransaction + empty batch after commit) | Last-wins dedup matching Transaction; clear entries only on success; document |
| CKV-015 | Low | PARTIALLY CONFIRMED (mechanism verified, no runtime repro) | ~ReadWriteTransaction 7924-7929 skips release_slot / deregister_phantom_reader when engine_live() is false; engine_live() consults the Database liveness flag, not the engine keepalive the transaction chain holds. | Member-order analysis (narrow window: close() + abandonment + another keepalive) | Release the slot whenever the engine keepalive is still held; reorder Transaction members |
| CKV-016 | Low | CONFIRMED | put_async 8863-8889 and erase_async 8941-8966 have no catch boundary; get_async 8905-8927 has one. Worker exceptions rethrow through future.get(), contradicting the documented Result/Status async contract. | api_probe D (put_async().get() rethrows a callback exception) | Add the get_async-style catch(...) -> Status::Failed (folds into the CKV-007 boundary work) |
| CKV-017 | Low | CONFIRMED (documentation) | README claims 'strict LSN gap/duplicate detection on recovery'; the check is per-segment only (wal_recover_buf 2226); cross-segment LSN gaps open fine (hand-built WAL PoC). | lsn_gap_test.cpp (segment 1: LSN 1-3, segment 2: 100-101, opens OK) | Qualify the README sentence; no engine change |
| CKV-018 | Low (test-infrastructure, HIGH bug-hiding value) | CONFIRMED | fault::fire(WriteFail/WriteShort) exists only in write_all 1972-1992 (MANIFEST/checkpoint/backup writers); the WAL data path writes through io_uring / pwrite_all 3065 which have no hooks; the short-write test at main.cpp ~2763 arms a charge that cannot fire. | Mutation test: delete write_all's retry loop - suite still passes | Fault hooks in pwrite_all + re-armed test asserting the charge fired (fault::remaining() == 0) |
| CKV-019 | Low | PARTIALLY CONFIRMED (code-read, not reproduced) | find_max_lsn_in_segment 3405-3406 returns 0 for a CORRUPT segment; the append-only constructor re-seeds lsn_ at 1 inside a still-corrupt file; a later recovery of the same directory reports duplicate-LSN CORRUPT. | Constructable by hand-corrupting a segment; not built | Signal corruption out of find_max_lsn_in_segment; append-only open throws on a corrupt active segment |
| CKV-020 | Low | PARTIALLY CONFIRMED (code-read, terminal-state only) | maybe_rotate_segment 3496: failed checked_close sets active_fd_ = -1 while the descriptor may remain open; the instance is already in the failed_ terminal state, so availability impact is nil. | Not built (requires close-failure injection) | Log fd and errno on close failure; document the terminal-state leak; no functional change |
| CKV-021 | Low | CONFIRMED (documentation) | Header comment 100-108 (from 6d8a13d) claims commit_locks are released before await_published; commit_locks is function-scoped (5963) and held through the barrier call (6120). No runtime effect; TSan clean either way. | Code reading (lock scope vs comment) | Correct the comment; no code change |

Note on CKV-016: folded into CKV-007's specification (the same exception boundary, one shared test matrix) but tracked as its own commit for revert isolation. Note on CKV-003: specified as three sub-defects (a/b/c) with independent tests; the ID remains one finding for traceability.

---

## 3. Wave 1 — Tree and Data Integrity (CKV-001, 002, 003, 005)

Wave 1 contains all three Critical findings and the permanent-data-loss chain. These four specifications are the deepest in the document: every rebuild writer, every split path, every affected engine surface, and the capacity model are enumerated. Read CKV-001 and CKV-002 together — they share the page-capacity model and the pre-memset validation layer, and land as consecutive commits.

### CKV-001 — Oversized key wraps the uint16 page offset: out-of-bounds write via one legal put

- **Severity:** Critical
- **Status:** CONFIRMED
- **Wave:** 1

**Current defect**

The B+ tree stores variable-length keys inside a 4096-byte page using 16-bit byte offsets. The direct-insert path validates that an entry fits a page before writing (put_leaf_nolatch 11152-11163), but the split-rebuild writers perform no fit validation at all. A key larger than the page's usable capacity makes the computed offset wrap modulo 2^16, and the subsequent memcpy writes the key bytes far past the page. The engine's only key-size gate is MAX_KEY_BYTES = 65535 (line 2064, enforced at commit_txn 5884-5887), so any key between 4045 and 65535 bytes is accepted by the public API and reaches this write.

**Root cause**

File: chronokv.hpp (single header, all references below). Functions and exact sites: (1) insert_into_leaf_no_split 11266-11292 - value_off = static_cast<uint16_t>(free_hi(p)) - value.size() at 11282 and key_off = value_off - key.size() at 11283 are computed in size_t arithmetic and truncated back to uint16_t; the memcpy at 11284-11285 then writes key.size() bytes at the wrapped offset with no capacity check. (2) BTree::put root-split writer 9961-9963: key_off = PAGE_SIZE - r.split_key.size() wraps for split keys over 4096 bytes. (3) BTree::put_with_old root-split writer 10001-10003: identical pattern. (4) insert_into_interior_no_split 11583-11584: key_off_new = slab_top - key.size(), no check. Variables involved: key_off / value_off / slab_top (uint16_t), key.size() and value.size() (size_t), PAGE_SIZE = 4096 (9570), PageHeader = 32 bytes (9742-9756), LeafSlot = 12 bytes (9763-9768), InteriorSlot = 12 bytes (9771-9775). Control flow: Database::put -> commit_txn (size gate passes: k <= 65535) -> ensure_index (11812, tree_->put at 11839) -> put_recursive -> put_leaf_nolatch (space check fails at 11155-11163) -> split_leaf (11188) -> insert_into_leaf_no_split rebuild (11282-11285) -> out-of-bounds memcpy. Invariant violated: PAGE - every key/value write must stay inside its 4096-byte page. Capacity model correction (verified this session): the engine's tree value is an 8-byte encoded KeyEntry pointer (encode_ptr 4968-4972), so a leaf entry costs 32 + 12 + key + 8 bytes; the exact page-safe key bound is 4096 - 32 - 12 - 8 = 4044 bytes (the audit's '~4052' assumed zero-length values).

**Observable failure**

A single call db.put(std::string(60000, 'K'), "v") executes a 60000-byte memcpy approximately 55 KB past the 4 KiB page. Under ASan with a small pool: heap-buffer-overflow WRITE of size 60000 at chronokv.hpp:11285. Under the default 256 MiB pool the write lands inside the pool and silently cross-corrupts live pages, which manifests later as mass key loss, disordered leaf chains, and SEGV (the same write class as the oob4 PoC). The user sees either an unexplained crash or, worse, no immediate symptom at all.

**Contract / invariant violated**

The PAGE invariant (in-code). The README's documented contract is itself defective here: 'Values capped just under 1 MiB; keys to 64 KiB (TooLarge beyond)' describes a limit the tree cannot honor - the documented contract must change as part of this fix. There is no documented contract that promises 64 KiB keys work; the defect is that the limit is enforced at the wrong layer.

**Minimal safe fix**

A single authoritative page-safe key policy plus defense-in-depth tree guards. (1) Define TREE_MAX_KEY_BYTES = PAGE_SIZE - sizeof(PageHeader) - sizeof(LeafSlot) - sizeof(KeyEntry*) = 4044 as a named constant next to MAX_KEY_BYTES (line ~2064), with static_asserts pinning the layout sizes so a future header change moves the bound automatically. (2) Extend the existing gate at commit_txn 5884-5887: reject k.size() > TREE_MAX_KEY_BYTES with TxnResult::TooLarge (maps to Status::TooLarge). Validation position: the same pre-lock, pre-WAL, pre-reservation, pre-index point as the existing MAX_KEY_BYTES check - no cts is reserved, no WAL record is built, no index mutation occurs, so nothing must be burned or rolled back. The transaction remains retryable: no state was mutated, and a retry with a smaller key (or after the caller shrinks the key) succeeds. (3) Tree-level guards (defense in depth, required because the BTree is also a standalone component whose values are not 8-byte pointers): split_leaf validates every collected entry satisfies sizeof(PageHeader) + sizeof(LeafSlot) + key + value <= PAGE_SIZE BEFORE the memset at 11230; split_interior validates sizeof(PageHeader) + sizeof(InteriorSlot) + key <= PAGE_SIZE per entry before the memset at 11527; the two root-split writers (9959-9963, 9999-10003) assert r.split_key.size() + sizeof(PageHeader) + sizeof(InteriorSlot) <= PAGE_SIZE before their memcpy and throw std::length_error otherwise. These guards throw BEFORE any page mutation, so a throw leaves the tree structurally valid (the exception-safety posture required by CKV-003a).

**Exact implementation locations**

commit_txn 5884-5887 (add the TREE_MAX_KEY_BYTES condition to the existing loop); constant block at 2050-2081 (new constant + static_asserts); split_leaf between the sort at 11216-11217 and the memset at 11230; split_interior between entries assembly (~11520) and the memset at 11527; BTree::put 9959-9963; BTree::put_with_old 9999-10003; insert_into_leaf_no_split and insert_into_interior_no_split remain unguarded by design (they are only called with entries that passed the split-entry validation; the non-split direct paths already check space at 11152-11163 and 11313-11320 - verified safe, no wrap possible there because the check bounds the same arithmetic).

**Required regression test**

Name: tree-oversized-key-rejection (public API, run under ASan in the asan+ubsan CI job). Setup: Database with default options plus a small page_pool_bytes; no fault hooks needed. Input: puts of keys sized 4044 (boundary-legal), 4045, 4090, 4096, 60000, and 65535 bytes, each with a short value; then a 4044-byte key with an 8-byte value (exactly saturating the page). CURRENT COMMIT: expected FAIL - the 4045+ puts corrupt memory (ASan aborts with heap-buffer-overflow at 11285; without ASan the tree silently corrupts and later verifiers crash). FIXED COMMIT: expected PASS. Assertions: (a) every put with key.size() >= 4045 returns Status::TooLarge; (b) after each rejection, get(key) is absent and the tree's size() is unchanged (no partial mutation); (c) the 4044-byte key commits OK and reads back byte-exact; (d) verify_leaf_chain and verify_separator_invariants (test-only accessors already used by the suite's btree tests) stay green after all operations; (e) the same matrix through Transaction::write and Batch::put (both route through commit_txn's gate). The test asserts the actual invariant - no mutation occurs on rejection and the boundary key round-trips - not merely 'returns a status'.

**Additional affected paths**

Root split: covered (9961/10001 guards + separator validated by split_leaf's entry check, since a separator is one of the entries and interior capacity 4052 >= leaf bound 4044). Leaf split: covered (pre-memset validation). Interior split: covered. Insert (non-split): already safe (11152-11163 space check bounds the same arithmetic - verified no wrap). Delete: tree_->erase has no callers today; erase_recursive performs no slab writes of caller keys, but must receive the same entry-validation if ever wired - note for the implementer, no change required now. Merge: does not exist (README-documented limitation). Checkpoint: no direct key writes; the checkpoint serializer's own 0xFFFF guard at 6250 remains as backstop. Recovery: replays WAL keys through ensure_index directly (not through commit_txn's gate) - the tree guards make a >4044 key in a WAL fail LOUDLY at open (std::length_error -> Database::open throws); a pre-fix database containing such a key was already memory-corrupted, so a loud open refusal is the correct, honest behavior - document it in the commit message. Backup: copies and CRC-verifies files without tree mutation - unaffected; verify_backup parses WAL frames whose framing still supports 65535-byte keys - unaffected. PITR: same as recovery. Async and Batch: route through commit_txn - gated. Transaction::write: routes through commit_txn - gated. The ReadWriteTransaction ws_ overlay: stores full keys in memory only - unaffected.

**Compatibility impact**

Public API: behavior change - keys of 4045-65535 bytes now return TooLarge instead of corrupting memory. No correct program can have depended on the previous behavior; the README's key-limit sentence must be updated in the same commit (recommend: 'keys up to 4044 bytes are stored in the B+ tree index; keys above the page-safe bound return TooLarge'). MAX_KEY_BYTES itself stays 65535 (it is the WAL/framing limit referenced by serialization; the new constant is the tree-storage limit). On-disk format: unchanged. WAL format: unchanged. Recovery: unchanged for clean databases; poisoned pre-fix databases fail loudly at open. Performance: one integer comparison per write-set entry on the existing size-check loop - negligible. Memory: unchanged. Concurrency: unchanged (check is pre-lock).

**Regression risks**

(1) Tests that expect TooLarge only above 65535 must be updated to the new bound - grep the suite for MAX_KEY_BYTES uses. (2) Do not lower MAX_KEY_BYTES itself: WAL framing, checkpoint serialization limits, and public docs reference it; conflating the two limits would break the v24 Fix Group-1 rationale documented at 2264-2267. (3) The 4044 bound depends on the page layout - the static_asserts are mandatory, not decorative. (4) The tree guards throw std::length_error; ensure no engine path catches-and-ignores broad exception types between ensure_index and the caller (verified: none today; the CKV-003b latch will observe it). (5) put_with_old has no callers - its guards exist so future wiring cannot reintroduce the bug; do not be tempted to delete the function instead (out of scope).

---

### CKV-002 — Count-based split overflows a half-page with page-legal keys

- **Severity:** Critical
- **Status:** CONFIRMED
- **Wave:** 1

**Current defect**

Leaf splits choose the split point by entry count: split_leaf computes mid = entries.size() / 2 at line 11220 with no byte weighting and no post-check that each half fits a page. When a leaf holds many small entries and one multi-kilobyte entry, the half receiving the large entry can exceed 4096 bytes in total even though every individual entry is page-legal. The rebuild then writes past the page (wrapping uint16 offsets exactly as CKV-001, or clobbering in-page slot metadata), corrupting adjacent live pool pages. The failure is silent at write time and manifests later as mass key loss, disordered leaf chains, and crashes. split_interior has the identical defect: mid = entries.size() / 2 at 11523 feeding the unguarded rebuild at 11531-11533 and 11546-11548.

**Root cause**

split_leaf 11188-11264: entries are collected and sorted (11200-11217), mid is chosen by count (11220), the original page is memset (11230) and rebuilt with entries[0..mid-1] via insert_into_leaf_no_split (11242-11244), a new page is allocated (11246) and rebuilt with entries[mid..n-1] (11257-11259). Variables: entries (vector of materialized (key, value) string pairs), mid (size_t), the implicit per-entry byte cost which is never computed. Control flow: put_leaf_nolatch's non-split space check fails (11155-11163, entry does not fit even after compaction) -> split_leaf -> count-based mid -> one half overflows in bytes -> out-of-bounds write. Invariant violated: PAGE, plus consistency of the leaf slot array and free-space accounting (an overflowing half overwrites LeafSlot metadata in-page even when the memcpy itself stays inside the allocation - the 'overlap' variant in the oob4 PoC).

**Observable failure**

Fill a leaf region with approximately 130-270 small keys, then insert a 3,100-3,900-byte key that sorts into it. The large key alone inserts cleanly into an empty tree (page-legal), so no key-size gate stops this. PoC (poc1 mode oob4, ASan): after 15,000 small keys plus one 3,900-byte key, thousands of keys return get() == false and the run ends in SEGV inside verify_leaf_chain (10370) - the wrapped write clobbered roughly 15 live pool pages, leaving wild next_leaf_id values. Earlier stages show in-page corruption (LeafSlot arrays overwritten with key bytes) before any ASan trip.

**Contract / invariant violated**

PAGE (page bounding box) and leaf free-space accounting consistency. Both are in-code invariants of the tree; no documented contract licenses their violation. This is a pure defect with no contract dimension to change.

**Minimal safe fix**

Replace the count-based mid with a byte-aware split point that provably fits both halves, plus a typed refusal when no valid split point exists. Exact algorithm for split_leaf: (1) The CKV-001 pre-memset validation already guarantees every entry cost_i = sizeof(LeafSlot) + key_i.size() + value_i.size() <= 4064 (the leaf budget PAGE_SIZE - sizeof(PageHeader)). (2) Compute prefix sums P[j] = sum of cost_0..cost_{j-1} for j in [0, n]; total = P[n]. (3) A candidate mid m in [1, n-1] is valid iff P[m] <= 4064 AND (total - P[m]) <= 4064. (4) Among valid mids, choose the one minimizing |P[m] - (total - P[m])| (byte balance); break ties toward the smaller m. (5) If no valid mid exists, throw a typed exception (PageCapacityError, a std::runtime_error subclass declared next to the tree, or reuse std::length_error) BEFORE the memset - the tree remains valid and unchanged, and the caller's insert fails loudly. split_interior: identical algorithm with cost_i = sizeof(InteriorSlot) + key_i.size(); the median entry (which travels to the parent and is stored in NEITHER half) must additionally satisfy sizeof(PageHeader) + sizeof(InteriorSlot) + median.size() <= PAGE_SIZE; the halves are entries[0..m-1] and entries[m+1..n-1] with entries[m].child becoming the left page's rightmost child (the existing convention at 11536 - preserve it exactly). Why step 5 is necessary and correct: with old_used <= 4064 and the new entry <= 4064, total <= 8128; a valid mid exists iff some prefix lands in the window [total - 4064, 4064]. A pathological but reachable distribution - one near-maximum entry strictly between two non-empty flanking groups on a ~99% full page (for example: 193 one-byte engine keys filling a leaf, then a 4044-byte key sorting into the middle) - leaves no prefix in that window; no two-way split produces two valid pages. Multi-way splits and overflow pages are design changes explicitly out of scope for this remediation. The loud, pre-mutation refusal is the minimal sound behavior; with the CKV-001 key gate the refusal case is rare (it requires a nearly-full page AND a near-maximum key). The README's known-limitations section gains one sentence documenting it.

**Exact implementation locations**

split_leaf 11188-11264: insert the prefix-sum scan and mid selection between the sort (11217) and the old-next save (11227); replace 'size_t mid = entries.size() / 2' at 11220. split_interior ~11460-11551: same insertion between entries assembly and the memset at 11527; replace 'size_t mid = entries.size() / 2' at 11523. No other function changes: insert_into_leaf_no_split / insert_into_interior_no_split receive only validated entries; the parent insertion and root-split writers receive a separator key already bounded by the entry validation (an engine separator is a leaf entry key, and 4044 <= 4052 interior capacity).

**Required regression test**

Name: btree-byte-aware-split (test-only BTree level plus a public-API mixed-size leg; run under ASan). Setup: standalone BTree over a PagePool plus a Database leg. Input matrix: (a) mixed-size fuzz - keys drawn from [1, 4044] bytes with distributions clustered at page boundaries, 20,000 inserts; (b) the oob4 shape - 15,000 small keys then one 3,900-byte key sorting mid-range; (c) root-split leg - insert until the root splits twice with mixed sizes; (d) interior-split leg - insert enough large separators to force at least three interior splits (each interior holds at most ~338 maximum-size separators); (e) repeated-splits leg - continue (a) past 10 cumulative splits; (f) refusal corner - deterministically construct the no-valid-mid configuration (fill one leaf's worth of 1-byte keys, then insert a 4044-byte key that sorts between them) and assert the typed exception. CURRENT COMMIT: expected FAIL - legs (a)-(e) corrupt memory (ASan abort; without ASan, mass key loss and SEGV in verify_leaf_chain exactly as oob4); leg (f) has no defined behavior (silent corruption). FIXED COMMIT: expected PASS. Assertions after every insert: verify_leaf_chain, verify_separator_invariants, and verify_chain_completeness (the suite's existing test-only verifiers) are green; every previously inserted key is reachable via get(); pool page count grows monotonically (no wild page ids). Leg (f) asserts: the exception type is PageCapacityError; after the throw, all prior keys are still reachable, the verifiers are green, and the tree's size() is unchanged (the tree was not mutated). These assert the real invariant - the tree never writes outside a page and never loses an inserted key - not merely 'no crash'.

**Additional affected paths**

Root split: safe by construction once split_leaf validates entries (the separator travelling up is bounded); the CKV-001 root-writer guards remain as backstop. Leaf split: the fix target. Interior split: the fix target (same class, verified this session at 11523). Insert: the trigger path, now guarded. Delete: no callers today; erase does not split (the tree never shrinks pages - README limitation). Merge: does not exist. Checkpoint: consumes the tree - correctness restored indirectly; no checkpoint change required for this finding. Recovery: replays through ensure_index -> the same guarded put path. Backup / PITR: consume checkpoints and WAL - unaffected directly. Async / batch / Transaction: all reach the tree through the same commit_txn -> ensure_index -> BTree::put path - protected by the guards.

**Compatibility impact**

Public API: one new, rare failure mode - a put whose entry cannot coexist with a nearly-full page's distribution fails with the typed PageCapacityError surfacing as Error from put/Transaction::commit (recommendation: map it to Status::TooLarge at the API layer for consistency with CKV-001's rejection - flagged in IMPLEMENTER MUST NOT GUESS, decision required). On-disk format: unchanged. WAL format: unchanged. Recovery: unchanged. Performance: the prefix-sum scan is O(n) with n <= ~338 maximum-size entries per leaf (typical n is a few dozen); it is dominated by the existing O(n log n) sort and the existing O(n) materialization - negligible. Memory: one O(n) size_t array, reusable across splits (reserve once). Concurrency: none - the split runs under the leaf's exclusive latch exactly as today; the algorithm is local and deterministic.

**Regression risks**

(1) Split points change for ALL mixed-size workloads (not only overflowing ones) - any test asserting exact tree topology, height, or page occupancy must be rewritten to invariant-based assertions; the suite's btree tests already assert invariants (fuzz/depth verifiers), verified compatible. (2) The cursor protocol is split-point agnostic (splits rewrite the old page in place and relink next-pointers; the cursor re-enters and re-scans from slot 0 - the audit verified this independently), but re-run the concurrent-cursor and DST S4 tests to confirm. (3) Preserve mid in [1, n-1]: an empty half (mid = 0 or n) breaks separator arithmetic; the algorithm's candidate range enforces this, do not 'optimize' it away. (4) Preserve the relink order exactly: old_next saved at 11227 BEFORE the memset, set_next_leaf(p, 0) then relink at 11261 - the v25.1 M1.2 fix history lives here. (5) The refusal case must throw BEFORE the memset - an after-the-memset throw is CKV-003 all over again; the test's leg (f) asserts the tree is unchanged, which is precisely the property that distinguishes this design.

---

### CKV-003 — Transient bad_alloc mid-split converts into silent total data loss via a corrupted checkpoint

- **Severity:** Critical
- **Status:** CONFIRMED (three sub-defects)
- **Wave:** 1

**Current defect**

This finding is a chain of three independent defects; each is specified and tested separately. CKV-003a: the B+ tree's split path mutates pages in place BEFORE all allocations succeed (split_leaf memsets the original page at 11230 and only then calls pool_.alloc() at 11246; split_interior memsets at 11527 before alloc at 11539), so an escaping bad_alloc leaves half-linked, half-zeroed pages in the live tree. CKV-003b: the engine has no fail-stop latch for index-side failures - the WAL's D3 machinery latches WalSegments::failed_, but nothing analogous exists for exceptions escaping the tree, so commit_txn keeps returning Committed for later writes on a corrupted tree. CKV-003c: checkpoint_locked snapshots state through the same corrupted cursor scan (6224) with no cross-validation against the dirty map or the WAL, writes a structurally valid base containing the WRONG (empty) view, and then rotate_after_checkpoint (6337) deletes the WAL segments that held the real data - permanently and silently.

**Root cause**

CKV-003a - file chronokv.hpp, split_leaf 11188-11264 (memset 11230 precedes pool_.alloc() 11246; the rebuild loops 11242-11244 and 11257-11259 also construct std::string temporaries per comparison at 11273 which can throw mid-rebuild after the original page contents are gone) and split_interior 11460-11551 (memset 11527 precedes alloc 11539). Variables: p (the page being rewritten in place), new_page_id, entries (the materialized copy that exists BEFORE the memset and could be used to restore). CKV-003b - commit_txn 5935-5948: ensure_index for ws keys (and rs keys at 5945-5948) sits OUTSIDE every try/catch in the function; the burn-on-throw at 6090-6104 protects only the post-WAL install tail; there is no index_failed_ state and no analogue to the wal_->is_failed() check at 5924. CKV-003c - checkpoint_locked 6156-6340: the scan at 6224 (full) or 6205 (dirty range) feeds snap directly; no check compares the scanned set against dirty_keys_to_capture (materialized at 6184-6190) or against tree_->size(); the dirty map is cleared at 6328-6335 and rotation runs at 6337 unconditionally after a successful write. Invariant violations: CKPT-C (checkpoint completeness) is violated by 003c; R-REBASE (rotation never deletes WAL needed for recovery) is violated by the 003c chain; basic exception safety of the index is violated by 003a; the engine fail-stop invariant (D3's index-side analogue, which does not exist yet) is the missing piece of 003b.

**Observable failure**

The verified chain (probe13 / probe14, deterministic): (1) 17,047 successful commits of key 'w' against a 1 MiB pool; (2) a bad_alloc escapes commit_txn mid-split; (3) db.get('w') still returns '17046' but db.range_scan(...) returns 0 keys - the tree is corrupted; (4) db.put('after-crash', ...) returns Status::OK - no fail-stop; (5) db.checkpoint() SUCCEEDS on the corrupted tree; (6) after close and reopen, get('w') is PERMANENTLY LOST and the scan is empty. Every acknowledged, durable, Sync-mode write is gone, and the reopened database reports a perfectly healthy empty state.

**Contract / invariant violated**

CKPT-C and R-REBASE (both in-code invariants with README-facing consequences: the checkpoint is documented as containing every live key visible at its cts). Additionally the undocumented but load-bearing expectation that an engine either works or fails loudly - the WAL half honors it via D3, the tree half does not. No documented contract permits the current behavior; all three sub-fixes restore rather than change contracts.

**Minimal safe fix**

CKV-003a (allocation-order + residual-window containment): move pool_.alloc() BEFORE the first memset in split_leaf (relocate the 11246 allocation and its page fetch to before 11230) and in split_interior (relocate 11539 before 11527). If alloc throws, no page has been touched - the tree is intact and the insert fails loudly with the tree unchanged. This removes the most likely throw point (pool exhaustion - exactly the PoC's trigger). Residual window: the rebuild loops' per-comparison std::string construction can still throw under general memory pressure AFTER the memset; making the rebuild loop bit-exactly exception safe requires either pre-reserved scratch strings or journal-and-restore, both of which are larger changes than this remediation allows. The specified layering is deliberate: 003a removes the reachable trigger; 003b contains every residual throw by latching the engine before anything durable can be built on the corrupted state. Document this layering in the commit message. CKV-003b (engine index fail-stop latch): add std::atomic<bool> index_failed_{false} to ChronoKV; wrap the ensure_index loops (5935-5948) in a try/catch that sets index_failed_.store(true, std::memory_order_seq_cst) and rethrows (the public layers already convert exceptions to Error/Status); at the top of commit_txn, next to the wal_->is_failed() check at 5924, add: if (index_failed_) return TxnResult::DatabaseFailed; in checkpoint()/checkpoint_locked, before acquiring or after acquiring the exclusive lock but BEFORE the scan at 6192: if (index_failed_) throw std::runtime_error('checkpoint() refused: index failed - engine is latched'); health() and diagnostics expose the latch. The latch is monotonic (cleared only by process restart / reopen, exactly like the WAL's failed_). CKV-003c (checkpoint cross-validation): after the scan loop and BEFORE the serialization at 6243, add two checks. Check 1 (dirty coverage - the primary): build the set of scanned keys; every key in dirty_keys_to_capture (which is already filtered to dcts <= cts at 6184-6190) must be present in the scanned set; if any dirty key is missing, throw std::runtime_error('checkpoint refused: index scan incomplete') - nothing has been written, the WAL is intact, rotation never runs. In the probe14 chain, key 'w' is dirty and the corrupted scan omits it - the checkpoint refuses and the data survives in the WAL for recovery. Check 2 (count backstop): pairs.size() == tree_->size() (tree_size walks children from the root, 11856-11859, and is independent of the leaf-chain walk the cursor uses); a mismatch throws the same refusal. Check 2 also backstops CKV-005's checkpoint face and any future scan-truncation class. Both checks run before any file write; the existing RAII slot_guard (6170-6173) releases the reader slot on the throw path.

**Exact implementation locations**

003a: split_leaf 11188-11264 (move 11246-11247 above 11230); split_interior 11460-11551 (move 11539-11540 above 11527). 003b: ChronoKV class body (new member near the failed-state diagnostics); commit_txn 5924 area (entry check) and 5935-5948 (try/catch around the ensure_index loops); checkpoint_locked after 6167/before 6192 (refusal); health()/stats surface (one field). 003c: checkpoint_locked between the scan block (6192-6242) and the serialization (6243); applies to both the incremental path (6205) and the full path (6224). export_checkpoint_no_rotate (the restore_pitr export path, 6342+) flows through checkpoint_locked(rotate=false) - it inherits the checks; verify a PITR export of a healthy engine still passes (it must - all dirty keys are scanned).

**Required regression test**

Name: oom-checkpoint-refusal (three legs, all fail-first). Leg 1 - the probe14 chain: 1 MiB pool, Sync durability; loop { txn reads an absent key; txn.put('w', i); commit } until an exception escapes (pool exhaustion). CURRENT COMMIT: expected FAIL - after the escape, a subsequent put returns OK (assert fails), checkpoint() succeeds (assert fails), and after close+reopen get('w') is absent (assert fails - permanent loss). FIXED COMMIT: expected PASS - the escaping exception latches the engine; subsequent puts return Status::Failed (DatabaseFailed); checkpoint() throws; close+reopen replays the intact WAL and get('w') returns the last committed value. Assertions: every acknowledged commit is present after reopen; the latched engine refuses writes and checkpoints; health() reports the index failure. Leg 2 - injected allocation failure: add a charge-budget fault hook at PagePool::alloc (new fault kind AllocFail under CHRONOKV_FAULT_INJECTION, or a test hook - see IMPLEMENTER MUST NOT GUESS); inject at each split step; after each injection assert: all previously acknowledged keys readable via BOTH get() and range_scan(); verify_leaf_chain / verify_separator_invariants green; if the engine latched, checkpoint() throws instead of writing a base. CURRENT: the injection point does not exist - the test cannot run (counts as FAIL per the fail-first rule; the charge-fired assertion fails). FIXED: PASS. Leg 3 - checkpoint-vs-WAL differential: after a NORMAL checkpoint under load (no injection), reopen and assert every key visible pre-checkpoint is present post-reopen, and the recovered state equals a WAL-only replay of a pre-checkpoint directory copy (the assert that would have caught 003c even without injection). CURRENT: PASS on healthy trees (this leg is the guard against regression, not the fail-first proof); FIXED: PASS. The fail-first property is carried by legs 1 and 2, which assert the actual invariant - acknowledged writes are never silently destroyed.

**Additional affected paths**

Root split: covered by 003a (the root-split writers allocate the new root via pool_.alloc() at 9943/9991 BEFORE any page mutation of the old root - verified already safe; the old root is only rewritten by split_leaf itself). Leaf split / interior split: the 003a targets. Insert: the trigger. Delete: no callers. Merge: does not exist. Checkpoint: the 003c target. Recovery: the beneficiary (the WAL survives to replay). Backup: calls checkpoint first - inherits the refusal (correct: a backup of a corrupted tree would otherwise launder the empty view into a 'verified' backup). PITR: export path inherits the checks; PITR read-only opens never mutate the tree, so the latch cannot trigger on them - verify with one test. Async / batch / Transaction: all surface DatabaseFailed once latched - consistent with the existing WalFailure -> DatabaseFailed mapping. GC: gc_once walks the tree but does not mutate it; on a latched engine GC passes are harmless but pointless - optionally skip them when latched (one if-statement, not required).

**Compatibility impact**

Public API: additive behavior on a previously-undefined state - a corrupted engine now returns DatabaseFailed / throws on checkpoint instead of silently destroying data; no existing correct behavior changes. health()/stats gain one field (additive). On-disk format: unchanged. WAL format: unchanged. Recovery: unchanged (strictly more data survives). Performance: one atomic load per commit (alongside the existing is_failed() check) and an O(dirty-key) set-membership pass per checkpoint over data already in memory - negligible. Memory: the scanned-key set is O(live keys) for full checkpoints / O(dirty) for incremental - bounded by what the scan already materialized (pairs vector), acceptable. Concurrency: index_failed_ is a monotonic atomic; no lock ordering changes; the checkpoint refusal happens under the exclusive checkpoint_mu_ exactly where the scan already runs.

**Regression risks**

(1) Do not latch on WAL-side failures - wal_->is_failed() at 5924 already covers them; latching only index-origin exceptions keeps the two failure domains diagnosable separately (double-latching is harmless but conflates diagnostics - avoid). (2) The dirty-coverage check must use dirty_keys_to_capture (filtered dcts <= cts), NOT the full dirty_since_ckpt_ map - entries with dcts > cts are legitimately absent from this checkpoint and must not trigger the refusal. (3) The check runs BEFORE any file write and BEFORE the dirty-map clearing at 6328-6335 - if it throws, the dirty map must remain intact for the next (post-recovery) checkpoint. (4) A first-checkpoint-on-empty-database edge: dirty map empty, scan empty - vacuously consistent, must not refuse (the check is over the capture set, so it passes - add a test leg). (5) tree_->size() is a recursive walk - on a corrupted tree it could recurse deeply or fault; the dirty-coverage check fires first in every observed corruption shape (probe13's scan-empty signature), but the implementer must not rely on size() being safe on arbitrary corruption - wrap check 2 in the same try/catch posture as check 1 (refuse on any exception). (6) The 003a allocation reorder must not change the page-id allocation sequence observably (tests that pin exact page ids - none found in the suite, verified). (7) PITR export and backup must still succeed on healthy engines - covered by the existing B1/PITR batteries, re-run them.

---

### CKV-005 — 0xFF sentinel bounds every full scan: keys above it are invisible to GC, checkpoint, and teardown

- **Severity:** High
- **Status:** CONFIRMED
- **Wave:** 1

**Current defect**

Every full-keyspace walk in the engine bounds the scan with the literal std::string(255, '\xFF') as the upper bound: gc_once (5053), free_all (5164), verify_full (5220), verify_version_chains (5601), and checkpoint_locked's full path (6224). Any key whose first 255 bytes are 0xFF and whose length is at least 256 compares greater than the sentinel and is skipped by every one of these walks. A checkpoint's base omits such keys while rotate_after_checkpoint (6337) deletes the WAL segments covering their commits, so after reopen the key is permanently gone even though every neighbor survives. GC never visits the key (its versions are never anchored or retired - unbounded chain growth), and free_all leaks its KeyEntry (LeakSanitizer-confirmed).

**Root cause**

File chronokv.hpp; the five call sites enumerated above all call tree_scan("", std::string(255, '\xFF')) which delegates to BTree::range_scan (11850-11854) - the tree's inclusive [lo, hi] implementation is CORRECT (the audit verified it); the defect is the engine's choice of hi. The sentinel is not a true upper bound for a keyspace whose keys can be up to MAX_KEY_BYTES = 65535 bytes (2064). Note the incremental checkpoint path (6205) is NOT affected: its bounds are derived from the actual dirty key strings (6203-6204), so 0xFF keys that are dirty are included - verified this session. The full-scan path (6224) fires on first checkpoint, on rebase, and whenever the dirty set is empty. Variables: the hi string literal; the comparison semantics of std::string operator< (lexicographic, length breaks ties: a 256-byte key with a 255-byte 0xFF prefix sorts above the 255-byte sentinel). Invariant violated: CKPT-C (checkpoint completeness), GC reachability of every live key, teardown completeness.

**Observable failure**

db.put(std::string(256, '\xFF'), "v"); db.checkpoint(); db.close(); reopen - the key is absent while its neighbors survive. PoC (poc2 mode ckpt, public API with WAL + checkpoint + reopen assertion): put 'aaa', a 256-byte 0xFF key, 'zzz'; checkpoint; reopen -> aaa present, zzz present, the 0xFF key ABSENT - 'DATA LOSS CONFIRMED'. The same run's LeakSanitizer report shows the leaked 56-byte KeyEntry from free_all. The key is legal (256 <= 65535) and round-trips correctly through the WAL - only the scans lose it.

**Contract / invariant violated**

CKPT-C and GC reachability are in-code invariants with README-facing consequences (checkpoint completeness is implied by the recovery documentation). No documented contract permits the loss. The fix restores contract rather than changing one. Note the README does not anywhere promise that 0xFF-prefixed keys work - the defect is purely an implementation bound choice, and the fix does not need a contract change (unlike CKV-001).

**Minimal safe fix**

Replace the sentinel with a sentinel-free full-walk and add the checkpoint count assert (which CKV-003c already installs as its check 2 - the same assert closes this finding's checkpoint face; do not duplicate it, share it). Sentinel-free walk, exact design: add to BTree a scan_all() (and a Cursor mode backing it) that iterates from the leftmost leaf (descend from the root always taking child 0 / the leftmost child pointer) and walks the leaf chain via next_leaf_id until the chain ends, with NO upper-bound comparison at all. Concretely: open_cursor(lo, hi) gains a sibling open_cursor_all(lo) whose Cursor stores an unbounded flag; the Cursor's advance/has-next logic skips every hi-side check (key <= hi test and the hi-side fence pruning that uses the page's max_key); the lo-side logic (descent to the first leaf at-or-after lo, lo-side fence pruning) is unchanged - for the five engine sites lo is "" so the walk starts at the leftmost leaf. Add ChronoKV::tree_scan_all() (mirror of tree_scan at 11850-11854) and replace the five sentinel sites. Why sentinel-free rather than a longer sentinel (e.g. 65535 x 0xFF): the user-facing requirement is to eliminate the class - any sentinel reintroduces a bound coupled to MAX_KEY_BYTES that silently breaks again if the documented limit ever changes; the unbounded walk is correct for every future keyspace. The hi-side fence pruning loss is free for full scans: a full-keyspace walk visits every leaf anyway (pruning could only skip pages whose keys are ALL above hi, and there are none).

**Exact implementation locations**

BTree::Cursor (open_cursor and its advance/termination logic, ~10090-10302); new open_cursor_all / scan_all members; ChronoKV::tree_scan_all (new, next to tree_scan 11850-11854); the five call sites: 5053 (gc_once), 5164 (free_all), 5220 (verify_full), 5601 (verify_version_chains), 6224 (checkpoint_locked full path). The incremental path at 6205 stays as-is (bounds from dirty keys are correct). The count assert lives in CKV-003c's check 2 (shared).

**Required regression test**

Name: sentinel-key-visibility (public API, WAL + checkpoint, LSan leg). Setup: Database with default options, GC enabled for one leg. Input: put 'aaa'; put std::string(255, '\xFF') (the sentinel-equal key - inclusive bound means it is visible TODAY and must remain visible); put std::string(256, '\xFF'); put std::string(300, '\xFF') + 'x'; put 'zzz'; overwrite the 256-byte key once (to create a reclaimable old version); checkpoint; close; reopen. CURRENT COMMIT: expected FAIL - after checkpoint+reopen the 256-byte and 300-byte keys are ABSENT (assert fails); the GC leg shows no retirement for their old versions; LSan reports the leaked KeyEntry at close (assert fails). FIXED COMMIT: expected PASS. Assertions: (a) all five keys present after reopen, byte-exact; (b) a GC pass retires the overwritten key's old version (gc_stats retirement counters advance for that key); (c) checkpoint's scanned key count equals tree size (via the shared CKV-003c assert - zero-refusal on the healthy tree); (d) close under LSan leaks nothing; (e) the keys immediately before and after the 0xFF keys in sort order ('aaa', 'zzz', and a key like std::string(255, '\xFF') + '\x00') are unaffected. The test asserts the real invariant - every acknowledged key survives checkpoint+reopen and teardown - not merely 'checkpoint returns'.

**Additional affected paths**

Checkpoint: the primary fixed site (6224). Recovery: consumes the base - fixed indirectly (the base now contains the keys). Backup: checkpoints first then copies - inherits the fix. PITR: export_checkpoint_no_rotate flows through checkpoint_locked(rotate=false) - inherits the fix; PITR boundary semantics themselves (delta filtering) were verified correct and untouched. GC (5053): fixed - 0xFF keys' versions now anchored/retired. Teardown free_all (5164): fixed - no KeyEntry leak. verify_full (5220) and verify_version_chains (5601): diagnostic paths fixed - they now actually cover the full keyspace (a corrupted-tree detection win, synergistic with CKV-003). Root/leaf/interior split, insert, delete, merge: unaffected (no scan involvement). Async / batch / Transaction: read paths use point/range lookups with user-supplied bounds - unaffected. The vector range_scan public API (7178-7181): user-supplied hi - correct as-is.

**Compatibility impact**

Public API: none (internal walk replacement). On-disk format: none. WAL format: none. Recovery: strictly more keys survive (databases written pre-fix that contain 0xFF keys above the sentinel: their LAST checkpoint already lost them - nothing this fix can do retroactively; the WAL segments that still cover them (never checkpointed since) now survive - note in the commit message). Performance: the unbounded walk removes one string comparison per key and disables hi-side fence pruning, which a full scan never benefited from - measured-neutral expected; keep the gc-idle and checkpoint-under-load benchmarks green to confirm. Memory: none. Concurrency: none (Cursor protocol unchanged; shared latches as today).

**Regression risks**

(1) The Cursor's hi-side fence pruning exists for NORMAL range scans - the unbounded mode must be a separate code path or a flag, never a change to range_scan's default behavior; the cursor differential test and DST S4 must stay green. (2) The leftmost-leaf descent must handle the empty-tree case (root is an empty leaf - walk yields nothing; the old sentinel scan also yielded nothing - behavior preserved). (3) Keep iteration order identical (ascending key order) - gc_once and checkpoint serialize in scan order and the checkpoint format is order-tolerant but tests may not be. (4) The five sites must ALL be converted - a partial conversion leaves GC or teardown leaking (the test's LSan leg catches exactly this). (5) Do not 'fix' the incremental path at 6205 - it is already correct and gating it on scan_all would change dirty-key semantics.

---

## 4. Wave 2 — Durability and Failure Handling (CKV-004, 018, 012)

Wave 2 restores documented durability invariant D2 for Async-mode write failures, and installs the fault-injection instrumentation whose absence hid the divergence. CKV-018 lands first: its hooks are the test infrastructure CKV-004's regression test requires. CKV-012 closes the publication-barrier wedge class with a delegated burn.

### CKV-004 — Async-mode write-stage failure resurrects transactions that reported WalFailure (D2 violated)

- **Severity:** High
- **Status:** CONFIRMED
- **Wave:** 2

**Current defect**

When a group-commit batch fails, the rollback truncation is skipped for any batch containing async-durability records. The gate at 3992 tests batch->has_async.load() - a durability CLASS marker - on the theory that 'async commits already returned Committed'. That theory holds only for fsync-stage failures, where batch->written was published under batch_mu_ at 3933 and async waiters were indeed released with success. On a WRITE-stage failure (the pwrite / io_uring write fails partway at 3866-3928), written is never set, so every waiter of the batch - async ones included - exits the wait loop at 3748 with failed=true, returns WalFailure, and has its cts burned by commit_txn at 6068. The partially or fully written batch nevertheless remains in the WAL file, and recovery replays every CRC-valid frame: transactions whose callers observed WalFailure silently resurrect after reopen. Two adjacent defects ride along: the diagnostic debit at 3999-4001 counts never-acknowledged records as async_committed_then_lost, and the comment block at 3791-3804 claims 'The has_async flag is no longer used to gate truncation' - directly contradicting the code at 3992 and the (accurate) comment at 3984-3989; this stale comment must be reconciled or the next auditor repeats this audit's confusion.

**Root cause**

File chronokv.hpp. Function WalSegments::group_append. The rollback section 3991-4009: 'if (!io_ok && batch_start >= 0) { if (batch->has_async...) { no truncate; debit } else { truncate } }'. Variables: batch->has_async (atomic bool, class marker set at 3744-3745), batch->written (published only when io_ok at 3933), io_ok, batch_start (the pre-write lseek end, the truncation target). Control flow: leader writes at batch_start -> write fails (io_ok=false, written never published) -> rollback section gates on has_async -> skips ftruncate -> state flip marks batch->failed -> waiters (predicate at 3747-3748: async waiters gate on !written && !failed) exit with WalFailure -> commit_txn burns cts (6068-6071) -> caller told the write failed -> reopen replays the surviving prefix frames -> resurrection. Invariant violated: D2 - 'a batch for which any caller observed WalFailure is absent from the WAL after any crash' (README line ~259). The predicate confuses failure STAGE with durability CLASS.

**Observable failure**

Eight threads commit in Async durability mode; the group-commit leader's write fails partway (PoC: RLIMIT_FSIZE caps the WAL file so the batch write hits EFBIG mid-batch). All 8 waiters receive WalFailure. After close and reopen, 1-3 of the failed keys are PRESENT - 'RESURRECTED (caller saw WalFailure, key present after reopen)'. The sync-mode control under identical conditions produces 0 resurrections (the truncation fires), proving the asymmetry is exactly the has_async gate. No crash and no power loss is required - the divergence is deterministic on the next open.

**Contract / invariant violated**

D2, verbatim from the README's durability section. This is a documented, headline invariant currently violated - the fix restores the documented contract rather than changing it. (The alternative - documenting write-stage failures as excepted - was considered and rejected: D2's wording says 'any crash', and the resurrection is indistinguishable from the corruption D2 exists to prevent.)

**Minimal safe fix**

Change the rollback gate from the class marker to the stage predicate: truncate whenever written was NOT published; keep the no-truncate branch only for written==true (fsync-stage failure on an async batch - the documented silent-loss contract for already-acknowledged async commits). Exact change at 3991-4001: if (batch->written) { /* fsync-stage failure after async acks: keep records, debit the async subset */ if (batch->has_async.load(std::memory_order_relaxed)) { debit async_committed_then_lost by batch->records.size(); } } else { /* write-stage failure: nobody could have acknowledged - truncate exactly like the sync path */ ...existing truncation body 4002-4009... }. Batches are single-class by the XOR separation at 3715-3732 (verified), so has_async implies every record in the batch is async - the whole-batch debit is exact, no per-record tracking needed. Reading written outside batch_mu_ at this point requires it to be an atomic: change Batch::written to std::atomic<bool> (its other uses are the store under lock at 3933 and reads under lock in waiter predicates - atomics are drop-in compatible; done/failed stay as they are, they are only touched under batch_mu_). Replace the stale comment block 3791-3804 with a corrected description of the actual policy: truncate on !written regardless of class; no-truncate only for published-written async batches (fsync stage).

**Exact implementation locations**

group_append rollback section 3991-4009 (the gate and the debit); Batch struct definition (written -> std::atomic<bool>); the stale comment block 3791-3804. Nothing else: the waiter predicates at 3747-3748 are already correct for both stages; the truncation body 4002-4009 (ftruncate + counters) is already correct; commit_txn's burn path 6067-6071 is already correct.

**Required regression test**

Name: d2-async-write-stage (extends the existing D2a/D2b battery; requires the CKV-018 pwrite_all fault hooks - land CKV-018 first or in the same commit). Setup: WAL directory on the normal filesystem (no RLIMIT needed once hooks exist), DurabilityMode::Async, 8 threads each committing one distinct key, fault charge WriteFail armed to fire once at the WAL write. CURRENT COMMIT: expected FAIL - the fault charge never fires on the WAL path (pwrite_all has no hooks), so all commits return OK; the test's charge-fired assertion (fault::remaining() == 0) fails, exposing the vacuity (and without that assertion the test would pass vacuously - exactly the CKV-018 lesson). FIXED COMMIT: expected PASS - the write fails mid-batch; every waiter returns WalFailure; after close+reopen: every key whose commit returned WalFailure is ABSENT from the recovered state; every key PRESENT after reopen had returned Committed/OK; async_committed_then_lost was NOT debited (nothing was acknowledged). Companion leg (fsync-stage, already covered by D2a extended to async): FsyncFail armed with async committers -> waiters returned Committed -> records present after reopen AND the debit equals the batch size (the documented silent-loss contract). Sync-mode control leg: identical write-stage injection with Sync durability -> 0 resurrections (unchanged behavior). The test asserts the D2 invariant itself - reported-failure implies absent-from-WAL - not merely 'no crash'.

**Additional affected paths**

Async (the trigger surface). Batch / Transaction / put / erase: all committers of a write-stage-failed batch - the truncation now protects every class uniformly. Recovery: replays the truncated-away records as nothing - correct. Checkpoint rotation: unaffected (rotation happens under checkpoint_mu_ exclusive, not on the failure path). PITR / backup: recovery-side consumers - consistent. Root/leaf/interior splits, insert/delete/merge: unrelated (tree-side). The mixed-durability handoff (H3, class flip at 3715-3732): unchanged - a mixed handoff produces SEPARATE single-class batches, each gated correctly by its own written flag.

**Compatibility impact**

Public API: none (internal predicate). On-disk format: none. WAL format: none. Recovery: behavior change only within the D2 contract - fewer records replay (exactly the ones whose callers were told they failed). Performance: one atomic load on the failure path only - negligible. Memory: none. Concurrency: written becomes atomic<bool>; its publication at 3933 is under batch_mu_ and its failure-path read at 3992 is unlocked - the atomic removes a latent data race the current code only avoids by reading the OTHER field (has_async) unlocked; ordering note: on the failure path written was never stored, and on the fsync-failure path the store at 3933 happens-before the rollback section in program order on the same thread - relaxed/acquire loading is sufficient; use seq_cst if the implementer prefers uniformity.

**Regression risks**

(1) The truncation body at 4002-4009 must keep its counter increments and the ftruncate error path (wal_truncate_fails at 4008+) intact - D2's rollback fsync follows in the existing code; do not reorder. (2) The debit must fire ONLY when written==true AND has_async - double-billing or under-billing breaks the coverage-vehicle's accounting assertions (fault_coverage.py reads these counters). (3) The stale comment rewrite must not delete the H3 handoff comments at 3715-3732 - they describe the (correct) class-separation machinery, only the 3791-3804 'Fix 4' narrative is wrong. (4) C1a/C1b deterministic hang detectors and the D3a-e battery must stay green (they exercise sync-class failure paths whose behavior is unchanged). (5) The atomic<bool> conversion of written touches the waiter predicate at 3748 - keep its semantics identical (read under batch_mu_, no ordering change needed).

---

### CKV-018 — The WAL sync-fallback write path has zero fault-injection coverage; the short-write test is vacuous

- **Severity:** Low (test-infrastructure defect with HIGH bug-hiding value)
- **Status:** CONFIRMED
- **Wave:** 2

**Current defect**

fault::fire(Kind::WriteFail / Kind::WriteShort) exists only inside write_all (1972-1992), whose callers are the MANIFEST, checkpoint, and backup writers. The WAL data path writes through io_uring (submit_write at 3866) or the fallback pwrite_all (3065) - neither has fault hooks. Consequently the test 'fault-inj: short write handled by write_all retry loop' (main.cpp ~2763) arms a WriteShort charge around a commit whose execution path never calls write_all: the charge never fires and the test passes with write_all's retry loop deleted (mutation-tested). The entire WAL write-error class - ENOSPC, EIO, short writes on the highest-volume path in the engine - is untestable in CI. This is precisely how CKV-004's divergence stayed invisible: no test can drive a write-stage failure on the WAL path at all.

**Root cause**

File chronokv.hpp; function pwrite_all at 3065 (no fault::fire anywhere in it), contrasted with write_all at 1972-1992 (WriteFail at entry, WriteShort per write call). The hooks were attached to the wrong I/O primitive when the WAL moved off write(2) to io_uring/pwrite in v25.1 M2 Phase 2. Control flow: group_append leader -> io_uring chain (3866-3915) OR fallback pwrite_all (3923-3928) - neither consults the fault system. Invariant violated: the suite's own anti-vacuity discipline ('an armed fault must be able to fire on the path under test') - the project's coverage vehicle (CKV_COVERAGE_FAULT, scripts/fault_coverage.py) treats fired-charges as the ledger's currency, and this path contributes zero coverage.

**Observable failure**

Not a runtime failure - a testing failure. Demonstrated by mutation: delete write_all's retry loop; the suite still passes (the armed charge is unreachable through the tested path). Corollary demonstrated by CKV-004: a real divergence in WAL write-failure handling shipped and survived every CI run because the failure class cannot be produced.

**Contract / invariant violated**

No engine contract - this is a test-infrastructure defect. The violated expectation is the repository's own documented testing discipline (fault-injection coverage with anti-vacuity assertions, per the coverage vehicle and the README's testing claims).

**Minimal safe fix**

Add fault hooks to pwrite_all, mirroring write_all's pattern exactly: at function entry, 'if (fault::fire(Kind::WriteFail)) return false;' (simulated full write failure - the caller's existing io_ok=false path handles it); inside the partial-write retry loop, 'if (fault::fire(Kind::WriteShort)) { pretend the pwrite returned a short count (e.g., len/2, minimum 1 byte); }' - the loop's next iteration then completes the write, exercising the retry logic the vacuous test claims to cover. Both hooks under #ifdef CHRONOKV_FAULT_INJECTION exactly as write_all's. Scope note: hooking pwrite_all covers BOTH the no-io_uring production path (io_uring-disabled CI configuration and kernels without io_uring) AND the io_uring remediation path (a deadline-canceled or short CQE write at 3897-3903 re-routes to the offset-pinned pwrite_all fallback at 3917-3928 - verified this session), so the injection reaches the production WAL write machinery on every configuration. A real-kernel io_uring CQE-fault hook (negative res on the write CQE) is a larger instrumentation of the wrapper - explicitly deferred (see IMPLEMENTER MUST NOT GUESS); the mock IoUring already covers the state machine, and the fallback re-routing makes pwrite_all the convergence point of all write-failure remediation. Re-arm the vacuous test: point the existing 'short write' test at the WAL commit path (a commit through pwrite_all - force it by disabling io_uring in that test's Options, the same knob the io_uring-disabled CI configuration uses), and add the anti-vacuity assertion 'fault::remaining() == 0' (the charge fired) plus 'commit succeeded with the retry loop having consumed the short write' (the committed key reads back after reopen).

**Exact implementation locations**

pwrite_all 3065 (two hook insertions); main.cpp ~2763 (the vacuous test: retarget to the WAL path + charge-fired assertion + read-back assertion); fault::Kind declarations (no new kinds needed - WriteFail/WriteShort exist).

**Required regression test**

Name: fault-inj: short write handled by pwrite_all retry loop on the WAL path (de-vacuated) - the fixed test itself is the regression test. Setup: Options with io_uring disabled (the existing knob), WAL enabled, Sync durability. Input: one put with a WriteShort charge armed; then a WriteFail charge armed for a second put. CURRENT COMMIT: expected FAIL - the charge never fires (fault::remaining() != 0 - the assertion fails), and the test passes only vacuously without it; with the assertion, it fails loudly, which is the point. FIXED COMMIT: expected PASS - the short write fires and is retried (commit returns OK; key present after reopen; fault::remaining() == 0); the WriteFail leg returns WalFailure and the key is absent after reopen. Mutation check (the anti-vacuity proof): delete pwrite_all's retry loop -> the test must FAIL (the charge fires but the write never completes). Assertions: charge fired; outcome matches the injected failure; durability after reopen matches the reported outcome - asserting the real contract (injected faults produce the documented outcomes), not 'no crash'.

**Additional affected paths**

The hooks sit in the shared pwrite_all - every caller gains injectability: the WAL fallback (the target), the io_uring remediation re-route (3897-3928), and any future positioned-write caller. write_all's existing MANIFEST/checkpoint/backup coverage is untouched. Recovery / checkpoint / backup / PITR: unaffected (no behavior change - hooks are compile-time gated and default-off in release builds). Tree paths: unrelated. Async / batch / Transaction: their write failures become testable (CKV-004's test rides on this).

**Compatibility impact**

None in production: both hooks are inside #ifdef CHRONOKV_FAULT_INJECTION (the fault-injection builds define it; release builds compile them out). On-disk / WAL formats: none. Performance: one branch per pwrite_all call in fault builds only. The CI matrix gains no new job - the existing asan+ubsan and stress jobs compile with fault injection; the coverage vehicle's ledger (fault_coverage.py) automatically picks up the new fire sites (it greps error-line sites - add the two lines to its ledger if the script enumerates sites explicitly; verified the script is heuristic on fault::fire call sites, so it picks them up without edits).

**Regression risks**

(1) WriteShort's short-count simulation must return a PARTIAL positive count (len/2, at least 1) - returning 0 or -1 exercises different error paths than the retry loop; keep it faithful to a genuine short write. (2) The hook must fire only ONCE per charge (fault::fire's existing semantics - verified) so the retry's second iteration succeeds; otherwise the test loops forever. (3) pwrite_all is also used by the io_uring fallback DURING NORMAL short-CQE remediation - an armed charge there is fine (deterministic), but tests mixing io_uring deadlines and fault charges could double-fail; the re-armed test disables io_uring precisely to keep the leg deterministic. (4) The vacuous test's original intent (write_all's retry loop) still deserves coverage - write_all remains hooked; keep or retarget its own test rather than deleting the assertion that write_all handles short writes (the MANIFEST writer path).

---

### CKV-012 — group_append has no burn-on-throw in the reservation window: an escaped exception wedges every later commit at the publication barrier

- **Severity:** Medium
- **Status:** PARTIALLY CONFIRMED (mechanism certain, trigger OOM-class)
- **Wave:** 2

**Current defect**

commit_txn's error handling at 6009-6017 relies on a documented contract: 'group_append's internal catch guarantees any ts it reserved was already burned.' That catch exists only around the leader's unlocked I/O section (try at 3813). The window from the cts reservation (clock.fetch_add under batch_mu_ at 3679) through on_reserve (3697), wal_ser (3700), the class-flip handoff (3715-3737), make_shared<Batch> (3739), and records.push_back (3743) has no try/catch and no burn. An exception escaping this window - on_reserve re-throwing, wal_ser's size throw (closed by the API pre-checks at 5884-5921, which replicate its limits exactly - verified), or records.push_back / make_shared bad_alloc - propagates with the cts reserved but neither completed nor burned. The published prefix then stalls below that cts forever, and because the v27 strict-serializability barrier (pub_.await_published at 6120) blocks every later commit until the prefix covers its own cts, every subsequent commit hangs - a permanent, silent engine wedge while holding its KeyEntry commit mutexes.

**Root cause**

File chronokv.hpp. Function WalSegments::group_append: the unprotected window is lines 3679-3743 plus the leader-election prologue up to the try at 3813; the only burn sites in the entire engine are in commit_txn (6055, 6062, 6068, 6102 - verified by grep: pub_.burn appears nowhere else), and none of them can fire for this window because the exception escapes group_append before commit_txn's catch at 6025 even knows a ts existed (the catch correctly notes 'we cannot burn cts here because we don't know whether fetch_add was reached'). WalSegments has no access to the engine's PublicationTracker (pub_) - the burn must be delegated back. Invariant violated: publication-prefix progress - every reserved cts eventually completes or burns (the precondition the v27 barrier's deadlock-freedom argument rests on; R1 itself still holds, progress stalls instead).

**Observable failure**

No runtime PoC exists (no injection hook reaches the window; the size-throw path is closed by the pre-checks) - this is a code-path demonstration: with the barrier in place, the historical failure mode ('silently stale prefix') becomes 'every later commit hangs'. A single transient bad_alloc under memory pressure at records.push_back is sufficient. The mechanism was verified by direct reading: the pre-leader window has no try/catch, and the call-site comment that claims otherwise (6009-6017) prevented the gap from being noticed through two review cycles.

**Contract / invariant violated**

The burn-on-throw contract as WRITTEN IN THE COMMENT at 6009-6017 - the code does not implement the comment. The publication-prefix progress invariant (unwritten but load-bearing for the v27 barrier). Not a README-documented contract; the fix makes the code match its own documentation.

**Minimal safe fix**

Delegate the burn to the engine via a callback, mirroring the existing on_reserve parameter: group_append gains a parameter 'const std::function<void(uint64_t)>& on_abandon'; the window from the reservation through the leader-election try is wrapped in try/catch: on catch, call on_abandon(ts) (if ts was reserved - the reservation is the FIRST statement in the window, so ts is always valid there), then rethrow. commit_txn passes '[this](uint64_t t){ pub_.burn(t); }'. The leader section's existing catch (3813+) handles post-election exceptions and already fails the batch (waiters burn via the WalFailure path at 6068) - do not double-burn: the wrapper must cover only up to the point where the batch is enqueued and the leader machinery owns the record. Precisely: wrap 3679 (after fetch_add) through 3745 (the has_async store, i.e., the record's enqueue into my_batch); from 3747 on, the waiter/leader machinery guarantees either a batch outcome or the existing leader-exception fail-stop - verified. Deadlock analysis (required by this finding's constraints): on_abandon is invoked while holding batch_mu_; pub_.burn acquires the publication tracker's mutex (a leaf lock - it never acquires other locks; verified against PublicationTracker 4418-4499). No path acquires the publication mutex and then batch_mu_ (await_published waits on the publication cv WITHOUT holding batch_mu_; complete/burn are leaf calls) - the lock order batch_mu_ -> pub_mu_ is a new but acyclic edge; no deadlock is possible. Also correct the stale comment at 6009-6017 to describe the actual mechanism (pre-leader window burns via on_abandon; leader section fails the batch).

**Exact implementation locations**

group_append signature (3673-3675, new parameter); try/catch wrapping 3679-3745; commit_txn call site (6021-6022, pass the burn lambda); the comment block 6009-6017 (rewrite). The in-memory/no-WAL path's burn at 6047-6057 is already correct (verified) - no change.

**Required regression test**

Name: wal-reserve-burn-on-throw (timeout-bounded). Setup: a test hook at the enqueue site - add leader_pre_enqueue_hook_ mirroring the existing leader_post_unlock_hook_ at 3828 (one #ifdef CHRONOKV_TEST_HOOKS member + call at ~3743), or an AllocFail fault kind at records.push_back - either is acceptable, the hook is smaller (IMPLEMENTER MUST NOT GUESS lists the choice). Input: thread A commits with the hook armed to throw; thread B commits afterwards on a different key. CURRENT COMMIT: expected FAIL - the injection point does not exist (the test cannot arm; with the hook added but the burn missing - the mutation variant - thread B blocks forever in await_published and the test times out: FAIL). FIXED COMMIT: expected PASS - thread A receives WalFailure (exception surfaces as WalFailure via commit_txn's catch at 6025); thread B's commit completes within the timeout (the burned hole advanced the prefix past A's cts). Assertions: A's outcome is WalFailure; B's commit returns OK within a bounded wait (e.g., 5 seconds - matching the lincheck barrier timeout discipline); pub_.published() advances past A's cts; no thread is left holding commit_mu (B's later put on A's key succeeds). The test asserts the actual invariant - the prefix always advances past an abandoned reservation - not merely 'no hang detected by luck'.

**Additional affected paths**

Async / batch / Transaction / put / erase: every committer - the wedge blocked all of them. The mixed-durability class flip (3715-3737) sits INSIDE the wrapped window - a throw during the flip now burns correctly (previously: same wedge). The leader section (3813+) is already safe (fail-stop + waiter notification - verified by the audit's group-commit analysis and the C1a/C1b detectors). Checkpoint: unaffected (does not reserve through group_append). Recovery: unaffected (burn is in-memory only; recovery seeds publication from the WAL). PITR read-only: no commits - unaffected. Tree paths: unrelated.

**Compatibility impact**

Public API: none. On-disk / WAL formats: none. Recovery: none. Performance: one std::function parameter on group_append's signature (already takes on_reserve the same way - one more indirect call ONLY on the exception path; the happy path pays nothing). Memory: one lambda capture per commit call (stack). Concurrency: the new lock-order edge batch_mu_ -> pub_mu_ (analyzed acyclic above); TSan run required in the matrix (the tsan job covers commit concurrency - verified it exercises multi-threaded commits).

**Regression risks**

(1) Do not wrap the WAITER park (3747+) in the same try/catch - a burn there would double-burn records the leader machinery will also fail; the window ends at enqueue (3745). (2) The burn callback must be invoked before rethrowing, exactly once - wrap it so an exception FROM the burn itself (pub mutex corruption - inconceivable) does not mask the original. (3) group_append's early return at 3677 ('if (failed_) return {1, 0}') precedes the reservation - nothing to burn, correct as-is. (4) The no-WAL path (6033-6058) already burns correctly - do not 'harmonize' it into the callback (it has direct pub_ access); the asymmetry is fine. (5) C1a/C1b hang detectors and the H3 mixed-durability test must stay green - they exercise the leader machinery whose behavior is unchanged. (6) The comment rewrite at 6009-6017 must not delete the surrounding v24 Fix-1b history - it documents why the catch exists at all.

---

## 5. Wave 3 — API and Transaction Behavior (CKV-006, 007, 009, 014, 015, 016)

Wave 3 repairs public-API outcome semantics: the stream that truncates at tombstones, the observer boundary that inverts commit outcomes across six write surfaces (including the async future contract, CKV-016), the invisible index growth from negative lookups, Batch's duplicate-key and consumption semantics, and the abandoned-transaction pin leak.

### CKV-006 — RangeScanStream stops at the first tombstone: every later key in the range is silently dropped

- **Severity:** High
- **Status:** CONFIRMED
- **Wave:** 3

**Current defect**

The streaming cursor caches exactly one (key, value) pair. ChronoKVRangeScanCursorState::advance (11908-11918) reads the cursor's current key, resolves its value at the scan timestamp via read_at_idx, and advances the cursor - leaving the cached entry in a (key present, value nullopt) state when the key is deleted at the read timestamp (a tombstone) or otherwise invisible. stream_has_next (11927-11929) returns cached_valid && cached_val.has_value(), so a tombstone is interpreted as end-of-stream: iteration stops and every subsequent key in [lo, hi] is silently omitted. Because tree_->erase is never called (no page reclamation - README limitation), tombstones accumulate forever in the tree, so ANY range containing a deleted key truncates the stream at that key. The vector-based range_scan handles the identical case correctly by skipping invisible keys (7178-7181) - the two public scan surfaces disagree.

**Root cause**

File chronokv.hpp. Struct ChronoKVRangeScanCursorState: advance() at 11908-11918 performs one cache-and-step without checking whether the cached value is visible; stream_has_next() at 11927-11929 conflates 'no value at this key' with 'no more keys'. Variables: cached_valid, cached_key, cached_val (optional<string>), effective_ts, cursor. Control flow: constructor calls advance() once; has_next() gates the loop; next() consumes the cached pair and calls advance(). The defect is the missing loop in advance(). Invariant violated: scan completeness - a stream over [lo, hi] must yield exactly the keys visible at its snapshot (the README's per-page snapshot consistency claim for the stream API).

**Observable failure**

put k01..k05; erase k03; RangeScanStream over [k01, k05] yields k01, k02 and stops. PoC (poc2 mode stream, hooks-off public API): the vector range_scan yields 4 keys (correct); the stream yields 2 keys, last = k02 - 'TRUNCATION CONFIRMED - stream stopped at the tombstone; later keys dropped'. Any workload that deletes inside a scanned range (a consumer draining a queue prefix, a compaction pass) silently loses the tail of its scan.

**Contract / invariant violated**

Scan completeness for the stream API, as documented (README API table: streaming range scans; the per-page snapshot consistency note). The vector range_scan's correct behavior is the de-facto specification of the shared semantics - the fix aligns the stream to it. No contract change; the fix restores the documented behavior.

**Minimal safe fix**

Make advance() skip invisible entries: 'void advance() { while (cursor.valid()) { cached_key = cursor.key(); KeyEntry* e = ChronoKV::decode_ptr(cursor.value()); cached_val = kv.read_at_idx(effective_ts, e); cached_valid = true; cursor.next(); if (cached_val.has_value()) return; } cached_valid = false; }'. The loop consumes tombstones and invisible keys without surfacing them, returning only when a visible entry is cached or the cursor is exhausted. stream_has_next stays 'cached_valid && cached_val.has_value()' - now equivalent to cached_valid alone (a true cached_valid implies a visible value); keeping both conjuncts is harmless and defensive. next() is unchanged (it consumes the cached pair and calls advance()). A null KeyEntry (decode failure - cannot happen for engine-written trees, but defensively) also yields nullopt and is skipped - same loop, no special case needed.

**Exact implementation locations**

ChronoKVRangeScanCursorState::advance 11908-11918 (add the while loop). Nothing else: stream_has_next 11927-11929 unchanged; next() unchanged; the vector range_scan 7178-7181 unchanged (already correct); the Cursor itself unchanged.

**Required regression test**

Name: stream-tombstone-differential. Setup: Database with WAL; random seed fixed. Input: for N iterations: random key set of 20-50 keys from a 64-key universe; random erases (25%); for each of a fixed set of ranges [lo, hi] including (a) a range whose FIRST key is deleted, (b) a range entirely of tombstones, (c) a range with a tombstone in the middle, (d) empty ranges and full-universe ranges: collect the stream's output and the vector range_scan's output at the same snapshot. CURRENT COMMIT: expected FAIL - the stream's output is a strict prefix of the vector's whenever a tombstone falls inside the range (assert stream_output == vector_output fails; the (b) ranges yield empty-vs-nonempty). FIXED COMMIT: expected PASS. Assertions: stream output == vector output == the expected visible set at the snapshot (computed independently from the operation log); iteration order strictly ascending; has_next() false exactly when the visible set is exhausted; the stream's first key is visible (never a tombstone). The test asserts the real invariant - the two public scan surfaces agree and both match the snapshot's visible set - not merely 'stream yields something'.

**Additional affected paths**

The stream API (the fix target). Vector range_scan: unchanged reference behavior. Transaction range reads: use the vector path - unaffected. Checkpoint / GC scans: internal tree_scan walks, not the stream - unaffected (and fixed separately by CKV-005). Backup / PITR / recovery: do not consume the stream - unaffected. Async get: point reads - unaffected. The keepalive/lifecycle semantics of the stream state (keepalive_ member, v25.7 review M2) are untouched by the loop change - the close-race battery stays green.

**Compatibility impact**

Public API: behavior fix within the documented contract - streams now yield MORE keys (the previously-dropped visible tail). No signature change. On-disk / WAL / recovery: none. Performance: tombstone keys now cost one extra read_at_idx resolution per skip (a version-chain walk of depth 1-2 typically) - bounded by the number of invisible keys in the range, which the vector path already paid. Memory: none. Concurrency: none (same latch protocol; the loop holds the same state it held per-key before).

**Regression risks**

(1) The loop must not spin on a cursor that reports valid() forever - cursor.next() advances monotonically toward the hi bound or chain end (verified protocol); the existing stream test (Test 14) plus the differential test bound this. (2) Read-only semantics: the loop calls read_at_idx per skipped key - same visibility rules as before, no snapshot drift (effective_ts fixed at construction - verified). (3) The lifecycle keepalive contract: the loop may now run longer per advance() call (skipping many tombstones) - still bounded by the range; the close-race tests cover interruption between calls, not within - acceptable, unchanged semantics.

---

### CKV-007 (+CKV-016) — A throwing observer callback converts committed, durable writes into reported failures; async futures rethrow

- **Severity:** High
- **Status:** CONFIRMED
- **Wave:** 3

**Current defect**

User observer callbacks run inline with no exception isolation: notify_observers (9185-9199) invokes obs.callback at 9195 bare. When a callback throws, the exception escapes into the commit machinery at a point where the commit is already irrevocable, and each public surface mangles it differently: Database::put (8524 notify after map_txn_result; catch at 8540-8542) converts it into a THROWN Error('put failed: ...') AFTER the write is durable - the caller is told the write failed while it is committed and on disk; erase (8555, 8571-8573) identical; Transaction::commit propagates the raw exception after the transaction committed (the caller cannot learn the outcome); Batch::commit (9040) propagates after the batch committed; put_async (8871) and erase_async (8948) invoke notify_observers inside the worker lambda with NO catch boundary (8863-8889, 8941-8966) - the exception is stored in the future and RETHROWN by .get(), while get_async (8905-8927) wraps its whole body in catch(...) -> Status::Failed - the documented async contract exists in one of three lambdas only (this is CKV-016, folded here because it is the same boundary). Additionally: callbacks registered BEFORE the throwing one fire while later observers and later write-set entries are silently skipped (partial notification), and a standard retry-on-failure caller double-writes.

**Root cause**

File chronokv.hpp. Function Database::notify_observers 9185-9199: the callback invocation at 9195 has no try/catch, and the function returns void with no channel for callback failures. The commit paths fire observers at points where the commit is already durable (8524, 8555, 8871, 8948, 9040, and Transaction::commit's notify via write_set()). Variables: observers_ (vector under observer_mu_), ws (the write set being notified), obs.callback (user code - can throw anything). Control flow (put leg): commit_txn returns Committed -> map_txn_result -> Status::OK -> notify_observers(ws) -> callback throws -> propagates out of notify_observers -> put's catch(const std::exception&) at 8540 -> throw Error('put failed: ' + e.what()) -> caller sees failure; reopen shows the write durable. Invariant violated: outcome correctness - a reported failure must not correspond to a committed write (the client-side face of D2); and the README's async contract ('errors via Result<T> / Status'; 'recoverable conditions return Status').

**Observable failure**

Register an observer whose callback throws; db.put('w:k', 'durable') throws Error('put failed: cb fail'); close + reopen -> w:k == 'durable' - 'CONFIRMED: put() reported failure but write is DURABLE on disk' (probe8). Transaction leg (api_probe C): commit() propagates the callback's exception after the commit - the caller cannot distinguish committed-with-callback-failure from failed. Async leg (api_probe D): put_async(...).get() RETHROWS the callback exception instead of yielding a Status.

**Contract / invariant violated**

The README's error-surface contract: 'Status codes ... recoverable conditions return Status' and the async table's 'errors via Result<T> / Status'. The current behavior inverts both. The fix must decide and document the callback contract explicitly: this specification sets it to 'observer callbacks must not throw; if one does, the exception is contained, counted, and reported out-of-band; the commit outcome is unaffected' - a documentation addition, not a change to an existing promise (no existing text promises exceptions propagate from callbacks).

**Minimal safe fix**

Contain exceptions per callback, complete the notification, surface out-of-band, and report the true commit outcome. (1) notify_observers: wrap EACH obs.callback invocation (9195) in try/catch(...): capture the first exception_ptr, continue the loop (every write-set entry x every observer is attempted - notification completeness), count diag::observer_callback_errors (new atomic counter), and return std::exception_ptr (nullptr when all callbacks succeeded) instead of void. (2) Delivery channel: add an OPTIONAL public handler - Database::set_observer_error_handler(std::function<void(std::exception_ptr)>) storing it under observer_mu_; notify_observers invokes it once (outside observer_mu_ - copy the handler under the lock, invoke after release, so the handler itself may call Database APIs) with the captured exception; with no handler registered, the exception is counted and dropped (the documented containment). (3) Call sites report the TRUE outcome: put/erase keep returning the mapped Status (their existing catch at 8540-8542 now only fires for genuine commit-path exceptions - notify no longer throws); Transaction::commit / Batch::commit likewise return their Status. (4) Async legs (CKV-016): put_async and erase_async worker lambdas get the get_async-style outer catch(...) -> return Status::Failed (8863-8889 / 8941-8966); with notify_observers contained, Status::Failed now only ever means the commit itself failed - which IS the documented async error surface; the txnrec recording blocks (873-887 etc.) sit inside the try and record committed=false on the failure path, keeping the checker's history faithful. (5) README: one paragraph in the observers section documenting the containment contract and the error handler.

**Exact implementation locations**

notify_observers 9185-9199 (per-callback try/catch, exception_ptr return, counter, handler invocation); Database class body (handler member + setter + diag counter declaration); put 8524/8540-8542, erase 8555/8571-8573 (no structural change - the catch simply stops firing for observer failures); Batch::commit 9040; Transaction::commit (the notify call site - audit's 9040-family); put_async lambda 8863-8889 (outer catch); erase_async lambda 8941-8966 (outer catch); README observers section. get_async 8905-8927 unchanged (already correct).

**Required regression test**

Name: observer-exception-matrix (six legs: put, erase, batch, transaction, put_async, erase_async). Setup: Database with WAL; one observer whose callback throws std::runtime_error('cb fail') on every invocation; a second well-behaved observer registered AFTER the throwing one (to prove notification completeness). Input: one write per leg through each surface. CURRENT COMMIT: expected FAIL - put throws Error while the write is durable (assert: returned normally with a Status? no - it threw; the leg asserts the reported outcome matches durability: reported failure => absent after reopen FAILS because the key IS present); transaction commit propagates the raw exception (same mismatch); the async futures RETHROW (assert: .get() returns a Status FAILS - it throws); the well-behaved second observer is skipped after the throwing one fires (notification completeness assert fails). FIXED COMMIT: expected PASS - every leg reports the TRUE outcome (Status::OK); after close+reopen every written key is present byte-exact; every observer that can fire did fire (the second observer saw every write - completeness); the error handler captured exactly one exception_ptr per write (or the counter equals the number of matching write-set entries); the async futures yield Status::OK (commits succeeded) and NEVER rethrow. Additional leg: a commit-path failure (WalFailure via injected WriteFail - CKV-018 hooks) with the throwing observer registered: the async future returns Status::WalFailure (not Failed-from-callback) - the outcome stays attributable. The test asserts the real invariant - reported outcome == durable outcome for every public write surface - not 'no exception escaped'.

**Additional affected paths**

put / erase / Batch::commit / Transaction::commit / put_async / erase_async: all fixed (the six legs). get / get_async: read-only, no observers - unaffected. RangeScanStream: no observers - unaffected. Checkpoint / GC / recovery / backup / PITR: no observer invocation - unaffected. The reentrancy deadlock pin (an observer calling back into the Database from notify - the known pinned deadlock): unchanged - the containment does NOT acquire new locks around the callback (observer_mu_ is held exactly as today during callbacks; the error HANDLER runs outside observer_mu_ by design so it may call Database APIs safely - this strictly widens what user code may do from the error channel without touching the pinned deadlock's preconditions). Tree paths: unrelated.

**Compatibility impact**

Public API: additive (set_observer_error_handler + one diagnostics counter); behavior change on a previously-undefined surface - observer exceptions no longer escape (they were never documented to propagate); commit outcomes become truthful. On-disk / WAL / recovery: none. Performance: one exception_ptr local + one branch per callback invocation in the normal path - negligible; the failure path pays one catch per throwing callback. Memory: one function object. Concurrency: the handler is invoked OUTSIDE observer_mu_ (copy-then-invoke) - no new lock ordering; the counter is atomic. The async lambdas' outer catch changes future exception-storage behavior (documented contract now honored).

**Regression risks**

(1) The per-callback try/catch must NOT swallow exceptions from notify_observers' own machinery (lock acquisition, string building) - wrap ONLY the obs.callback call at 9195. (2) The handler invocation outside observer_mu_ means a handler racing observe()/unobserve() sees a handler captured under the lock - acceptable (copy-then-invoke); document the snapshot semantics. (3) The async outer catch must return Status::Failed only for genuine worker failures - with notify contained, classify carefully: commit_txn-escaping exceptions (bad_alloc etc.) -> Failed; observer exceptions never reach the outer catch anymore. (4) The txnrec recording in the async legs must record the OUTCOME the future will report (committed flag matches the returned Status) - the checker's ack edges depend on it (verified the recording blocks already sit correctly inside the lambdas; the catch returns BEFORE recording in get_async's pattern - mirror that ordering and record committed=false with op recorded at entry... precisely: follow get_async's existing structure, which the lincheck battery already validates). (5) The existing observer tests (firing/order/unregister/reentrancy/TSan race) must stay green - the containment adds no locks and no ordering changes on the happy path.

---

### CKV-009 — Transactional reads of absent keys permanently materialize invisible empty entries: unbounded pool exhaustion

- **Severity:** Medium
- **Status:** CONFIRMED
- **Wave:** 3

**Current defect**

commit_txn calls ensure_index(k) for every key in the transaction's read set (5945-5948) in order to lock and validate its KeyEntry::commit_mu. ensure_index (11812-11841) CREATES a KeyEntry and inserts it into the B+ tree when the key is absent. A transaction that reads an absent key (a negative lookup) and also writes anything therefore leaves behind a permanent, invisible empty entry: no version chain, invisible to every range scan and to get(), never reclaimed by GC (empty chains yield nothing to retire), never erased (tree_->erase has no callers). Read-only transactions are masked by the ws.empty() early return at 5873, but any mixed workload - existence checks before conditional inserts, cache-miss probing - grows the index without bound. 17,047 such transactions exhausted a 1 MiB page pool in the PoC while the database contained exactly ONE visible key; the terminal symptom is std::bad_alloc (which then feeds CKV-003's chain).

**Root cause**

File chronokv.hpp. commit_txn 5945-5948 (the rs loop calling ensure_index); ensure_index 11812-11841 (creation semantics); ReadWriteTransaction::read 7951-7958 (rs_.insert(k) for every read, present or absent); the ws.empty() early return at 5873 (which masks read-only transactions - the reason the suite never saw this). Variables: rs (std::set<std::string> of read keys), range_reads (vector<RangeRead> - the phantom-tracker registration surface, validated at 5974-5979 and via on_reserve at 5995-5998). Control flow: txn.read(absent-k) -> rs_.insert(k) -> commit_txn -> ensure_index(k) creates KeyEntry + tree node -> validation passes -> commit succeeds -> the empty entry stays forever. Invariant violated: index size proportional to the live keyspace, not to the history of read keys (an unwritten but load-bearing expectation; GC's memory-boundedness test intent).

**Observable failure**

Loop { txn.get('absent-' + i); txn.put('w', v); txn.commit(); } - after ~17k iterations against a 1 MiB pool (default 256 MiB scales linearly: ~4.3M entries) the pool throws std::bad_alloc with one visible key. PoC (probe12): committed=17047, failed=0, exception=bad_alloc; visible keys: 1; the 17k empty entries are invisible to users AND to range_scan. Combined with CKV-003, the exhaustion event then converts into the total-loss chain.

**Contract / invariant violated**

Bounded index growth (implicit in GC's documented purpose - 'memory boundedness' is the name of the vacuous test that should have caught this). Not forbidden by an explicit README sentence - the fix preserves SSI semantics exactly while removing the side effect; no contract change required (see the equivalence argument below).

**Minimal safe fix**

Route never-existed-key reads through the phantom tracker instead of materializing entries. In ReadWriteTransaction::read (7951-7958): after resolving res = kv_.read_at(read_ts_, k), if the key is ABSENT at the snapshot AND has no KeyEntry at all (kv_.find_index(k) == nullptr - never existed), register the point range [k, k] into the transaction's range_reads instead of rs_ (range_reads.emplace_back(k, k, read_ts_)); otherwise (present at snapshot, or tombstoned - a KeyEntry exists) keep the existing rs_ route (ensure_index will find the existing entry, creating nothing). Equivalence argument (why SSI semantics are preserved exactly): the materialized-entry route detects a conflicting write via e->last_write_ts > read_ts under e->commit_mu; a write to a never-existed key k is an existence transition absent->present recorded in the phantom tracker at its cts (5999-6000, under batch_mu_ via on_reserve); has_phantom_in_range(k, k, read_ts) fires iff a to-exists transition was recorded at a cts above the snapshot - the same set of conflicting schedules, because cts order and phantom-record order are identical (the on_reserve critical section, 5994-6002 - the audit verified this ordering closure). Inclusive bounds [k, k] match point-read semantics exactly (the v20.1 inclusive-bounds fix applies). Self-conflict safety: a transaction that reads absent k and then writes k registers the point range AND writes k - the phantom pre-check at 5995-5997 runs BEFORE the transaction's own transitions are recorded at 5999-6000, so a transaction never conflicts with itself (the existing ordering, verified for range reads today). commit_txn's rs loop (5945-5948) is unchanged - it simply stops receiving never-existed keys. Existing databases: the empty entries are memory-only artifacts (the tree is rebuilt from WAL/checkpoint on open, materializing only replayed keys) - they vanish on restart; no migration, no on-disk impact.

**Exact implementation locations**

ReadWriteTransaction::read 7951-7958 (the routing decision); the read_observed variant (7960+, same routing for its rs bookkeeping - verified it shares rs_); ReadWriteTransaction's range-reads member (already exists for range_scan registration - the v24 Fix-12 machinery); commit_txn 5945-5948 (NO change - the loop naturally stops seeing never-existed keys). find_index 11843-11848 already exists as the non-creating probe (verified).

**Required regression test**

Name: negative-lookup-boundedness. Setup: Database with a small page_pool_bytes (1 MiB) and WAL; pool allocated-pages accessor (pool stats exist in the diagnostics surface - verify; if not exposed at Database level, use the test-only engine accessor the suite's page-pool self-tests use). Input: 50,000 iterations of { txn.read('absent-' + i); txn.put('w', i); txn.commit(); } - three times the PoC's exhaustion threshold. CURRENT COMMIT: expected FAIL - pool exhaustion (std::bad_alloc from put/commit) around iteration ~17k; allocated pages grows linearly with i (assert: bounded-by-constant fails). FIXED COMMIT: expected PASS - all 50,000 commits return OK; allocated pages stays bounded by a small constant (the single 'w' entry plus tree overhead - assert allocated(now) - allocated(after warmup) < 64 pages for 50k iterations); range_scan output is exactly the visible keys ('w' and nothing else); get('w') returns the last value; close under LSan leaks nothing. Conflict-semantics leg (equivalence proof): T1 reads absent k; T2 inserts k and commits; T1 commits -> Conflict (the phantom point-range fires - same outcome as the materialized route, asserted by a direct schedule test); T1 reads absent k, then T2 UPDATES an unrelated present key j, T1 commits writing j -> OK (no false conflicts from the routing). The test asserts the real invariant - index growth is bounded by the live keyspace while SSI conflict detection is byte-for-byte preserved - not merely 'no bad_alloc'.

**Additional affected paths**

Transaction (the only surface that builds an rs - verified: put/erase/Batch pass rs={}). ReadWriteTransaction::read and read_observed: both routed. range_scan inside a transaction: already registers range reads - unchanged. The lincheck mixed-API workloads: now exercise the point-range route for absent keys - their checkers validate the snapshot semantics (a bonus: the write-skew blindness note in the audit recommended exactly this shape of coverage). Async/batch/put: no rs - unaffected. GC: simply never sees the entries anymore; the retire counters for real keys are unchanged. Checkpoint: the dirty map never contained the empty entries (they were never written) - the CKV-003c coverage check is unaffected. Recovery: unchanged (replays real keys only).

**Compatibility impact**

Public API: none. Semantics: preserved exactly (the equivalence argument; the conflict leg proves it). On-disk / WAL / recovery: none - existing databases' in-memory empty entries vanish on restart either way. Performance: absent-key reads now perform a find_index probe (a tree descent - the same cost class as read_at's own lookup; read_at already descends) plus one RangeRead entry at commit; materialization cost (tree insert + allocation) is REMOVED - net neutral-to-positive. Memory: the unbounded growth is removed (the point of the fix). Concurrency: the phantom registration for point ranges uses the exact machinery range scans use today (reader_mu_ critical section via acquire_slot_with_phantom - the v24 Fix-12 protocol) - no new ordering.

**Regression risks**

(1) The routing must use find_index (never-existed) NOT read_at's nullopt (which conflates tombstoned with never-existed) - a tombstoned key HAS a KeyEntry whose last_write_ts must be validated by the rs route; routing tombstoned keys through the phantom tracker would MISS updates-after-tombstone (exists->exists transitions are not phantoms) and break SSI. The test's conflict-semantics leg must include a tombstone-update schedule to pin this. (2) read_observed must get the identical routing or the lincheck history records diverge from validation. (3) The ws.empty() early return at 5873 stays - read-only transactions never reach validation by design (their snapshot is the consistency point); do not 'fix' that too. (4) A transaction reading MANY distinct absent keys now registers many point ranges - the phantom tracker's per-range check is O(log n) per entry (verified) and the on_reserve re-check iterates range_reads linearly - same complexity class the range-scan path already pays; bound the test's universe so the battery stays fast. (5) The m16/m2-phase batteries and the lincheck suite must stay green - they exercise rs validation heavily.

---

### CKV-014 — Batch accepts duplicate keys and is silently consumed on failed commit

- **Severity:** Low
- **Status:** CONFIRMED
- **Wave:** 3

**Current defect**

Batch::put (8980-8985) appends entries without deduplication; the engine's duplicate-key rejection (commit_txn 5928-5934, InvalidTransaction) then rejects the WHOLE commit, and entries_.clear() at 9008 wipes the batch on every non-throwing return path - including Conflict / TooLarge / WalFailure / InvalidTransaction - so a failed batch cannot be retried either. ReadWriteTransaction::write dedups via std::map with last-wins; the two public write surfaces thus have different duplicate-key semantics, and neither is documented. A batch that fails via an exception, by contrast, is NOT cleared (the clear at 9008 is on the normal path only) - inconsistent retry semantics across failure modes.

**Root cause**

File chronokv.hpp. Batch::put 8980-8985 (push_back, no dedup); Batch::commit 8988-9042: entries_.clear() at 9008 executes unconditionally after commit_txn returns, before map_txn_result. Variables: entries_ (vector<Entry>). Control flow: put('k','v1'); put('k','v2'); commit() -> ws contains two 'k' entries -> commit_txn's new_keys.insert fails at 5933 -> InvalidTransaction -> entries_.clear() -> the caller has an empty batch and no commit. Invariant violated: API consistency between the two write surfaces (Transaction last-wins vs Batch all-or-nothing rejection), and the reasonable expectation that a failed submission leaves the submission retryable.

**Observable failure**

batch.put('k','v1'); batch.put('k','v2'); batch.commit() -> Status::InvalidTransaction, batch.size() == 0, nothing committed, the entered data is gone. Same silent consumption for an injected WalFailure: the batch is destroyed on a durability failure the caller is expected to retry.

**Contract / invariant violated**

Not forbidden by any documented contract - the README's Batch row says 'atomic write-set commit' and is silent on duplicates and consumption. This finding therefore REQUIRES a contract decision, which this specification makes: dedup last-wins (matching Transaction - one consistent semantic across both write surfaces), and consume-on-success-only (retryable failures). Both are documentation additions; the behavior change is from undefined to defined.

**Minimal safe fix**

(1) Dedup at put() time with last-wins: replace entries_ (vector<Entry>) with std::map<std::string, Entry> keyed by the entry key (assignment overwrites - last-wins); put() and erase() insert/assign; size() returns the map size; commit() iterates the map to build ws (order-independent: commit_txn locks by KeyEntry* address and the WAL sorts by cts - verified the write-set order is not semantically load-bearing). A put() followed by erase() of the same key correctly ends as the erase entry (map[k] = {deleted:true}). (2) Consume only on success: move entries_.clear() to after the status computation, executed ONLY when status == Status::OK; on Conflict / TooLarge / WalFailure / InvalidTransaction / Failed, the batch remains intact for retry or inspection; the exception path already leaves it intact - now uniform. (3) Document in the README's Batch row: duplicate keys collapse last-wins (same as Transaction); a failed commit leaves the batch intact; only a successful commit consumes it; clear() remains the explicit manual reset.

**Exact implementation locations**

Batch class 8976-9051: entries_ type change (900-905: the Entry struct stays); put 8980-8982 / erase 8984-8986 (map insert_or_assign); commit 8988-9042 (ws construction from the map at 9001-9005; the clear moves from 9008 to after 9009, gated on status == OK); size 9044; clear 9045; README Batch row.

**Required regression test**

Name: batch-dedup-and-retry. Setup: Database with WAL; fault hooks enabled for the WalFailure leg (CKV-018's WriteFail at the WAL). Input legs: (a) batch.put('k','v1'); batch.put('k','v2'); commit() -> CURRENT: FAIL (returns InvalidTransaction, size()==0, get('k') absent - the assertions expect OK and get('k')=='v2'); FIXED: PASS - commit returns OK, get('k') == 'v2', batch.size() == 0 (consumed on success). (b) put('a','1'); put('b','2'); arm WriteFail; commit() -> returns WalFailure; CURRENT: FAIL (batch.size() == 0 - silently consumed; retry impossible); FIXED: PASS - batch.size() == 2 (intact); disarm the fault; commit() again -> OK; both keys present. (c) put('k','v'); erase('k') in one batch -> commits; get('k') absent (the erase won the dedup). (d) a Conflict leg: two batches racing on one key from two threads - the loser returns Conflict and remains retryable (size preserved). Assertions: outcome semantics, durability matching the reported outcome after reopen, and size/consumption behavior - the real contract, not 'returns a Status'.

**Additional affected paths**

Batch::commit only. Transaction (the reference semantics - unchanged). put/erase/async: single-entry - no dedup question. Checkpoint / GC / recovery: consumers of committed state - unaffected. The engine's own duplicate rejection at 5928-5934 STAYS (defense in depth for the raw API - the v20.1 blocker history documented there).

**Compatibility impact**

Public API: behavior change on an undefined surface - duplicate-key batches now commit last-wins instead of failing InvalidTransaction; failed batches stay retryable. size() now reports distinct keys (a batch with duplicates shrinks at put() time - observable but sensible). On-disk / WAL / recovery: none. Performance: map vs vector for batch building - O(log n) insert vs amortized O(1); batches are small (the README's usage pattern); negligible. Memory: comparable (map nodes vs vector slots). Concurrency: none.

**Regression risks**

(1) Any test relying on InvalidTransaction for duplicate batches must be updated (grep the suite - the engine-level dup test at 5106-5117 tests the RAW API, unaffected). (2) The ws built from a map iterates in KEY order, not insertion order - commit_txn is order-insensitive (verified: locking by KeyEntry* address, WAL sorts by cts, transitions iterate the same witems) but the observer notification order for a batch changes to key order - the observer tests assert prefix matching, not order (verified) - confirm. (3) The txnrec Stage records iterate ws - key-ordered now; the checker folds by txn id (order-insensitive - verified). (4) clear() semantics unchanged (manual reset still available).

---

### CKV-015 — Abandoned Transaction after close() leaks its reader slot and phantom registration on a live engine

- **Severity:** Low
- **Status:** PARTIALLY CONFIRMED (mechanism verified; no runtime repro)
- **Wave:** 3

**Current defect**

~ReadWriteTransaction (7924-7929) skips release_slot / deregister_phantom_reader when engine_live() returns false. engine_live() consults the DATABASE liveness flag (the db_alive_ weak_ptr), which the v25.8 keepalive design decoupled from engine lifetime: when a transaction is abandoned after Database::close() but OTHER engine keepalive holders exist (a second transaction, a stream, an in-flight async op), the engine is still alive - yet the destructor skips the release, pinning the reader slot and phantom registration on a live engine until teardown. The pin holds the GC retirement floor down (deferred version reclamation, unbounded memory growth for the pinned window). In the single-keepalive case, member destruction order guarantees the engine is already gone when ~ReadWriteTransaction runs, so the skip is currently correct - the defect is the WRONG PREDICATE, exposed only in the narrow composition (close() + abandonment + another keepalive).

**Root cause**

File chronokv.hpp. ~ReadWriteTransaction 7924-7929 (the engine_live() gate); engine_live() (checks the db_alive_ weak_ptr - the Database flag, not the engine); Transaction's member layout (engine_keepalive_ declared at 9231; txn_ the unique_ptr<ReadWriteTransaction> - destruction order between them determines when the engine dies relative to ~RWT). Mechanism (member-order analysis, verified): ~Transaction destroys members in reverse declaration order; if txn_ is declared AFTER engine_keepalive_, ~RWT runs while engine_keepalive_ still holds the engine alive - but engine_live() says false (Database closed) - skip - leak on the live engine. Invariant violated: reader slots and phantom registrations are always paired with their acquisition (liveness, not safety - E1-E16 safety holds; the pin merely delays reclamation).

**Observable failure**

No runtime PoC was built for the audit (narrow window). The mechanism is certain from the member-declaration-order analysis: open db; t1 = begin; t2 = begin; close(); destroy t1 -> t1's slot and phantom registration stay pinned while the engine lives under t2's keepalive; GC's retirement floor stays at t1's snapshot epoch; retired_pending drains only after t2 dies and teardown runs.

**Contract / invariant violated**

Slot/phantom-registration pairing (liveness bookkeeping). Not a documented README contract; the v25.8 keepalive design's own intent (release resources against the ENGINE's lifetime) is the violated expectation - the fix realigns the predicate with that design.

**Minimal safe fix**

Release whenever the ENGINE is alive, regardless of Database closure. Concretely: ReadWriteTransaction gains its own engine keepalive member - std::shared_ptr<ChronoKV> engine_keepalive_ - declared FIRST in the class (destroyed LAST: the slot release in ~RWT then always runs against a live engine when one exists); the destructor's gate becomes 'if (engine_keepalive_)' (non-null = we hold a reference = the engine outlives this destructor call) instead of engine_live(); the constructors (7908, 7919) take/copy the engine shared_ptr from the caller. Transaction reorders its members so engine_keepalive_ (9231) is declared BEFORE txn_ (destroyed AFTER ~RWT completes - the engine cannot die mid-destructor). The db_alive_ weak_ptr member stays for the API-level lifecycle error paths (commit-after-close etc.) - only the DESTRUCTOR's predicate changes. Do NOT disable reclamation or add global pin sweeping - the fix is the predicate.

**Exact implementation locations**

ReadWriteTransaction: member block (new engine_keepalive_ member, declared first); constructors 7908-7921 (accept the shared_ptr); destructor 7924-7929 (gate on the member). Transaction: member declarations around 9223-9231 (order: engine_keepalive_ before txn_); the begin() call site that constructs the RWT (pass the engine shared_ptr it already holds). Database::begin - unchanged (it already operates under the keepalive contract).

**Required regression test**

Name: abandoned-txn-pin-release. Setup: Database with GC enabled; gc_stats()/epoch_stats() expose the oldest active pin epoch and retired_pending (verified in the diagnostics surface). Input: t1 = db.begin(); t2 = db.begin(); write and commit several versions of a key through a third handle; db.close(); destroy t1 (abandoned - aborts per the documented active-transaction contract... precisely: destroying an active Transaction is documented to std::abort - so the test aborts t1 explicitly if abort() is the sanctioned route, or lets the documented abandonment path run; either way ~RWT executes); assert the pin floor advances; destroy t2; assert retired_pending drains and the versions are reclaimed. CURRENT COMMIT: expected FAIL - after destroying t1 (post-close, engine alive under t2's keepalive), the oldest active pin epoch stays frozen at t1's snapshot (assert: floor advances after t1's death - fails); FIXED: expected PASS - the floor advances when t1 dies and reclamation proceeds. Single-keepalive control leg: one transaction, close, destroy - the engine dies cleanly, no use-after-free (TSan leg in the tsan job). Assertions: the pin bookkeeping advances at the right destruction points and nothing crashes - the liveness invariant, not 'no leak detected by LSan' (LSan cannot see a live-engine pin).

**Additional affected paths**

Transaction lifecycle only. Streams (own keepalive design, v25.7 review M2 - already correct: their keepalive_ is declared first at 11884-11889); async ops (engine copied into the lambda - correct); observers (outlive by design). GC: the beneficiary. Checkpoint / recovery / backup / PITR: unaffected (they hold their own slots via RAII).

**Compatibility impact**

Public API: none (internal member + constructor plumbing; the public Transaction API is unchanged). On-disk / WAL / recovery: none. Performance: one shared_ptr copy per transaction begin (an atomic increment - the keepalive design already pays this in streams/async). Memory: one shared_ptr per transaction. Concurrency: none - the release path runs under the same reader_mu_ discipline it always did; only its trigger condition widens from 'database-open' to 'engine-alive'.

**Regression risks**

(1) The single-keepalive case changes WHEN the release happens (at ~RWT under the still-live engine, instead of never-because-engine-dead): release_slot on a live engine is safe (the normal commit/abort path does it constantly) - but the destruction-order requirement (engine_keepalive_ declared FIRST in RWT, BEFORE the SnapshotGuard and Cursor members it protects - mirroring the stream state's layout at 11884-11896) is mandatory or the release touches a dying engine. (2) The TSan close-race battery (rank-1, 40 iters x 5 API modes) must stay green - it exercises exactly these destruction interleavings. (3) The documented active-transaction abandonment std::abort path (README) is unrelated to this destructor gate - do not conflate; the test uses the sanctioned abort route. (4) acquire_slot_with_phantom / release pairing is reader_mu_-serialized - no new ordering.

---

## 6. Wave 4 — PITR and Filesystem Behavior (CKV-010, 011)

Wave 4 enforces the documented PITR contracts: the destination guard for restore_pitr and the byte-exact read-only guarantee for PITR source opens.

### CKV-010 — restore_pitr into a non-empty destination silently merges stale foreign state

- **Severity:** Medium
- **Status:** CONFIRMED
- **Wave:** 4

**Current defect**

Database::restore_pitr (8682-8725) documents dest_dir as a fresh directory but never checks or cleans it: create_directories at 8702 runs unconditionally; the final Database::open at 8719-8724 points at dest_dir/wal which may still hold a PREVIOUS restore's WAL. When the stale records are cts-contiguous with the fresh export, they replay over it - writes made to the PREVIOUS restore silently appear in the NEW as-of state. When they are not contiguous, open throws a misleading CorruptionError ('interior gap ... WAL corruption detected') although nothing is corrupt - the destination was dirty.

**Root cause**

File chronokv.hpp. Function Database::restore_pitr 8682-8725: no destination emptiness validation between the argument checks (8687-8692) and create_directories (8702). Variables: dest_dir, dest_ckpt (dest_dir + '/ckpt'), the final open's o.wal_dir (dest_dir + '/wal'). Control flow (probe6): restore at as_of W into dest; write to the restored DB (commits land in dest/wal at cts > W); close; restore again at the same W into the same dest -> the second export's base is written; the final open replays dest/wal which still contains the first restore's post-W records - cts-contiguous with the export's watermark - foreign state merges. Invariant violated: the restore_pitr contract as documented at 8661-8670 (dest holds the source's state as of as_of_cts, fresh directory).

**Observable failure**

probe6 (H3e): 'STALE foreign write from previous restore leaks into as-of state' - the new restore contains keys written to the OLD restore. probe5: a second restore at a LOWER as_of into the same dirty dest terminates with CorruptionError('recovery failed: interior gap expected_ts=3 found_ts=4 - WAL corruption detected') - the operator is sent chasing WAL corruption that does not exist.

**Contract / invariant violated**

The restore_pitr contract (in-code documentation 8661-8670; README's restore_pitr row says 'materializes a writable as-of DB in a fresh directory'). The fix ENFORCES the documented contract - the destination guard is the contract's missing precondition check, plus one README sentence making the precondition explicit.

**Minimal safe fix**

At entry, after the existing argument checks (8692), require the destination to be empty or absent: if std::filesystem::exists(dest_dir) && !std::filesystem::is_empty(dest_dir), throw Error('restore_pitr: destination directory is not empty - pass a fresh directory (the as-of export must not merge with prior state); remove or rename the existing contents first'). The atomic-swap alternative (write to a temp sibling and rename over dest) is a larger change and was rejected as beyond the minimal fix - the loud refusal matches the function's existing error posture (it already throws LifecycleError/CorruptionError/Error for precondition violations). Update the README row: 'dest must be empty or nonexistent; restore_pitr refuses otherwise'.

**Exact implementation locations**

restore_pitr 8692-8702 (insert the guard after the src checks, before create_directories); README restore_pitr row.

**Required regression test**

Name: pitr-dirty-destination-refusal. Setup: a source database with several checkpoints and a mid-window as_of boundary W (the existing PITR test rig already builds these). Input legs: (a) restore at W into dest; put a 'post' key into the restored DB; close; restore again at W into the SAME dest. CURRENT COMMIT: expected FAIL - the second restore succeeds and the restored DB CONTAINS 'post' (assert: the as-of state equals a fresh-dest restore's state - the foreign key breaks equality; probe6's exact shape). (b) restore at a lower as_of into the same dirty dest. CURRENT: FAIL - a misleading CorruptionError is thrown (assert: the error names the destination, not WAL corruption). FIXED COMMIT: expected PASS - both legs throw the destination-not-empty Error with the remedy in the message; a control leg (fresh dest each time) produces exactly the source's as-of state, byte-diffed against the expected key/value set. Assertions: refusal semantics AND fresh-dest correctness - the actual contract.

**Additional affected paths**

restore_pitr only. backup()/verify_backup: unaffected. PITR read-only opens: unaffected. The export path (export_checkpoint_no_rotate): writes only the ckpt file into dest - unaffected. Recovery: the final open is the consumer whose inputs the guard sanitizes.

**Compatibility impact**

Public API: behavior change on an undefined surface - a dirty destination now throws instead of merging silently or mis-diagnosing; the error message carries the remedy. On-disk / WAL: none. Recovery: none. Performance: one directory-existence + emptiness check (std::filesystem::is_empty - a directory iteration that stops at the first entry) - negligible. Memory / concurrency: none.

**Regression risks**

(1) is_empty() on a directory containing only hidden/dot files returns false - correct behavior for this guard (any prior content refuses). (2) The guard must run BEFORE create_directories so an absent dest is created normally (the common case). (3) Existing PITR tests always pass fresh dests - verified, they stay green. (4) Do not auto-clean the destination - silent deletion of a previous restore is exactly the destructive behavior this finding exists to prevent.

---

### CKV-011 — A PITR open mutates the source directory: the README's read-only promise is false twice

- **Severity:** Medium
- **Status:** CONFIRMED
- **Wave:** 4

**Current defect**

README section 'PITR opens are read-only' states the open 'does not modify the source directory at all (not even stale-delta cleanup)'. In reality a PITR open performs two mutations: (1) the orphaned .tmp cleanup in recover_with_checkpoint (6712-6742) removes ckpt_path + '.tmp' and any '<base>.delta.<digits>.tmp' files it finds - NOT gated on the PITR mode (the stale-delta deletion at 6953 IS correctly gated on the as-of boundary, verified); (2) open_segment (3453) calls truncate_torn_tail before ::open, ftruncating the source's active WAL segment torn tail in the WalSegments constructor - unconditional for every open mode. Both are benign for the as-of view's correctness but break the documented non-destructive promise - which matters because PITR's documented purpose includes forensic inspection of a damaged directory, where every byte of evidence must survive the inspection.

**Root cause**

File chronokv.hpp. recover_with_checkpoint 6703-6742: the .tmp cleanup block was written for the normal-open path and never audited against the PITR promise (the v24 Fix 9 comment even celebrates its thoroughness); WalSegments::open_segment 3446-3464: '(void)truncate_torn_tail(p)' at 3453 runs before the O_WRONLY open - the repair is needed only for the APPEND path (the file must end at a frame boundary before appending); a PITR open never appends. Variables: pitr_as_of_ (the boundary; present in the engine), the torn-tail repair's file target. Invariant violated: P1 - PITR opens are read-only and non-destructive to the source (README + the in-code comment at 8666-8669, which itself only acknowledges the torn-tail repair - a third documentation inaccuracy folded into this fix's README update).

**Observable failure**

Plant ckpt.tmp and ckpt.delta.7.tmp files in a PITR source directory and append 9 garbage bytes to the active WAL segment; open with pitr_as_of_cts; all three mutations occur: both .tmp files are gone and the segment shrank (api_probe2: 83 -> 74 bytes - 'README violated' x3).

**Contract / invariant violated**

P1, the README's explicit read-only promise. The fix restores the documented contract; no contract change (the README already promises the fixed behavior).

**Minimal safe fix**

(1) Gate the .tmp cleanup on the open mode: wrap the block at 6712-6742 in 'if (!pitr_as_of_) { ... }' (the engine member is available in recover_with_checkpoint's scope - verify; if the flag lives only on Options, thread it through the recover call the same way pitr_as_of_ reaches the delta-deletion gate at 6953). (2) Defer torn-tail repair on PITR opens: open_segment gains the mode (or WalSegments gains a read_only_append_never flag set from Options when pitr_as_of_cts != 0) and skips truncate_torn_tail at 3453 - recovery's parser already tolerates a torn tail (TORN_TAIL status stops replay at the boundary - verified by wal_recover_buf's valid-after probe), and a PITR open never appends, so the repair is unnecessary; the file is opened O_WRONLY (unchanged fd semantics - or O_RDONLY for cleanliness, but that changes more surface - keep O_WRONLY, the deferral alone satisfies read-only). (3) Update the in-code comment at 8666-8669 (drop 'the only source mutation is the torn-tail repair') and the README (no change needed - the README already claims the fixed behavior; optionally note the guarantee is byte-exact).

**Exact implementation locations**

recover_with_checkpoint 6712-6742 (the gate); WalSegments constructor/open_segment 3446-3464 (the flag + skip); Options-to-WalSegments plumbing (one bool); the comment at 8666-8669; README PITR section (one clarifying sentence).

**Required regression test**

Name: pitr-source-byte-identity. Setup: build a PITR source directory; plant ckpt.tmp, ckpt.delta.7.tmp; append 9 garbage bytes to the active segment (a torn tail); snapshot a recursive hash of every file (path, size, content hash) BEFORE the open. Input: open with Options::pitr_as_of_cts at a valid boundary; read some keys; close. CURRENT COMMIT: expected FAIL - the post-open hash differs (the two .tmp files are gone; the segment is 9 bytes shorter - assert: the directory tree and byte contents are identical - fails on three files). FIXED COMMIT: expected PASS - byte-identical source, keys read correctly, the as-of view is correct (the existing PITR boundary battery stays green). Control leg: a NORMAL open still performs both mutations (the cleanup and repair are correct for writable opens - assert they still fire). Assertions: the actual P1 invariant - byte identity - not 'no error thrown'.

**Additional affected paths**

PITR opens (the target). Normal opens: unchanged (both mutations still fire - the control leg). restore_pitr: opens the source read-only in step 1 - now genuinely read-only (its dest writes are its own). Crash-fuzz / recovery batteries: normal opens - unaffected. Backup verify: read-only already - unaffected. The torn-tail repair machinery itself: unchanged, merely deferred per mode.

**Compatibility impact**

Public API: none (a mode-internal flag). On-disk: none. WAL: none (a torn tail in a PITR source now SURVIVES - previously truncated; recovery of the same directory in normal mode still repairs it - idempotent by design, verified). Recovery: unchanged semantics. Performance: none. Memory / concurrency: none.

**Regression risks**

(1) The WalSegments constructor is shared by every open - the flag must default to repair-on (only PITR opts out) or normal recovery changes behavior. (2) Skipping the repair on a PITR open leaves the active fd positioned past a torn tail - harmless because no append ever happens, but verify lseek/batch paths are never reached on a read-only instance (writes are refused at 5880 - verified). (3) The .tmp gating must not also gate the STALE-DELTA deletion at 6953 (already correctly gated on as_of - leave it). (4) The existing PITR non-destructiveness tests (future deltas/records untouched) must stay green - they assert the WAL records, not the .tmp files (verified - the new test covers the gap).

---

## 7. Wave 5 — Lower-Severity Correctness and Documentation (CKV-008, 013, 017, 019, 020, 021)

Wave 5 handles the latent concurrency-contract break (classified as a standalone-BTree defect, not a production bug), the dead io_uring registered-files trap, two documentation corrections, and two high-confidence code-read findings. CKV-008 receives the full specification format; the documentation-only items are compact by nature.

### CKV-008 — fence_unchanged misses count-preserving splits: the BTree OLC contract is broken for concurrent standalone use (latent in-engine)

- **Severity:** Medium (High for standalone BTree use)
- **Status:** CONFIRMED (latent in-engine)
- **Wave:** 5

**Current defect**

The optimistic-lock-coupling retry validates that a leaf did not change during the shared-to-exclusive latch gap by comparing (key_count, min_key, max_key) before and after (fence_unchanged 11683-11693). A split whose left half preserves all three fields passes the re-check - which happens whenever the leaf held a single entry (count 1 -> 1, min = max = the sole key) or was empty. The retrying writer then mutates a stale page snapshot: it inserts into the left leaf although the separator already routes keys at/above the split key to the new right sibling - a key present in the tree but unreachable by descent, a disordered leaf chain, and separator-invariant violations. CLASSIFICATION per this remediation's rules: in the SHIPPED ENGINE this is masked - every tree mutation is serialized under nm_ (ensure_index 11829) and tree_->erase / put_with_old have no callers (verified by grep this session) - so it is a LATENT / STANDALONE-BTree defect, not a production Critical/High. The BTree's own documented contract ('the fence re-check below is the guard', 11632-11636) is false as written.

**Root cause**

File chronokv.hpp. fence_unchanged 11683-11693 (the triple comparison); find_leaf_crabbing_with_fence 11642+ (the fence capture); the gap + re-check at 10958-10969 (put path) and 11767-11773 (erase path). Variables: LeafFence {min_key, max_key, key_count} - a content fingerprint, not a version. Control flow (the deterministic schedule from poc3): T1 captures fence {1, K1, K1} on leaf L under shared latch; releases; T2 splits L into L{K1} + R{K2} (its key did not fit); L's fence is STILL {1, K1, K1}; T1 acquires exclusive; fence_unchanged -> true; T1 inserts K3 into L; the parent routes >= K2 to R; get(K3) by descent -> not found; the chain is out of order. Invariant violated: OLC validation soundness - the re-check must reject any concurrent structural change it raced with.

**Observable failure**

poc3, deterministic (the repo's own dst scheduler at the code's own tree_leaf_latch_gap stress point, seed 12, 3/3 runs): put(K3) returns true, get(K3) returns false; verify_leaf_chain reports 'leaf chain out of order'; verify_separator_invariants reports 'leaf max key K3 >= parent's upper bound K2'; full scan order is a, c, b. The erase path has the same hole (returns not-found for a key that moved to the sibling). UNREACHABLE in-engine today (nm_ serialization) - no production symptom exists.

**Contract / invariant violated**

The BTree's in-code OLC contract (the fence comment at 11632-11636 asserts the re-check is the guard). No README contract covers standalone BTree concurrency. Per the classification rules: preserve as a latent/standalone defect; the fix restores the BTree's own documented contract without touching the engine's serialization.

**Minimal safe fix**

Add a per-page mutation epoch and compare it in the retry. PageHeader is memory-only (the tree is never serialized - the checkpoint stores key/value pairs, not pages; verified), so the layout may change freely: repurpose the reserved fields to add uint32_t mutation_epoch to PageHeader (reserved2 2B + reserved3 2B are adjacent at offsets 8-11 - combine into one 4-byte field; keep sizeof(PageHeader) == 32 via the existing static_assert). Bump mutation_epoch in every function that mutates a page's logical content: put_leaf_nolatch (on success), split_leaf (both halves - the rewrite), insert_into_leaf_no_split (it only runs under the split's exclusive latch - bump there or rely on split_leaf's bump), insert_into_interior_nolatch, insert_into_interior_no_split, split_interior (both halves), the two root-split writers, and the erase mutation paths. All mutations hold the page's exclusive latch (verified across the tree code) - the epoch is a plain uint32_t under that latch, read under shared - no atomics needed. Extend LeafFence with the epoch (captured under the shared latch in find_leaf_crabbing_with_fence); fence_unchanged compares epoch equality FIRST (keep the triple as belt-and-braces or drop it - keep it, free). Wraparound: 2^32 mutations of ONE page cannot alias within any realistic process lifetime, and the comparison is equality (a wrap aliases only if the SAME page mutates exactly 2^32 times between the fence capture and the re-check - a window measured in microseconds; accepted, document the reasoning at the field).

**Exact implementation locations**

PageHeader 9742-9756 (field re-layout + static_assert); LeafFence 11637-11641 (add epoch); find_leaf_crabbing_with_fence 11642+ (capture); fence_unchanged 11683-11693 (compare); every mutation site enumerated above (the bumps). No engine-side change whatsoever.

**Required regression test**

Name: btree-concurrent-putters (standalone BTree, dst-scheduled). Setup: standalone BTree over a PagePool with the dst scheduler driving N >= 2 concurrent putters over overlapping key sets through the tree_leaf_latch_gap stress point (the poc3 rig already does this for 2 writers - promote it into the suite as a stress-build test beside the existing dst scenarios). Input: 2-8 threads x 1,000 puts over a 500-key universe with sizes that force splits mid-race. CURRENT COMMIT: expected FAIL - the poc3 shape (seeded): put returns true then get returns false for a raced key; verify_leaf_chain reports disorder (assert: every key reachable + chain ordered - fails). FIXED COMMIT: expected PASS - all keys reachable via get, verify_leaf_chain / verify_separator_invariants green, scan order strictly ascending. TSan leg: run the same test under the tsan job (the first-ever concurrent exercise of the retry path - it may surface the races the audit predicted were unobservable; treat new TSan reports as part of this fix's scope). Assertions: the real OLC invariant - no writer ever mutates a page the separators no longer route to.

**Additional affected paths**

put / erase retry paths (the fix target). The engine: ZERO change (nm_ serialization untouched - the epoch is transparent to it). Cursor paths: read-only - they capture no fence. get: no fence - unaffected. The fence_recheck_retries_ counter: still bumped on every re-descent (now including split-triggered ones - the counter becomes MEANINGFUL; its stress assertions may need loosening - verify).

**Compatibility impact**

Public API: none. On-disk: none (PageHeader is memory-only - verified; the checkpoint/WAL formats never see it). Performance: one uint32 bump per mutation (an add to a cacheline already being written) and one comparison per retry-check - negligible. Memory: none (repurposed reserved bytes). Concurrency: the OLC contract becomes sound; the engine's serialized path is unaffected.

**Regression risks**

(1) The re-layout must keep sizeof(PageHeader) == 32 (the static_assert enforces it - adjust the field order carefully: is_leaf(1) + reserved[3] + key_count(2) + mutation_epoch(4, from reserved2+reserved3's 4 bytes at offsets 8-11... verify offsets: reserved2 is at offset 8 (2B), then next_leaf_id at 12 - the 4 bytes 8-11 are reserved2(2)+pad... recompute exactly at implementation time from the #pragma pack(1) struct - the assert is the safety net). (2) EVERY mutation site must bump - a missed site silently degrades to the old fence semantics (the triple remains as backstop); the concurrent-putters test with varied key shapes is the detector. (3) The stress builds' fence_recheck_retries_ assertions (the m16 battery) count retries - splits now add retries in the standalone test only (the engine is serialized - zero change there). (4) Do NOT wire tree_->erase or hazard-pointer reclamation as part of this fix - those are separate roadmap items; the epoch only repairs the validation predicate.

---

### CKV-013 — io_uring registered-files feature is dead code with a data-destruction trap if ever enabled

- **Severity:** Low (latent Critical if enabled)
- **Status:** CONFIRMED
- **Wave:** 5

**Current defect**

fixed_files_ is initialized false (2663) and never set true anywhere in the header (verified by grep: the only assignments set it false); ensure_file_registered (2865-2874) is only called under 'if (fixed_files_ && ...)' (2517) - so the registered-files feature advertised by the constructor comment and describe() ('fixed_files=lazy') never activates; every SQE uses a plain fd. Benign today (no perf win, misleading diagnostics). The latent trap: ensure_file_registered decides 'already registered' by comparing the fd NUMBER. Segment rotation closes the old fd and opens the new segment, and the kernel typically reuses the number (poc5a: closed fd 4, new segment got fd 4). Registered files pin the old struct file - enabling the feature as designed would write every post-rotation WAL batch into the sealed OLD segment at the new segment's offsets, with full-success CQEs, destroying the WAL. A one-line 'fix' of the dead flag activates this.

**Root cause**

File chronokv.hpp. IoUring wrapper: fixed_files_ (2663, dead flag), ensure_file_registered 2865-2874 (fd-number identity check), the guarded call site 2517. No activation site exists. Invariant violated (if enabled): SQE targets must reference the intended file across fd reuse.

**Observable failure**

None today (dead code). poc5a proves the premise: the fd-reuse after rotation is live on this kernel, and the ring's feature summary prints fixed_files=lazy - a diagnostic that implies a behavior that does not exist.

**Contract / invariant violated**

None active (the feature is unreachable). The defect is dead code + a misleading diagnostic + a documented trap. The fix removes rather than repairs - activating the feature is a performance project out of this remediation's scope (and would require identity tracking by inode/dev plus unregister-on-rotation).

**Minimal safe fix**

Remove the dead machinery: delete fixed_files_, ensure_file_registered, their call sites (2517's guarded branch), and the 'fixed_files=lazy' token from describe() (and the constructor-ladder comments referencing it). This eliminates the one-line-activation trap. The removal is of private, unreachable code - zero behavior change (verified: no activation site exists). Alternative (implement identity tracking) rejected as a feature implementation, not a defect fix. Record the trap in the commit message so the ROADMAP's io_uring notes reference it if the feature is ever revived.

**Exact implementation locations**

IoUring class: 2663 (member), 2517-2518 (guarded call), 2865-2874 (function), describe()'s output line, constructor comments referencing lazy registration.

**Required regression test**

Name: iouring-describe-honesty + existing io_uring E2E. The removal needs no new functional test (unreachable code); the regression proof is: (a) the existing real-kernel io_uring E2E tests stay green (plain-fd SQEs - the only path that ever ran); (b) grep-assertion in the self-test suite that fixed_files no longer appears in the header (a one-line source-level guard the project's hygiene style favors - optional). CURRENT/FIXED: the existing tests pass both before and after (the removed code was dead); the fail-first property is carried by the REMOVAL itself being verified unreachable (the audit's grep + poc5a) rather than by a runtime test - document this explicitly in the commit message.

**Additional affected paths**

None - the removed code is unreachable by construction. io_uring write/fsync chains, rotation, recovery: all use plain fds today (verified) - unchanged.

**Compatibility impact**

None. Public API: none. Diagnostics: describe() output changes (drops a dead token - an honesty improvement). Performance: none (the feature never ran). On-disk/WAL: none.

**Regression risks**

(1) The deletion must not disturb the constructor ladder's probe logic (the IORING_SETUP_PARAMS probing is separate - verified). (2) describe()'s consumers (the io_uring E2E tests may parse the output - grep the suite for 'fixed_files' and update any parser). (3) Nothing else - dead code removal with a grep-verified zero-caller property.

---

### CKV-017 — README overstates recovery validation: cross-segment LSN contiguity is not checked

- **Severity:** Low
- **Status:** CONFIRMED (documentation)
- **Wave:** 5

**Current defect**

README line ~51 claims 'strict LSN gap/duplicate detection on recovery'. The check exists WITHIN a segment only (wal_recover_buf 2226); across segments, recover_all never compares LSNs - a hand-built directory whose segment 1 holds LSNs 1-3 and segment 2 holds LSNs 100-101 with a valid MANIFEST opens successfully (PoC-verified). The engine's real cross-segment gate is cts contiguity (recover / recover_with_checkpoint) - the meaningful one, since LSN is physical write order and checkpoint rotation legitimately deletes segments (making cross-segment LSN validation impossible in general).

**Root cause**

Documentation overstatement; no engine defect. The doc-block at 2113 correctly calls the LSN a physical sequence - the README sentence simply claims more than the code does.

**Observable failure**

The lsn_gap PoC: recover_all returns OK; Database::open succeeds on a WAL with a cross-segment LSN gap. Correct behavior (the gap is tolerated by design) - only the claim is wrong.

**Contract / invariant violated**

README accuracy (D1-adjacent claims). Fix: qualify the sentence.

**Minimal safe fix**

README edit: 'strict LSN gap/duplicate detection within each segment on recovery; commit-timestamp contiguity is enforced across segments (the semantic ordering gate)'. Optionally mirror one sentence in the WAL doc-block at 2113. No engine change; the optional segment-boundary monotonicity assert (first LSN of each surviving segment exceeds the previous segment's max) was considered and REJECTED: rotation deletion makes it unenforceable in general, exactly as the audit noted.

**Exact implementation locations**

README.md line ~51; optionally chronokv.hpp 2113.

**Required regression test**

None (documentation-only). The existing recovery battery stays green unchanged.

**Additional affected paths**

None.

**Compatibility impact**

None (a doc sentence).

**Regression risks**

None beyond wording accuracy.

---

### CKV-019 — find_max_lsn_in_segment returns 0 for a CORRUPT segment: append-only mode re-seeds LSNs inside a corrupt file

- **Severity:** Low
- **Status:** PARTIALLY CONFIRMED (code-read; not reproduced)
- **Wave:** 5

**Current defect**

On an append-only open (recover_on_open=false) of a directory whose active segment is CORRUPT, find_max_lsn_in_segment (3403-3444) returns 0 at 3406 ('if (status == WalStatus::CORRUPT || records.empty()) return 0'), so lsn_ restarts at 1 inside a file that already contains those LSNs. A later recovery of that directory then reports duplicate-LSN CORRUPT (the within-segment check) - the append-only mode of the SAME version writes a file its own recovery mode rejects. Full recovery rejects the corrupt segment anyway, so the impact is confined to the append-only/replication-spike configuration.

**Root cause**

CORRUPT is treated identically to empty/absent for LSN seeding: the return-0 conflates 'no records parsed' with 'parse failed'. Variables: status (WalStatus), max_lsn, lsn_. Callers: the WalSegments constructor's seeding path.

**Observable failure**

Not reproduced (constructable by hand-corrupting the active segment). The mechanism is direct code reading: the 3406 conflation.

**Contract / invariant violated**

LSN uniqueness within a segment (the invariant the within-segment recovery check enforces). Not README-documented for the append-only mode; the fix makes the two modes consistent.

**Minimal safe fix**

Distinguish the statuses: change find_max_lsn_in_segment to signal corruption (std::optional<uint64_t> return - nullopt on CORRUPT; 0 only for genuinely empty/absent), and in the WalSegments constructor's seeding path: if the active segment is corrupt AND recover_on_open is false (the append-only mode - it is exactly the mode that calls this seeding), throw std::runtime_error('append-only open refused: active WAL segment is corrupt') at construction - loud refusal instead of silent re-seeding. Full-recovery opens unchanged (they reject the segment through recover_all as today).

**Exact implementation locations**

find_max_lsn_in_segment 3403-3444 (signature + the 3406 branch); the constructor's LSN-seeding call site (grep the caller of find_max_lsn_in_segment - the ctor at 3584-3656).

**Required regression test**

Name: append-only-corrupt-refusal. Setup: a valid WAL directory; corrupt the active segment's interior (flip bytes in a middle frame's CRC region). Input: open with recover_on_open=false. CURRENT COMMIT: expected FAIL - the open succeeds (assert: it must throw - the current code returns and re-seeds; a subsequent append + reopen-with-recovery then fails with duplicate-LSN corruption - the full failure chain as the assertion). FIXED COMMIT: expected PASS - the append-only open throws the refusal; the same directory opened WITH recovery still rejects loudly (unchanged); a control leg (valid segment, recover_on_open=false) still opens and appends with correct LSN continuation. Assertions: the invariant - an append-only open never writes LSNs that duplicate existing frames.

**Additional affected paths**

The append-only configuration only. Normal opens, recovery, PITR, backup: unaffected (they do not take the seeding path on corrupt segments).

**Compatibility impact**

Public API: none (an exception on a previously-undefined corrupt-state open). On-disk/WAL: none. Recovery: none. Performance: none. The replication-spike use case (append-only opens against healthy WALs): unchanged.

**Regression risks**

(1) The nullopt/0 distinction must preserve the genuinely-empty case (a fresh segment with no frames returns 0 and seeds from 1 - correct; verified the fresh-database flow depends on it). (2) TORN_TAIL status on the active segment: the append path's truncate_torn_tail repair (3453) already handles it before the seeding question - verify the ordering (seed from the repaired parse, not the pre-repair status). (3) The constructor throw must happen before any fd is leaked (the open_segment fd - close on the throw path).

---

### CKV-020 — maybe_rotate_segment: failed checked_close sets active_fd_ = -1 while the fd may remain open

- **Severity:** Low
- **Status:** PARTIALLY CONFIRMED (code-read; terminal-state only)
- **Wave:** 5

**Current defect**

In maybe_rotate_segment, if checked_close fails (EINTR-exhausted close) at 3496, the code sets active_fd_ = -1 and treats the segment as closed, but the descriptor may still be open - a leak. The instance is in the failed_ terminal state at that point (rotation failure fail-stops, the same line), so availability impact is nil; the leaked fd persists until process exit.

**Root cause**

The close-failure path conflates 'cannot close' with 'closed'. Variables: active_fd_, failed_.

**Observable failure**

Not reproduced (requires close-failure injection; close rarely fails). Terminal-state-only consequence.

**Contract / invariant violated**

Descriptor hygiene (unwritten, minor).

**Minimal safe fix**

Per the audit's direction: emit a diagnostic carrying the exact fd and errno when checked_close fails ('WAL rotation: close(fd=%d) failed errno=%d - descriptor may leak until process exit; instance is fail-stopped') and leave the state transition as-is (failed_ is already latched - correct). Optionally loop the close once more (EINTR retry is already inside checked_close - verified - so the loop adds nothing; the diagnostic is the fix). No functional change.

**Exact implementation locations**

maybe_rotate_segment 3496 (the failure branch - add the diagnostic line).

**Required regression test**

None (terminal-state-only consequence; no close-failure injection exists and adding one for a nil-impact fd leak is out of proportion - documented decision). If a CloseFail fault kind is ever added, the test would assert /proc/self/fd count stays bounded - noted for the ROADMAP, not this remediation.

**Additional affected paths**

None beyond the rotation terminal state.

**Compatibility impact**

None (a diagnostic line).

**Regression risks**

None of consequence. Do not 'fix' by retrying close indefinitely - checked_close already exhausted EINTR.

---

### CKV-021 — Stale safety comment: commit_locks are described as released before the publication barrier but are held through it

- **Severity:** Low
- **Status:** CONFIRMED (documentation)
- **Wave:** 5

**Current defect**

The v26.3 header comment at 100-108 (from commit 6d8a13d) states the publication barrier waits 'with the per-key commit mutexes RELEASED (commit_locks RAII scope ends before before_publish)'. In the code, commit_locks is declared at function scope (5963) and destroyed only at commit_txn's return - AFTER pub_.await_published(cts) at 6120. The comment is factually wrong. This is NOT a deadlock and has NO runtime effect: the deadlock-freedom argument survives either way (every cts holder acquired checkpoint_mu_ shared before reserving; lower-cts committers never need locks the waiter holds), TSan is clean either way, and holding the commit mutexes through the barrier is arguably intentional (it keeps a committing key's validation window closed until publication). But an auditor relying on the comment would mis-model the lock graph - exactly the drift the project's own hygiene notes warn about.

**Root cause**

The comment was written against an earlier iteration where the locks were scoped to the validation block.

**Observable failure**

None (documentation). Verified by direct reading this session: locks at 5963, barrier at 6120, function-scoped RAII.

**Contract / invariant violated**

Comment-code consistency for lock-graph reasoning. Per the classification rules: documentation-only - there is NO synchronization consequence (no schedule exists that differs; the mutexes' extra hold window blocks only same-key committers, which the barrier's own ordering already serializes).

**Minimal safe fix**

Correct the comment at 100-108 to state the truth: the per-key commit mutexes are HELD through pub_.await_published (function-scope RAII at 5963; released at commit_txn's return after the barrier), and WHY that is safe (the deadlock-freedom argument above) and WHY it is desirable (the validation window stays closed until publication). Do NOT introduce an inner scope to make the code match the comment - that would change the concurrency behavior the comment mis-described, in a remediation whose rules forbid unrequired changes.

**Exact implementation locations**

chronokv.hpp 100-108 (the comment block).

**Required regression test**

None (documentation-only). The TSan job's existing commit-concurrency coverage re-validates that the described (actual) lock graph is clean.

**Additional affected paths**

None.

**Compatibility impact**

None.

**Regression risks**

None.

---

## 8. Consolidated Regression-Test Plan

Every fix ships with a fail-first regression test that genuinely fails on commit 8c77a7c and passes after the fix. The table below consolidates the per-finding tests specified above into the suite-level plan; the assertions column names the actual invariant asserted (never 'exits 0', never 'no crash', never 'returns a status').

| Test name | Finding | CURRENT / FIXED | Invariant asserted |
|---|---|---|---|
| tree-oversized-key-rejection | CKV-001 | FAIL / PASS | Keys >= 4045 B rejected TooLarge with zero mutation; the 4044 B boundary key round-trips; tree verifiers green (ASan) |
| btree-byte-aware-split | CKV-002 | FAIL / PASS | Mixed-size workload completes with every inserted key retrievable; both split halves provably fit; the no-valid-split corner throws with the tree unchanged (ASan) |
| oom-checkpoint-refusal (3 legs) | CKV-003 | FAIL / PASS | After a mid-split allocation failure: acknowledged keys readable via get AND range_scan; latched engine refuses writes and checkpoints; after reopen every acknowledged key present (the WAL was never rotated away) |
| checkpoint-vs-wal-differential | CKV-003c | PASS / PASS (guard) | After a normal checkpoint, the recovered state equals the WAL-only replay - the assert that would have caught the corrupted-checkpoint class |
| sentinel-key-visibility | CKV-005 | FAIL / PASS | 255/256/300-byte 0xFF keys survive checkpoint+reopen byte-exact; GC retires their old versions; LSan clean at close |
| d2-async-write-stage | CKV-004 (+018) | FAIL / PASS | Every key whose commit returned WalFailure is absent after reopen; every present key was acknowledged; the debit fires only for fsync-stage async losses; sync control 0 resurrections |
| fault-inj: short write on the WAL path (de-vacuated) | CKV-018 | FAIL / PASS | The armed charge fires (fault::remaining() == 0); the short write is retried and the committed key reads back; WriteFail yields WalFailure and absence; deleting the retry loop makes the test fail |
| wal-reserve-burn-on-throw | CKV-012 | FAIL / PASS | The failing committer receives WalFailure; a subsequent commit completes within a bounded timeout (the publication prefix advanced past the burned hole) |
| stream-tombstone-differential | CKV-006 | FAIL / PASS | RangeScanStream output == vector range_scan output == the snapshot's visible set, including tombstone-first and all-tombstone ranges |
| observer-exception-matrix (6 legs) | CKV-007 (+016) | FAIL / PASS | Reported outcome == durability after reopen on every write surface; notification completes; async futures yield Status, never rethrow |
| negative-lookup-boundedness | CKV-009 | FAIL / PASS | 50,000 negative-lookup transactions leave allocated pages bounded by a constant; range_scan shows only the live keys; SSI conflict semantics byte-for-byte preserved (incl. the tombstone-update schedule) |
| batch-dedup-and-retry | CKV-014 | FAIL / PASS | Duplicate-key batches commit last-wins; a WalFailure/Conflict commit leaves the batch intact and retryable; consumption only on success |
| abandoned-txn-pin-release | CKV-015 | FAIL / PASS | After close() with another keepalive alive, the abandoned transaction's death advances the GC pin floor; retired_pending drains; single-keepalive control clean under TSan |
| pitr-dirty-destination-refusal | CKV-010 | FAIL / PASS | A non-empty destination is refused loudly with the remedy; fresh-dest restores produce exactly the source's as-of state |
| pitr-source-byte-identity | CKV-011 | FAIL / PASS | The source directory is byte-identical after a PITR open (planted .tmp files and torn tail survive); normal opens still repair |
| btree-concurrent-putters (dst + TSan) | CKV-008 | FAIL / PASS | Concurrent standalone-BTree putters: every key reachable, chain ordered, separator invariants green - the OLC contract holds |
| append-only-corrupt-refusal | CKV-019 | FAIL / PASS | An append-only open of a corrupt active segment throws instead of re-seeding LSNs; valid append-only opens continue LSNs correctly |
| iouring-describe-honesty + E2E | CKV-013 | PASS / PASS | Existing real-kernel io_uring tests stay green; the dead flag is gone (the unreachable-code proof is the audit's grep + poc5a, documented in the commit) |
| (doc-only: README + comment corrections) | CKV-017 / 020 / 021 | n/a | Wording matches behavior; no runtime test |

Suite integration notes: the tree-level tests join the existing self-test battery under the same hooks-on gates; the ASan/LSan legs run in the asan+ubsan job; the TSan legs in the tsan job; the dst-scheduled concurrent-putters test runs in the stress configuration beside the existing S1-S4 scenarios. Every CURRENT-FAIL entry was verified failing by execution or by the mutation argument stated in its finding's specification.

---

## 9. Invariant-Preservation Matrix

Each fix must preserve the engine's existing invariants. The matrix maps every Wave 1-5 fix against the audit's invariant inventory; 'preserved' means the fix's tests exercise the invariant, 'strengthened' means the fix adds an enforcement that did not exist, 'unchanged' means no interaction. No fix changes any invariant's definition - CKV-003c adds enforcement of CKPT-C rather than redefining it, and no new canonical invariant IDs are introduced (the two candidate new invariants - index fail-stop and publication progress - are enforcement of existing design intent, noted below).

| Fix | I1 chains | I2/I3 WAL replay + cts | R1 publication | D1 MANIFEST | D2 rollback | D3 fail-stop | R-REBASE rotation |
|---|---|---|---|---|---|---|---|
| CKV-001 gate+guards | preserved | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-002 byte-aware split | preserved | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-003a pre-alloc | preserved | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-003b index latch | preserved | unchanged | preserved | unchanged | unchanged | STRENGTHENED (index-side analogue) | preserved |
| CKV-003c ckpt cross-val | preserved | preserved | unchanged | unchanged | unchanged | unchanged | STRENGTHENED (refusal) |
| CKV-005 scan_all | preserved | unchanged | unchanged | unchanged | unchanged | unchanged | preserved |
| CKV-004 written gate | preserved | STRENGTHENED (fewer replays, same contiguity) | preserved | unchanged | STRENGTHENED (restored) | preserved | preserved |
| CKV-018 fault hooks | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-012 burn-on-throw | unchanged | unchanged | STRENGTHENED (progress) | unchanged | unchanged | unchanged | unchanged |
| CKV-006 stream skip | preserved | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-007/016 observer boundary | preserved | unchanged | unchanged | unchanged | preserved (client face) | unchanged | unchanged |
| CKV-009 phantom routing | preserved | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-014 batch dedup | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-015 slot release | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-010 dest guard | unchanged | preserved | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-011 PITR gating | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-008 page epoch | preserved | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-013/017/019/020/021 | unchanged | CKV-019: preserved (consistency) | unchanged | unchanged | unchanged | unchanged | unchanged |

| Fix | T1 read-your-writes | B1 backup | P1 PITR read-only | E1-E16 reclamation | PAGE bounding | CKPT-C completeness | OLC fence |
|---|---|---|---|---|---|---|---|
| CKV-001 gate+guards | preserved | unchanged | unchanged | unchanged | STRENGTHENED (enforced) | preserved | unchanged |
| CKV-002 byte-aware split | preserved | unchanged | unchanged | unchanged | STRENGTHENED | preserved | unchanged |
| CKV-003a pre-alloc | unchanged | unchanged | unchanged | unchanged | STRENGTHENED | preserved | unchanged |
| CKV-003b index latch | unchanged | unchanged | unchanged | unchanged | unchanged | preserved | unchanged |
| CKV-003c ckpt cross-val | unchanged | unchanged | unchanged | unchanged | unchanged | STRENGTHENED (enforced) | unchanged |
| CKV-005 scan_all | unchanged | unchanged | unchanged | preserved | unchanged | STRENGTHENED (enforced) | unchanged |
| CKV-004 written gate | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-018 fault hooks | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-012 burn-on-throw | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-006 stream skip | preserved | unchanged | unchanged | unchanged | unchanged | preserved (scan face) | unchanged |
| CKV-007/016 observer boundary | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-009 phantom routing | preserved | unchanged | unchanged | preserved | unchanged | unchanged | unchanged |
| CKV-014 batch dedup | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| CKV-015 slot release | unchanged | unchanged | unchanged | STRENGTHENED (pin liveness) | unchanged | unchanged | unchanged |
| CKV-010 dest guard | unchanged | unchanged | unchanged | unchanged | unchanged | preserved (restore face) | unchanged |
| CKV-011 PITR gating | unchanged | unchanged | unchanged | STRENGTHENED (enforced) | unchanged | unchanged | unchanged |
| CKV-008 page epoch | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | STRENGTHENED (sound) |
| CKV-013/017/019/020/021 | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |

Legend: preserved = the fix's tests exercise the invariant; strengthened = the fix adds an enforcement that did not exist; unchanged = no interaction.

- SSI behavior (the rw anti-dependency validation under commit_mu and the on_reserve phantom ordering) is preserved by every fix that touches commit_txn: CKV-001 and CKV-009's tests carry explicit conflict-semantics legs proving equivalence; CKV-012 wraps the reservation window without changing the on_reserve critical section's position (cts order == phantom order is untouched).
- Close/destruction safety: CKV-015 reorders members but keeps the v25.8 keepalive contract intact (the TSan close-race battery is the gate); every other fix leaves lifecycle untouched.
- The two enforcement additions (index fail-stop, publication progress) are the audit's identified missing analogues of D3 and R1's progress precondition - they close gaps rather than redefine invariants, so no new canonical invariant IDs are minted; if the maintainer prefers explicit IDs, name them D4 (index fail-stop) and R2 (prefix progress) in the header's invariant block - flagged in IMPLEMENTER MUST NOT GUESS.

---

## 10. Fix Dependency Graph, Commit Sequence, Release Assessment

### Dependency graph

Dependencies between fixes, expressed as commit ordering constraints. 'A -> B' means A must land before B (B's test or code depends on A).

- CKV-001 -> CKV-002 (the split-entry validation the byte-aware split relies on; same file region, one review surface).
- CKV-001 + CKV-002 -> CKV-003a (the pre-memset validation and the alloc reorder touch the same split functions - land the capacity work first, then the exception-safety reorder).
- CKV-003a -> CKV-003b -> CKV-003c (the latch is meaningful once the tree can refuse cleanly; the cross-validation refuses against the latch's semantics).
- CKV-003c and CKV-005 share the checkpoint count assert - land CKV-005's scan_all first or in the same commit as 003c's checks (the assert's inputs change from sentinel-bounded to unbounded in the same window).
- CKV-018 -> CKV-004 (the D2 async test needs the pwrite_all hooks to inject the write-stage failure).
- CKV-018 -> CKV-012's test variant (the enqueue-hook choice is independent, but the fault infrastructure is shared - land 018 first either way).
- CKV-007 -> CKV-016 (the async catch-alls are correct only once notify_observers no longer throws - otherwise Failed would mask durable commits).
- CKV-001 -> CKV-009's test (the boundedness test's universe must stay under the key gate; no code dependency).
- No dependencies among CKV-006, CKV-010, CKV-011, CKV-014, CKV-015, and the Wave 5 items beyond ordinary merge order - they touch disjoint code.

### Recommended commit sequence

Twenty commits, each independently buildable and testable (make + the suite's gate), each carrying its own regression test(s), each reverting cleanly. No unrelated changes are ever combined. The sequence respects the dependency graph above and the wave priorities.

**Commit 1:** CKV-001: TREE_MAX_KEY_BYTES gate at commit_txn + split-entry pre-memset validation + root-writer guards + static_asserts + README key-limit update. Tests: tree-oversized-key-rejection.

**Commit 2:** CKV-002: byte-aware split point in split_leaf and split_interior (prefix sums, both-halves-fit, balanced choice, typed refusal) + README limitation note. Tests: btree-byte-aware-split (a)-(f).

**Commit 3:** CKV-003a: pool_.alloc() reorder before the memsets in split_leaf/split_interior (+ the layered-posture comment). Tests: the injection legs of oom-checkpoint-refusal begin to apply (full legs complete at commit 5).

**Commit 4:** CKV-003b: index_failed_ latch (entry checks in commit_txn, checkpoint refusal, health field). Tests: oom-checkpoint-refusal legs 1 (probe14 chain).

**Commit 5:** CKV-003c: checkpoint dirty-coverage + count cross-validation (both scan paths, before any write) + checkpoint-vs-WAL differential test. Tests: leg 3 + the first-checkpoint-empty edge.

**Commit 6:** CKV-005: scan_all (Cursor unbounded mode) + tree_scan_all + the five site conversions. Tests: sentinel-key-visibility (incl. LSan leg).

**Commit 7:** CKV-018: pwrite_all fault hooks + the de-vacuated short-write test (retargeted to the WAL path with the charge-fired assertion). Tests: the fixed test + mutation check.

**Commit 8:** CKV-004: rollback gate written-predicate + written -> atomic<bool> + debit correction + the stale-comment rewrite (3791-3804). Tests: d2-async-write-stage (three legs).

**Commit 9:** CKV-012: on_abandon burn callback wrapping the reservation window + the 6009-6017 comment correction. Tests: wal-reserve-burn-on-throw.

**Commit 10:** CKV-006: advance() skip loop. Tests: stream-tombstone-differential.

**Commit 11:** CKV-007: notify_observers containment + error handler + diag counter + README observer contract. Tests: observer-exception-matrix legs put/erase/batch/txn.

**Commit 12:** CKV-016: put_async/erase_async outer catch-alls. Tests: the matrix's async legs.

**Commit 13:** CKV-009: phantom-routed negative lookups (read/read_observed routing). Tests: negative-lookup-boundedness + the conflict-semantics legs.

**Commit 14:** CKV-014: Batch map dedup + consume-on-success + README row. Tests: batch-dedup-and-retry.

**Commit 15:** CKV-015: RWT engine keepalive + destructor predicate + Transaction member order. Tests: abandoned-txn-pin-release.

**Commit 16:** CKV-010: restore_pitr destination guard + README row. Tests: pitr-dirty-destination-refusal.

**Commit 17:** CKV-011: .tmp gating + torn-tail deferral + comment/README. Tests: pitr-source-byte-identity.

**Commit 18:** CKV-008: PageHeader mutation_epoch + LeafFence + fence comparison + the bump sites. Tests: btree-concurrent-putters (dst + TSan).

**Commit 19:** CKV-013 + CKV-019 + CKV-020: dead registered-files removal; corrupt-segment append-only refusal; close-failure diagnostic. (Three tiny, disjoint, independently-testable changes - the implementer MAY split into three commits; the grouping reflects their size, not a shared surface.) Tests: iouring E2E green + append-only-corrupt-refusal.

**Commit 20:** CKV-017 + CKV-021 + release notes: README LSN qualification; the 100-108 comment correction; the release-notes entry summarizing the contract changes (key limit, Batch semantics, observer containment, restore_pitr precondition) for the version decision.

### Release and versioning assessment

| Question | Assessment |
|---|---|
| Patch-level or breaking? | The aggregate is nominally patch-level (no on-disk or WAL format change, no migration) BUT it changes four documented/observable public behaviors: the effective key limit (README currently claims 64 KiB), Batch duplicate-key semantics, observer exception propagation, and restore_pitr's destination precondition. Nominally-breaking for any caller that depended on the previous (undefined or corrupt) behaviors - none can have depended on them correctly. |
| Public API changes required? | One additive member (Database::set_observer_error_handler) plus one additive diagnostics counter. No signature changes, no removals. The key limit and Batch changes are behavioral, not signature. |
| On-disk compatibility changes? | None. The checkpoint format, WAL framing, and MANIFEST are untouched. PageHeader changes (CKV-008) are memory-only - the tree is rebuilt from the WAL/checkpoint on every open (verified). Empty read-set entries (CKV-009) are memory-only artifacts that vanish on restart. |
| Migration required? | None. Pre-fix databases open cleanly (except those already poisoned by CKV-001-class corruption, which now fail loudly at open instead of re-corrupting - the honest behavior, documented in commit 1's message). |
| Recommended release shape | 0.28.0 (MINOR): the README's documented key limit changes, which is a documented-contract revision the version should advertise. If the maintainer classifies the key limit as a bug fix against an impossible contract (defensible - no key over 4044 bytes ever worked correctly), 0.27.1 is equally defensible. DECISION RESERVED FOR THE MAINTAINER - see IMPLEMENTER MUST NOT GUESS. |

---

## 11. Findings That Must Not Be Fixed

Findings and near-findings that must NOT receive engine changes in this remediation, with reasons. The implementer must not 'improve' any of these while in the neighborhood.

**probe9 (PITR mid-window boundary)**

REFUTED as a probe-expectation error - the engine's mid-window as-of view is CORRECT (verified twice: the original audit and its re-verification). No change; the probe is retained as a correctness regression test.

**probe15 (backup verify/restore consistency)**

REFUTED as a probe-expectation error - verify_backup and open() consistently reject orphan-segment directories with the same diagnosis. No change.

**Vacuous tests beyond CKV-018's scope**

The audit enumerated further vacuous or misaimed tests (the v17 GC memory-boundedness test, the v18 LSN-contiguity test, m2_phase1's size-rotation admission, the 30s soak's no-oracle). Fixing test vacuity is valuable but is test-hardening work, not defect remediation - EXCEPT where this spec explicitly upgrades a test (CKV-018's short-write test; the negative-lookup boundedness test replaces the vacuous GC-boundedness intent for CKV-009). The remainder belongs to a separate test-hardening pass after the defect work - out of scope here.

**lincheck's SSI blindness (no anti-dependency checker)**

A genuine gap the audit identified, but it is a test-infrastructure investment (a new checker and synthetic write-skew histories), not a defect fix. The CKV-009 change deliberately routes more traffic through the phantom machinery and its equivalence legs pin the semantics; the checker itself is ROADMAP material.

**Page reclamation / PagePool::free / merges / rebalance**

README-documented known limitations, deliberately out of scope. CKV-008's epoch makes a FUTURE reclamation design safer but this remediation wires nothing.

**Hazard-pointer machinery having no consumers**

Verified admission (is_hazardous has no callers). Redesigning it is a reclamation-project prerequisite, not a defect fix. Untouched.

**The 'conc: no crash' noise flag and similar hygiene**

Test noise - out of scope.

**CKV-013's alternative (implementing registered files with identity tracking)**

Explicitly rejected in favor of dead-code removal - activating a performance feature is not defect remediation. If the maintainer wants the feature, it is a separate project with the trap documented.

**CKV-019's optional segment-boundary LSN monotonicity assert**

Rejected with CKV-017: rotation deletion makes it unenforceable in general.

---

## 12. IMPLEMENTER MUST NOT GUESS

The implementer will implement ONLY what is specified in this document. Every unresolved design decision is enumerated below; for each, stop and obtain the maintainer's decision (or record the decision explicitly in the pull request) before proceeding. Recommendations in this specification are recommendations, not decisions.

- 1. The public error surface for the tree-capacity refusal (CKV-002's PageCapacityError and CKV-001's boundary): this spec recommends mapping both to Status::TooLarge for a single 'does not fit' semantics, but a distinct Status code (e.g., a new PageCapacity status) is equally defensible. The choice is user-visible; the maintainer decides.
- 2. The exact value and naming of the key bound: TREE_MAX_KEY_BYTES = 4044 is derived from the current page layout. Whether to expose it in the public Options/documentation as a named limit (recommended) or keep it internal is open. If PAGE_SIZE ever becomes configurable, the derivation must be revisited.
- 3. The fault-injection mechanism for CKV-012's test (a leader_pre_enqueue_hook_ mirroring leader_post_unlock_hook_, vs. a new AllocFail fault kind at records.push_back): both are specified as acceptable; pick per the maintainer's preference for hook-vs-kind consistency - do not build both.
- 4. CKV-015's abandonment route in the test: destroying an ACTIVE Transaction is documented to std::abort. Whether the sanctioned test route is an explicit abort() before destruction, or relaxing the abort for a test-only destructor path, is a contract question - do not silently weaken the abort contract.
- 5. The observer error channel (CKV-007): the optional set_observer_error_handler is this spec's recommendation. The alternative - diagnostic counter only, no handler - is smaller but leaves the exception information inaccessible. If the handler is accepted, its exact signature (std::function<void(std::exception_ptr)> vs. carrying the key) is open.
- 6. Whether the two new enforcement invariants get canonical IDs (D4 index fail-stop, R2 publication progress) in the header's invariant block, or remain documented only in code comments - a project-convention question.
- 7. The release version: 0.28.0 (minor - documented key-limit revision) vs. 0.27.1 (patch - impossible-contract bug fix). Both defensible; the maintainer decides and writes the release notes accordingly.
- 8. Whether the io_uring CQE-level fault hook (negative res on a real-kernel write CQE) is added now alongside CKV-018's pwrite_all hooks or deferred: this spec defers it (the fallback re-routing covers the remediation path); the maintainer may want the wrapper-level hook for the coverage ledger's completeness.
- 9. CI gating for the new heavy tests: btree-concurrent-putters under TSan and the 50k-iteration negative-lookup test add minutes to jobs. Which jobs gate PRs vs. nightly is a CI policy decision.
- 10. The PageHeader re-layout for CKV-008's mutation_epoch: this spec sketches repurposing reserved bytes within the 32-byte size, but the exact field placement must be recomputed from the packed struct at implementation time - if the reserved space proves insufficient, widening the header (changing sizeof) is acceptable ONLY because the header is memory-only (verified); the maintainer should confirm no external tooling assumes the 32-byte layout.
- 11. CKV-014's dedup data structure: std::map is specified for minimal diff; an unordered_map plus a sorted iteration at commit is equivalent. Do not switch for performance without a measurement.
- 12. Nothing in this specification authorizes reformatting, renaming, comment-sweeping, warning-fixing, or any change not enumerated in a finding's Exact implementation locations. If a required change cannot be made without touching adjacent code, stop and report it.

