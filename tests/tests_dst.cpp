// tests/tests_dst.cpp (dstscn namespace + run_dst_test) — v27 M0 deterministic-scheduler harness — extracted from main.cpp (v29 M2 item 5: the test-suite TU
// split, unlocked by API-1's ODR-safe header). Content is byte-identical to
// the extracted region except: the battery entry point loses `static` (it is
// declared in tests/test_decls.hpp and called from main.cpp), and the whole
// body rides under CHRONOKV_TEST_HOOKS exactly as it did inside main.cpp's
// self-tests section. No engine or test logic changed in the move.
#include "chronokv.hpp"
#include "chronokv.hpp"   // API-1: header must be idempotent (#pragma once)
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>
#include <cstring>

#ifdef CHRONOKV_TEST_HOOKS

// =====================================================================
// v27 M0 test driver: the deterministic-scheduler (DST) harness.
//
// Why fork-per-seed: the two anchor bugs this harness exists to catch
// class of are a HANG (C1: followers stranded on batch_cv_ when the
// leader's cleanup was skipped) and a SEGV (H3: null batch dereference).
// In-process, either takes the whole suite down — which is exactly why
// the old "8 concurrent committers vs a failing rotation" variant was
// DROPPED from the suite (it wedged ~Database against unfixed code).
// Forked children turn both into ordinary, reportable failures: the
// parent classifies exit status (invariant), signal (crash) or deadline
// (hang), and prints (scenario, seed, op-count, last point) — the
// replay handle the roadmap demands ("a failure then reproduces from
// (seed, op-count)").
//
// Scenarios (one per bug-dense area the roadmap scopes, plus the two
// historical anchors):
//   S1 wal_mixed_handoff  — H3 class: durability flipper + concurrent
//                           engine commits; mixed-class batch handoffs on
//                           every flip. Clean code: every commit lands.
//                           H3 mutant: orphaned batch -> SEGV or livelock.
//   S2 wal_leader_fault   — C1 class: forced rotation + armed SegOpenFail
//                           + concurrent Database::put. The leader throws
//                           WHILE OWNING batch_mu_; followers are parked on
//                           batch_cv_. Clean code: fail-stop, WalFailure,
//                           every thread RETURNS. C1 mutant: cleanup
//                           skipped -> followers strand -> parent deadline.
//   S3 gc_epoch_scan      — GC class: version churn + synchronous gc_once
//                           passes + range scans with monotonic-value
//                           validation. Reclaim-vs-scan defects surface as
//                           SEGV (use-after-free) or garbage/rewound
//                           values (invariant exit).
//   S4 tree_cursor_split  — tree class: ascending inserts (rightmost-leaf
//                           splits) + concurrent full-range scans asserting
//                           prefix contiguity, order and exact values.
//                           Cursor-vs-split defects drop/duplicate/reorder
//                           keys or crash.
//   S5 tree_merge_cascade — v29 M2 item 2 (M4 grammar): seeded composition
//                           of delete windows (thin/run/stride/revive/
//                           sweep-all) over a prebuilt multi-leaf tree,
//                           concurrent latest + PINNED as-of scanners and
//                           gc pressure; quiescence differential-vs-
//                           std::map, drain-to-empty and regrow. Green on
//                           the merge-free tree; binding on borrow/merge/
//                           cascade/root-collapse when M4 lands.
//   S6 reclaim_retire_pin — v29 M2 item 2 (M3 grammar): deep version
//                           chains + page-churn material under a pinned
//                           reader; proves retirement is BLOCKED while the
//                           pin holds (witness) and DRAINS after release
//                           (liveness), with the pinned view byte-stable
//                           throughout. Green today at the Version layer;
//                           the same contract binds pages/KeyEntries when
//                           M3 wires PagePool::free() and the epoch/
//                           generation retirement of index nodes.
//
// Determinism notes: every scenario thread parks at dst::point("start")
// for the rendezvous, so the schedule is a pure function of the seed;
// the child's Progress page (shared mmap) carries the op count, grant and
// steal counters and the last point name to the parent for triage. Steals
// (bounded-patience deadlock breaking) are counted; clean scenarios on
// clean code complete without the parent deadline ever firing.
// =====================================================================
#ifdef CHRONOKV_STRESS
namespace dstscn {
    using namespace chronokv;

    // S1 — WAL mixed-durability handoff (H3 class).
    inline int s1_mixed_handoff(uint64_t seed, dst::Progress* prog, const std::string& wd) {
        ChronoKV kv(wd);
        constexpr int NT = 3, OPS = 8;
        std::atomic<long> commits{0};
        std::atomic<bool> stop{false};
        dst::arm(seed, NT + 1, prog);
        // 0.27.0 hardening: BOUND the flipper. The free-running flip loop
        // was the scenario's only unbounded workload, and a flipper-
        // dominant schedule turns into millions of baton handoffs — one
        // measured child ran 8.3M ops. Under a slow-disk runner that tail
        // stretches shard wall time by hours (observed: one nightly shard
        // at 2h15m+ vs 49-58m for its identical-code siblings). The budget
        // (8 flips per writer op) keeps the durability-class mixing dense
        // — every commit still sees multiple handoff opportunities — while
        // capping the spin source. H3/C1 detection re-verified with the
        // budget in place (mutants: SEGV 3/3, hang caught).
        constexpr long FLIP_BUDGET = (long)NT * OPS * 8;
        std::thread flipper([&] {
            dst::point("start");
            for (long f = 0; f < FLIP_BUDGET && !stop.load(std::memory_order_relaxed); ++f) {
                kv.set_durability(DurabilityMode::Async);
                dst::point("flip_a");
                kv.set_durability(DurabilityMode::Group);
                dst::point("flip_g");
            }
            dst::thread_done();
        });
        std::vector<std::thread> ws;
        for (int i = 0; i < NT; ++i) ws.emplace_back([&, i] {
            dst::point("start");
            for (int n = 0; n < OPS; ++n) {
                dst::point("w_op");
                try {
                    if (kv.commit("s1_k" + std::to_string(i) + "_" + std::to_string(n), "v")
                            == TxnResult::Committed)
                        commits.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {}
            }
            dst::thread_done();
        });
        for (auto& t : ws) t.join();
        stop.store(true, std::memory_order_relaxed);
        flipper.join();
        dst::disarm();
        if (commits.load() != NT * OPS) {
            fprintf(stderr, "dst S1: only %ld of %d commits landed (orphaned batch?)\n",
                    commits.load(), NT * OPS);
            return 9;
        }
        return 0;
    }

    // S2 — WAL leader exception with parked followers (C1 class).
    inline int s2_leader_fault(uint64_t seed, dst::Progress* prog, const std::string& wd) {
#ifdef CHRONOKV_FAULT_INJECTION
        Options o;
        o.wal_dir = wd;
        o.auto_start_gc = false;
        o.durability = DurabilityMode::Sync;   // every commit takes the leader path
        auto db = Database::open(o);
        db.force_wal_rotation_for_test();      // next leader pass MUST rotate
        fault::arm(fault::Kind::SegOpenFail, 1);  // ... and the rotation MUST fail
        // 4 threads x 8 puts: the C1 hang needs a FOLLOWER whose record is
        // already in (or parks against) the doomed leader's batch when the
        // mutated cleanup is skipped — puts that ENTER group_append after
        // failed_ was set short-circuit to WalFailure without ever parking.
        // More overlap raises the per-seed catch probability (measured:
        // ~0.4 at 3x6, higher here); at the CI seed counts (>=30/scenario)
        // the miss probability is negligible.
        constexpr int NT = 4, OPS = 8;
        std::atomic<int> returned{0}, escaped{0};
        dst::arm(seed, NT, prog);
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; ++i) ts.emplace_back([&, i] {
            dst::point("start");
            for (int n = 0; n < OPS; ++n) {
                dst::point("p_op");
                try {
                    (void)db.put("s2_k" + std::to_string(i) + "_" + std::to_string(n), "v");
                    returned.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    escaped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            dst::thread_done();
        });
        for (auto& t : ts) t.join();
        dst::disarm();
        fault::disarm();
        bool failed = db.wal_failed_for_test();
        try { db.close(); } catch (...) {}
        if (!failed) {
            fprintf(stderr, "dst S2: armed rotation failure never fired\n");
            return 9;
        }
        if (escaped.load() != 0) {
            fprintf(stderr, "dst S2: %d puts threw (leader exception escaped)\n", escaped.load());
            return 9;
        }
        if (returned.load() != NT * OPS) {
            fprintf(stderr, "dst S2: only %d of %d puts returned\n", returned.load(), NT * OPS);
            return 9;
        }
        return 0;
#else
        (void)seed; (void)prog; (void)wd;
        return 0;   // scenario is a no-op without fault injection
#endif
    }

    // S3 — GC/epoch reclamation vs concurrent scans.
    inline int s3_gc_scan(uint64_t seed, dst::Progress* prog, const std::string& wd) {
        ChronoKV kv(wd);
        constexpr int NW = 2, OPS = 30, NKEYS = 4;
        std::atomic<long> seq{0};
        std::atomic<int> bad{0};
        dst::arm(seed, NW + 2, prog);
        std::mutex legit_mu;
        std::map<std::string, std::set<std::string>> legit;
        std::vector<std::thread> ws;
        for (int i = 0; i < NW; ++i) ws.emplace_back([&] {
            dst::point("start");
            for (int n = 0; n < OPS; ++n) {
                dst::point("w");
                long v = seq.fetch_add(1, std::memory_order_relaxed) + 1;
                std::string k = "g" + std::to_string(n % NKEYS);
                std::string val = "v" + std::to_string(v);
                {
                    std::lock_guard<std::mutex> g(legit_mu);
                    legit[k].insert(val);
                }
                try { (void)kv.commit(k, val); } catch (...) {}
            }
            dst::thread_done();
        });
        // Legitimacy oracle (declared above, before the writers): every
        // (key, value) pair any writer ever handed to commit(). NOTE:
        // per-key MONOTONICITY would be a WRONG oracle — the two writers
        // share one global seq counter, and snapshot order follows commit
        // cts, not seq reservation order, so a later scan may legitimately
        // observe an EARLIER seq (the first DST run flagged exactly that).
        // "Observed => actually written to this key" is the sound invariant:
        // use-after-free / reclaim-vs-scan races surface as garbage or
        // foreign-key values, which are not in the set.
        std::thread scanner([&] {
            dst::point("start");
            for (int n = 0; n < OPS; ++n) {
                dst::point("scan");
                std::vector<std::pair<std::string, std::string>> rs;
                try { rs = kv.range_scan(UINT64_MAX, "g0", "g9"); }
                catch (...) { bad.fetch_add(1); break; }
                for (auto& [k, v] : rs) {
                    if (k.size() != 2 || k[0] != 'g') { bad.fetch_add(1); continue; }
                    std::lock_guard<std::mutex> g(legit_mu);
                    auto it = legit.find(k);
                    if (it == legit.end() || !it->second.count(v)) bad.fetch_add(1);
                }
            }
            dst::thread_done();
        });
        std::thread gct([&] {
            dst::point("start");
            for (int n = 0; n < OPS * 2; ++n) {
                dst::point("gc");
                try { kv.gc_pass_for_test(); } catch (...) {}
            }
            dst::thread_done();
        });
        for (auto& t : ws) t.join();
        scanner.join();
        gct.join();
        dst::disarm();
        if (bad.load()) {
            fprintf(stderr, "dst S3: %d scan anomalies (garbage/rewound values)\n", bad.load());
            return 9;
        }
        return 0;
    }

    // S4 — B+ tree cursor vs concurrent splits.
    inline int s4_tree_split(uint64_t seed, dst::Progress* prog, const std::string& wd) {
        ChronoKV kv(wd);
        constexpr int NINS = 400, NSCANS = 30;
        std::atomic<int> bad{0};
        dst::arm(seed, 3, prog);
        std::thread writer([&] {
            dst::point("start");
            for (int i = 0; i < NINS; ++i) {
                dst::point("ins");
                char k[16];
                snprintf(k, sizeof k, "t%05d", i);
                try { (void)kv.commit(k, std::string("val_") + k); } catch (...) {}
            }
            dst::thread_done();
        });
        auto scan_loop = [&] {
            dst::point("start");
            for (int n = 0; n < NSCANS; ++n) {
                dst::point("scan");
                std::vector<std::pair<std::string, std::string>> rs;
                try { rs = kv.range_scan(UINT64_MAX, "t00000", "t99999"); }
                catch (...) { bad.fetch_add(1); break; }
                // Ascending single-writer inserts: every snapshot scan must
                // be a CONTIGUOUS PREFIX t00000..tK with exact values. A
                // cursor that loses a leaf-switch race drops, duplicates or
                // reorders keys — all caught here.
                for (size_t j = 0; j < rs.size(); ++j) {
                    char k[16];
                    snprintf(k, sizeof k, "t%05d", (int)j);
                    if (rs[j].first != k || rs[j].second != std::string("val_") + k) {
                        bad.fetch_add(1);
                        break;
                    }
                }
            }
            dst::thread_done();
        };
        std::thread sc1(scan_loop), sc2(scan_loop);
        writer.join();
        sc1.join();
        sc2.join();
        dst::disarm();
        if (bad.load()) {
            fprintf(stderr, "dst S4: %d scan-prefix violations\n", bad.load());
            return 9;
        }
        return 0;
    }
    // =================================================================
    // v29 M2 item 2: the DST plan grammar grows merge/rebalance/cascade
    // and reclamation-retirement scenarios BEFORE that code exists —
    // written against the M3/M4 designs; the implementations must
    // survive them when they land. Both scenarios are green against the
    // current tree by construction: today's engine deletes are tombstone
    // versions (no physical leaf removal, no page retirement/reuse), so
    // the oracles below pin exactly the contracts that must hold BOTH
    // now and after M3 wires PagePool::free()/epoch retirement and M4
    // lands borrow/merge/cascade/root-collapse:
    //   * a pinned as-of view is byte-stable across every delete/gc/
    //     (future: merge/retire/reuse) window — the "concurrent cursor +
    //     concurrent merge" half of the M4 acceptance and the epoch-pin
    //     safety half of the M3 design (today: Version retirement; after
    //     M3: retired pages a pinned walk still traverses);
    //   * in-flight scans stay strictly ordered, duplicate-free and
    //     legitimate (every observed pair was really committed);
    //   * at quiescence the tree differentials against std::map (the
    //     M4 acceptance sentence) — including drain-to-empty (the
    //     root-collapse shape) and regrow-from-empty (split-from-root);
    //   * reclamation is BLOCKED while a reader pins (safety witness)
    //     and DRAINS after the pin releases (liveness — the "pool
    //     stabilizes instead of ratcheting" direction of M3's P1, at
    //     the version layer where retirement exists today).
    // The delete grammar is seeded: a per-seed PRNG composes THIN (borrow
    // shape), RUN (emptied-leaf merge shape), STRIDE (separator/interior
    // shape), REVIVE (split x merge interleave) and SWEEP-ALL windows
    // (cascade-to-root + collapse), so different seeds explore different
    // merge cascades against the same plan-independent oracles.
    //
    // Structure note (learned by measurement, first cut): the MAIN thread
    // is the pinner. It captures V0 quiescent, then arms the harness and
    // joins — the pin provably precedes every concurrent op, so no
    // gate/spin-loop is needed anywhere. The first cut gated workers on
    // a pin_ready flag polled through dst::point; a parked poller
    // self-grants the baton whenever the waiting set is just itself, so
    // every microsecond of another thread's engine work (a group-commit
    // fsync window, a stolen baton) burned hundreds of poll points —
    // 32k-47k scheduling ops per S5 child (vs ~4k of real work) and a
    // S6 that ground to the 20 s fork deadline. Bounded worker loops and
    // a bounded prober thread spend the op budget on engine windows
    // instead. Also non-negotiable here: model_mu is taken ONLY in
    // brace-scoped one-liners — never across a dst::point or an engine
    // call (the first S6 cut deadlocked exactly that way: a round-body
    // lock_guard lived into the loop's second acquisition — a
    // self-deadlock on a non-recursive mutex that the steal window
    // cannot break, hanging every seed).
    //
    // Non-vacuity (mutation-proven at introduction — see docs/ROADMAP.md
    // M2 item 2): re-introducing the v19-M2-fix defect class (gc
    // threshold computed WITHOUT the reader-slot minimum) breaks the
    // pinned-view stability oracle in BOTH scenarios — a tombstone
    // committed above the pin makes the pre-pin visible version
    // severable, and the pinned scan silently loses the key; neutering
    // retire() or reclaim_retired() fails S6's retirement witness and
    // drain oracles respectively.
    // =================================================================

    // S5 — merge/rebalance/cascade grammar (M4-shaped delete windows).
    inline int s5_merge_cascade(uint64_t seed, dst::Progress* prog, const std::string& wd) {
        ChronoKV kv(wd);
        // Build/quiescence phases run Async (durability is incidental to
        // the in-memory oracles and the fsync waits would dominate the
        // child); the concurrent churn phase runs Group, the realistic
        // handoff path. S1 established durability flipping as
        // scenario-legal.
        kv.set_durability(DurabilityMode::Async);
        constexpr int NB = 640;             // built keys m00000.. (2 versions each)
        constexpr int RN = 80;              // refiller keys n00000..
        const std::string LO = "a", HI = "z~";   // covers the m/n universe
        auto mkey = [](int i) { char b[8];  snprintf(b, sizeof b, "m%05d", i); return std::string(b); };
        auto nkey = [](int i) { char b[8];  snprintf(b, sizeof b, "n%05d", i); return std::string(b); };
        auto mval = [](int i, const char* p) { char b[16]; snprintf(b, sizeof b, "%s%05d", p, i); return std::string(b); };

        // Phase 0 (single-threaded, pre-arm): build ~4-6 leaves under the
        // 4 KiB page / 12 B slot geometry with an interior root, TWO
        // versions per key (v1 "vm", v2 "wm") so every chain has
        // below-anchor retirement material the pinned epoch must protect.
        for (int i = 0; i < NB; ++i) {
            if (kv.commit(mkey(i), mval(i, "vm")) != TxnResult::Committed ||
                kv.commit(mkey(i), mval(i, "wm")) != TxnResult::Committed) {
                fprintf(stderr, "dst S5: build commit %d failed\n", i);
                return 9;
            }
        }

        // ---- Seeded plan composition (the grammar). ----
        struct Rng { uint64_t s; uint64_t n() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s >> 11; } };
        Rng rng{seed ^ 0x5EEDC0DE5ULL};
        struct Step { bool del; int idx; };
        std::vector<Step> plan;
        std::set<int> gone;                  // currently-deleted built indices
        std::vector<int> gone_order;         // revive pick order
        int dels = 0;
        const int del_budget = 300 + (int)(rng.n() % 151);    // 300..450 of NB
        const bool empty_tree = (rng.n() & 3) == 0;           // 1/4 seeds: final sweep
        auto add_del = [&](int i) {
            if (dels >= del_budget || gone.count(i)) return;
            gone.insert(i); gone_order.push_back(i);
            plan.push_back(Step{true, i}); ++dels;
        };
        long guard = 0;
        while (dels < del_budget && guard++ < del_budget * 20L) {
            switch (rng.n() % 4) {
            case 0: {   // THIN: every other key in a window -> all leaves in
                        // the window drop to ~50% -> sibling-borrow shape.
                int a = (int)(rng.n() % (unsigned)NB), len = 40 + (int)(rng.n() % 120);
                for (int i = a; i < NB && i < a + len; i += 2) add_del(i);
                break; }
            case 1: {   // RUN: contiguous window -> whole leaves empty out ->
                        // merge shape (and interior separator removal).
                int len = 30 + (int)(rng.n() % 130);
                int a = (int)(rng.n() % (unsigned)(NB - len + 1));
                for (int i = a; i < a + len; ++i) add_del(i);
                break; }
            case 2: {   // STRIDE: boundary-ish hits across the whole range ->
                        // every leaf thins; the stride approximates leaf
                        // fanout fractions, the interior-key-delete shape.
                int st = 17 + (int)(rng.n() % 40);
                for (int i = (int)(rng.n() % (unsigned)st); i < NB; i += st) add_del(i);
                break; }
            default: {  // REVIVE: re-put deleted keys ("xm" value) -> inserts
                        // into a shrinking tree: the split x merge interleave.
                int nrev = 0;
                while (nrev < 16 && !gone_order.empty() && (int)plan.size() < del_budget + 120) {
                    int pick = (int)(rng.n() % gone_order.size());
                    int i = gone_order[pick];
                    gone.erase(i);
                    gone_order.erase(gone_order.begin() + pick);
                    plan.push_back(Step{false, i});
                    ++nrev; --dels;          // revived key re-enters the budget
                }
                break; }
            }
        }
        if (empty_tree)
            for (int i = 0; i < NB; ++i) add_del(i);   // cascade-to-root + collapse

        // ---- Oracles' shared state. ----
        std::mutex model_mu;
        std::map<std::string, std::string> live;             // quiescence differential
        std::map<std::string, std::set<std::string>> ever;   // in-flight legitimacy
        for (int i = 0; i < NB; ++i) {
            live[mkey(i)] = mval(i, "wm");
            ever[mkey(i)].insert(mval(i, "vm"));
            ever[mkey(i)].insert(mval(i, "wm"));
        }
        std::atomic<int> bad{0};

        // ---- The pinned window: main is the pinner (see structure note).
        uint64_t ts0 = 0;
        std::vector<std::pair<std::string, std::string>> v0;
        {
            SnapshotGuard sg(kv);
            ts0 = sg.read_ts();
            v0 = kv.range_scan(ts0, LO, HI);
            {   // capture-correctness control: quiescent at capture, so V0
                // IS exactly the build (all NB keys at their "wm" version).
                std::lock_guard<std::mutex> g(model_mu);
                if (v0.size() != live.size()) bad.fetch_add(1, std::memory_order_relaxed);
                else for (auto& [k, v] : v0) {
                    auto it = live.find(k);
                    if (it == live.end() || it->second != v) { bad.fetch_add(1, std::memory_order_relaxed); break; }
                }
            }
            kv.set_durability(DurabilityMode::Group);   // churn: the real handoff path
            dst::arm(seed, 5, prog);

            // T1 sweeper: executes the composed delete/revive grammar.
            std::thread sweeper([&] {
                dst::point("start");
                for (const auto& st : plan) {
                    const std::string k = mkey(st.idx);
                    if (st.del) {
                        dst::point("del");
                        if (kv.del(k) != TxnResult::Committed) { bad.fetch_add(1, std::memory_order_relaxed); continue; }
                        { std::lock_guard<std::mutex> g(model_mu); live.erase(k); }
                    } else {
                        dst::point("rev");
                        const std::string v = mval(st.idx, "xm");
                        {   // legitimacy first: observed => already in ever (S3 ordering)
                            std::lock_guard<std::mutex> g(model_mu);
                            ever[k].insert(v); live[k] = v;
                        }
                        if (kv.commit(k, v) != TxnResult::Committed) bad.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                dst::thread_done();
            });

            // T2 refiller: fresh keys while the tree shrinks — splits racing
            // the (future) merge windows.
            std::thread refiller([&] {
                dst::point("start");
                for (int i = 0; i < RN; ++i) {
                    dst::point("ins");
                    const std::string k = nkey(i), v = "vn" + std::to_string(i);
                    {
                        std::lock_guard<std::mutex> g(model_mu);
                        ever[k].insert(v); live[k] = v;
                    }
                    if (kv.commit(k, v) != TxnResult::Committed) bad.fetch_add(1, std::memory_order_relaxed);
                }
                dst::thread_done();
            });

            // T3 latest scanner: in-flight structural invariants (exact-set
            // equality is a quiescence property — during churn the scan races
            // the sweeper legitimately; sound checks are order, uniqueness,
            // legitimacy).
            std::thread scanner([&] {
                dst::point("start");
                for (int n = 0; n < 12; ++n) {
                    dst::point("scan");
                    std::vector<std::pair<std::string, std::string>> rs;
                    try { rs = kv.range_scan(UINT64_MAX, LO, HI); }
                    catch (...) { bad.fetch_add(1, std::memory_order_relaxed); break; }
                    std::string prev;
                    for (size_t j = 0; j < rs.size(); ++j) {
                        if (j && !(prev < rs[j].first)) { bad.fetch_add(1, std::memory_order_relaxed); break; }
                        prev = rs[j].first;
                        std::lock_guard<std::mutex> g(model_mu);
                        auto it = ever.find(rs[j].first);
                        if (it == ever.end() || !it->second.count(rs[j].second)) {
                            bad.fetch_add(1, std::memory_order_relaxed); break;
                        }
                    }
                }
                dst::thread_done();
            });

            // T4 prober: the pinned as-of stability oracle, IN FLIGHT —
            // ts0 is protected by main's guard (gc_threshold takes the min
            // over ALL slots), so every repetition must be byte-identical
            // to V0 no matter what the churn/gc windows do around it. When
            // M4 lands, this is the concurrent cursor that must survive
            // physical leaf removal underneath it; when M3 lands, the
            // epoch pin that must keep retired structures alive.
            std::thread prober([&] {
                dst::point("start");
                for (int n = 0; n < 12; ++n) {
                    dst::point("probe");
                    if (kv.range_scan(ts0, LO, HI) != v0) bad.fetch_add(1, std::memory_order_relaxed);
                    const int i = (n * 137) % NB;
                    auto one = kv.range_scan(ts0, mkey(i), mkey(i));
                    if (one.size() != 1 || one[0].second != mval(i, "wm"))
                        bad.fetch_add(1, std::memory_order_relaxed);
                }
                dst::thread_done();
            });

            // T5 gc: retirement pressure under the pin (tombstones/revives
            // stack above ts0; the pre-pin v1s become below-anchor
            // retirement material the pinned epoch must protect).
            // 6 passes: with the 256-keys/pass budget the ~720-key tree
            // gets full sweeps, and every severable chain is retired in
            // the first passes under the pin — more passes only multiply
            // the per-key gc_anchor_sever scheduling points.
            std::thread gct([&] {
                dst::point("start");
                for (int n = 0; n < 6; ++n) {
                    dst::point("gc");
                    try { kv.gc_pass_for_test(); } catch (...) {}
                }
                dst::thread_done();
            });

            sweeper.join(); refiller.join(); scanner.join(); prober.join(); gct.join();
            dst::disarm();
            // Final stability, still pinned: after EVERY window has run.
            if (kv.range_scan(ts0, LO, HI) != v0) bad.fetch_add(1, std::memory_order_relaxed);
        }   // pin released
        if (bad.load()) {
            fprintf(stderr, "dst S5: %d in-flight violations (pin-stability/order/legitimacy)\n", bad.load());
            return 9;
        }

        // ---- Quiescence oracles (post-join: deterministic, no points). ----
        kv.set_durability(DurabilityMode::Async);
        // Q1: differential-vs-std::map over the churned tree (the M4
        // acceptance sentence, applied at rest).
        {
            auto rs = kv.range_scan(UINT64_MAX, LO, HI);
            if (rs.size() != live.size()) {
                fprintf(stderr, "dst S5: quiescent scan %zu keys, model %zu\n", rs.size(), live.size());
                return 9;
            }
            for (auto& [k, v] : rs) {
                auto it = live.find(k);
                if (it == live.end() || it->second != v) {
                    fprintf(stderr, "dst S5: quiescent differential mismatch at %s\n", k.c_str());
                    return 9;
                }
            }
        }
        // Q2: point reads agree with the differential (sampled).
        for (int i = 0; i < NB; i += 7) {
            auto r = kv.read(mkey(i));
            auto it = live.find(mkey(i));
            if (it == live.end()) {
                if (r) { fprintf(stderr, "dst S5: deleted key %s reads back\n", mkey(i).c_str()); return 9; }
            } else if (!r || *r != it->second) {
                fprintf(stderr, "dst S5: point-read mismatch at %s\n", mkey(i).c_str()); return 9;
            }
        }
        // Q3: drain-to-empty and regrow — the root-collapse / empty-tree /
        // split-from-root shapes at rest. When M4 lands, this physically
        // empties every leaf, collapses the root, and regrows from a bare
        // root; the oracles are the same sentences either way.
        {
            std::vector<std::string> keys;
            keys.reserve(live.size());
            for (auto& [k, v] : live) keys.push_back(k);
            for (auto& k : keys)
                if (kv.del(k) != TxnResult::Committed) {
                    fprintf(stderr, "dst S5: quiescent delete failed\n"); return 9;
                }
            live.clear();
        }
        if (auto rs0 = kv.range_scan(UINT64_MAX, LO, HI); !rs0.empty()) {
            fprintf(stderr, "dst S5: tree not empty after full delete (%zu keys)\n", rs0.size());
            return 9;
        }
        constexpr int RG = 120;
        for (int i = 0; i < RG; ++i)
            if (kv.commit(nkey(RN + i), "rg" + std::to_string(i)) != TxnResult::Committed) {
                fprintf(stderr, "dst S5: regrow commit %d failed\n", i); return 9;
            }
        {
            auto rs1 = kv.range_scan(UINT64_MAX, LO, HI);
            if (rs1.size() != RG) {
                fprintf(stderr, "dst S5: regrow scan %zu keys, want %d\n", rs1.size(), RG);
                return 9;
            }
            for (int i = 0; i < RG; ++i)
                if (rs1[i].first != nkey(RN + i) || rs1[i].second != "rg" + std::to_string(i)) {
                    fprintf(stderr, "dst S5: regrow mismatch at slot %d\n", i); return 9;
                }
        }
        return 0;
    }

    // S6 — reclamation-retirement grammar (M3-shaped: retirement blocked
    // by a pin, drained after it; deep chains + page-churn material).
    inline int s6_reclaim_retire(uint64_t seed, dst::Progress* prog, const std::string& wd) {
        ChronoKV kv(wd);
        kv.set_durability(DurabilityMode::Async);   // pre-phase; churn runs Group
        constexpr int NK = 8;       // r-keys: deep version chains (the anchor+suffix shape)
        constexpr int VH = 6;       // versions per r-key pre-pin
        constexpr int EP = 240;     // e-keys: page-churn material (splits pre-pin;
                                    // after M3/M4 their tombstones become physical
                                    // leaf removals -> retired/reused PAGES a pinned
                                    // walk must survive)
        constexpr int EP_PRE = 120; // e-keys deleted pre-pin: [tombstone, v1] chains ->
                                    // retirement material strictly below the pin
        const std::string LO = "a", HI = "z~";
        auto rkey = [](int i) { return "r" + std::to_string(i); };
        auto ekey = [](int i) { char b[8]; snprintf(b, sizeof b, "e%04d", i); return std::string(b); };

        // Pre-history (single-threaded, pre-arm): NK chains x VH versions;
        // EP e-keys (single version each), EP_PRE of them deleted again.
        for (int i = 0; i < NK; ++i)
            for (int v = 1; v <= VH; ++v)
                if (kv.commit(rkey(i), "v" + std::to_string(v)) != TxnResult::Committed) {
                    fprintf(stderr, "dst S6: pre-history commit failed\n"); return 9;
                }
        for (int i = 0; i < EP; ++i) {
            if (kv.commit(ekey(i), "e" + std::to_string(i)) != TxnResult::Committed) {
                fprintf(stderr, "dst S6: e-key commit failed\n"); return 9;
            }
            if (i < EP_PRE && kv.del(ekey(i)) != TxnResult::Committed) {
                fprintf(stderr, "dst S6: e-key pre-delete failed\n"); return 9;
            }
        }
        // Expected pinned view V0 (deterministic): the EP-EP_PRE surviving
        // e-keys, and the r-keys at their VH-th version. Tree order = byte
        // order: all e-keys sort before all r-keys.
        std::vector<std::pair<std::string, std::string>> want_v0;
        for (int i = EP_PRE; i < EP; ++i) want_v0.emplace_back(ekey(i), "e" + std::to_string(i));
        for (int i = 0; i < NK; ++i) want_v0.emplace_back(rkey(i), "v" + std::to_string(VH));
        std::sort(want_v0.begin(), want_v0.end());

        std::mutex model_mu;
        std::map<std::string, std::string> live;    // quiescence differential
        for (auto& [k, v] : want_v0) live[k] = v;
        std::atomic<int> bad{0};

        // ---- The pinned window: main is the pinner (see S5 structure
        // note). Every gc pass below runs WHILE this slot pins (snapshot
        // ts0, reclaim epoch E): the M3 safety contract — nothing the ts0
        // view can reach may be reclaimed (today: Version suffixes; after
        // M3: pages and index nodes through the same epoch-pin protocol
        // or its generation-tagged successor).
        uint64_t ts0 = 0;
        std::vector<std::pair<std::string, std::string>> v0;
        {
            SnapshotGuard sg(kv);
            ts0 = sg.read_ts();
            v0 = kv.range_scan(ts0, LO, HI);
            if (v0 != want_v0) bad.fetch_add(1, std::memory_order_relaxed);   // capture control
            kv.set_durability(DurabilityMode::Group);
            dst::arm(seed, 4, prog);

            // T1: r-key version churn ABOVE the pin (new versions,
            // tombstones, revives) — the write pressure that makes gc
            // sever/retire while the pin holds the floor. r0..r3.
            std::thread churn_a([&] {
                dst::point("start");
                struct Rng { uint64_t s; uint64_t n() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s >> 11; } };
                Rng rng{seed ^ 0xC6A5C6ULL};
                bool deleted[4] = {false, false, false, false};
                for (int round = 0; round < 12; ++round) {
                    dst::point("w_a");
                    const int j = (int)(rng.n() % 4);
                    const std::string k = rkey(j);
                    if (!deleted[j] && (rng.n() & 3) == 0) {          // tombstone
                        if (kv.del(k) != TxnResult::Committed) bad.fetch_add(1, std::memory_order_relaxed);
                        deleted[j] = true;
                        { std::lock_guard<std::mutex> g(model_mu); live.erase(k); }
                    } else {                                          // new version / revive
                        const std::string v = "a" + std::to_string(round) + "_" + std::to_string(j);
                        if (kv.commit(k, v) != TxnResult::Committed) bad.fetch_add(1, std::memory_order_relaxed);
                        deleted[j] = false;
                        { std::lock_guard<std::mutex> g(model_mu); live[k] = v; }
                    }
                }
                dst::thread_done();
            });

            // T2: e-key drain (the future page-retirement traffic) + r4..r7
            // version churn. Disjoint keys from T1: one model, no cross-talk.
            std::thread churn_b([&] {
                dst::point("start");
                for (int round = 0; round < 12; ++round) {
                    dst::point("w_b");
                    const int j = 4 + (round % 4);
                    const std::string k = rkey(j);
                    const std::string v = "b" + std::to_string(round) + "_" + std::to_string(j);
                    if (kv.commit(k, v) != TxnResult::Committed) bad.fetch_add(1, std::memory_order_relaxed);
                    { std::lock_guard<std::mutex> g(model_mu); live[k] = v; }
                    // Drain two surviving e-keys per round (24 total): the
                    // post-M3/M4 page-retirement traffic — deleting keys the
                    // pinned view still shows. model_mu is brace-scoped:
                    // NEVER held across a dst::point or an engine call (see
                    // the S5 structure note — the first cut deadlocked).
                    for (int d = 0; d < 2; ++d) {
                        const int ei = EP_PRE + round * 8 + d;   // < EP by construction
                        if (ei >= EP) continue;
                        dst::point("del_e");
                        if (kv.del(ekey(ei)) != TxnResult::Committed) bad.fetch_add(1, std::memory_order_relaxed);
                        { std::lock_guard<std::mutex> g(model_mu); live.erase(ekey(ei)); }
                    }
                }
                dst::thread_done();
            });

            // T3 prober: the pinned as-of stability oracle, in flight (the
            // S5 prober's contract against the retirement-heavy history).
            std::thread prober([&] {
                dst::point("start");
                for (int n = 0; n < 12; ++n) {
                    dst::point("probe");
                    if (kv.range_scan(ts0, LO, HI) != v0) bad.fetch_add(1, std::memory_order_relaxed);
                    const std::string k = rkey(n % NK);
                    auto one = kv.range_scan(ts0, k, k);
                    if (one.size() != 1 || one[0].second != "v" + std::to_string(VH))
                        bad.fetch_add(1, std::memory_order_relaxed);
                }
                dst::thread_done();
            });

            // T4 gc: the retirement engine — sever/retire under the pin
            // (blocked), reclaim after (drain). 10 passes: 248 keys < the
            // 256-keys/pass budget, so every pass is a FULL sweep — the
            // first pass under the pin retires everything severable (the
            // witness material), the rest re-prove the block.
            std::thread gct([&] {
                dst::point("start");
                for (int n = 0; n < 10; ++n) {
                    dst::point("gc");
                    try { kv.gc_pass_for_test(); } catch (...) {}
                }
                dst::thread_done();
            });

            churn_a.join(); churn_b.join(); prober.join(); gct.join();
            dst::disarm();
            // Deterministic safety witness, STILL PINNED (main's guard is
            // alive until the scope ends): every gc pass ran under this
            // pin, so retirement must have accumulated and reclamation
            // must have been blocked. Floor NK: at minimum the r-key
            // suffixes below the anchor (VH-1 each = 5*NK = 40 nodes);
            // the EP_PRE e-key v1s add EP_PRE more (expected ~160).
            if (kv.retired_pending() < (uint64_t)NK) bad.fetch_add(1, std::memory_order_relaxed);
            if (kv.range_scan(ts0, LO, HI) != v0)    // final stability, still pinned
                bad.fetch_add(1, std::memory_order_relaxed);
        }   // pin released — the drain oracles below need it gone
        if (bad.load()) {
            fprintf(stderr, "dst S6: %d violations (pin-stability/witness/capture)\n", bad.load());
            return 9;
        }

        // ---- Quiescence drain oracles (the M3 liveness direction). ----
        const auto stats_pre = kv.gc_stats();
        for (int n = 0; n < 8 && kv.retired_pending() != 0; ++n) {
            try { kv.gc_pass_for_test(); } catch (...) {}
        }
        // D1: unpin => drain. Retirement blocked by a pin is safety; a pin
        // RELEASED must unblock it (or the pool ratchets — the defect M3
        // exists to fix, at the version layer where it is observable now).
        if (kv.retired_pending() != 0) {
            fprintf(stderr, "dst S6: retired_pending=%llu after unpin+8 gc passes (drain failed)\n",
                    (unsigned long long)kv.retired_pending());
            return 9;
        }
        // D2: non-vacuity controls — retirement really happened during the
        // run, and reclamation really freed after the unpin (a drain that
        // "passes" because nothing was ever retired is no pass at all).
        const auto stats_post = kv.gc_stats();
        if (stats_post.retired == 0) {
            fprintf(stderr, "dst S6: control failed: nothing was ever retired\n");
            return 9;
        }
        if (stats_post.reclaimed <= stats_pre.reclaimed) {
            fprintf(stderr, "dst S6: control failed: unpin freed nothing (reclaimed %llu -> %llu)\n",
                    (unsigned long long)stats_pre.reclaimed, (unsigned long long)stats_post.reclaimed);
            return 9;
        }
        // D3: differential-vs-std::map at rest.
        {
            auto rs = kv.range_scan(UINT64_MAX, LO, HI);
            if (rs.size() != live.size()) {
                fprintf(stderr, "dst S6: quiescent scan %zu keys, model %zu\n", rs.size(), live.size());
                return 9;
            }
            for (auto& [k, v] : rs) {
                auto it = live.find(k);
                if (it == live.end() || it->second != v) {
                    fprintf(stderr, "dst S6: quiescent differential mismatch at %s\n", k.c_str());
                    return 9;
                }
            }
        }
        // D4: clean run => healthy engine (no fail-stop latch tripped).
        if (kv.health().level != 0) {
            fprintf(stderr, "dst S6: health level %d after clean run\n", kv.health().level);
            return 9;
        }
        return 0;
    }
}   // namespace dstscn
#endif   // CHRONOKV_STRESS

int run_dst_test() {
    using namespace chronokv;
    int fails = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail = "") {
        std::cout << "   " << name << ":  " << (ok ? "PASS" : "FAIL") << "\n";
        if (!ok) {
            ++fails;
            if (!detail.empty()) std::cout << "      (" << detail << ")\n";
        }
    };
#ifndef CHRONOKV_STRESS
    check("dst: deterministic-scheduler harness (skipped — needs CHRONOKV_STRESS)", true);
    std::cout << "   DST TEST PASSED (skipped in this build)\n";
    return fails;
#else
    // In-suite default: a small sweep per scenario — enough to detect a
    // broken harness or an engine regression on every stress run, cheap
    // enough for the always-run suite. The CI dst job scales via
    // CKV_DST_SEEDS (bounded PR / long nightly, roadmap: N=100k total).
    int nseeds = 3;
    if (const char* e = getenv("CKV_DST_SEEDS")) { int v = atoi(e); if (v > 0) nseeds = v; }
    uint64_t seed_base = 0xD57D57ULL;
    if (const char* e = getenv("CKV_DST_SEED")) {
        uint64_t v = strtoull(e, nullptr, 0);
        if (v) seed_base = v;
    }
    std::cout << "    (dst config: seed_base=" << seed_base << " seeds/scenario=" << nseeds << ")\n";

    struct Scn {
        const char* name;
        int (*fn)(uint64_t, dst::Progress*, const std::string&);
    };
    const Scn scns[] = {
        {"wal_mixed_handoff (H3 class)", dstscn::s1_mixed_handoff},
        {"wal_leader_fault (C1 class)",  dstscn::s2_leader_fault},
        {"gc_epoch_scan",                dstscn::s3_gc_scan},
        {"tree_cursor_split",            dstscn::s4_tree_split},
        // v29 M2 item 2: the grammar growth — merge/rebalance/cascade and
        // reclamation-retirement windows, written against the M3/M4
        // designs; green against the current (merge-free, no-page-retire)
        // tree by construction, binding on the implementations when they
        // land. Appended AFTER the v27 four so historical replay handles
        // (scenario indices 0-3 in old failure lines) keep their meaning.
        {"tree_merge_cascade (M4 grammar)",  dstscn::s5_merge_cascade},
        {"reclaim_retire_pin (M3 grammar)",  dstscn::s6_reclaim_retire},
    };

    const int DEADLINE_MS = 20000;   // clean children finish in ~ms-seconds
    for (size_t si = 0; si < sizeof(scns) / sizeof(scns[0]); ++si) {
        int n_hang = 0, n_crash = 0, n_assert = 0, n_pass = 0;
        uint64_t max_ops = 0, max_steals = 0;
        std::vector<std::string> first_fails;
        for (int s = 0; s < nseeds; ++s) {
            const uint64_t seed = seed_base + (uint64_t)si * 1000003ULL +
                                  (uint64_t)s * 2654435761ULL;
            auto* prog = (dst::Progress*)mmap(nullptr, sizeof(dst::Progress),
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (prog == MAP_FAILED) { check("dst: mmap progress page", false, strerror(errno)); break; }
            new (prog) dst::Progress();
            const std::string wd = "/tmp/ckv_dst_" + std::to_string(getpid()) +
                                   "_" + std::to_string(si) + "_" + std::to_string(seed);
            std::filesystem::remove_all(wd);
            std::cout.flush();     // child must not inherit unflushed stdout
            pid_t child = fork();
            if (child < 0) {
                check("dst: fork", false, strerror(errno));
                munmap((void*)prog, sizeof(dst::Progress));
                break;
            }
            if (child == 0) {
                int rc = 3;
                try {
                    rc = scns[si].fn(seed, prog, wd);
                } catch (const std::exception& ex) {
                    fprintf(stderr, "dst child exception: %s\n", ex.what());
                    rc = 8;
                } catch (...) {
                    rc = 8;
                }
                std::error_code ec;
                std::filesystem::remove_all(wd, ec);
                fflush(stderr);
                _exit(rc);
            }
            // Parent: bounded wait, then classify.
            int wst = 0;
            bool reaped = false;
            auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count() < DEADLINE_MS) {
                pid_t r = waitpid(child, &wst, WNOHANG);
                if (r == child) { reaped = true; break; }
                if (r < 0) { reaped = true; wst = 0; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::string outcome;
            if (!reaped) {
                kill(child, SIGKILL);
                waitpid(child, &wst, 0);
                n_hang++;
                outcome = "HANG (deadline " + std::to_string(DEADLINE_MS) + "ms)";
            } else if (WIFSIGNALED(wst)) {
                n_crash++;
                outcome = "CRASH signal " + std::to_string(WTERMSIG(wst));
            } else if (WEXITSTATUS(wst) != 0) {
                n_assert++;
                outcome = "INVARIANT exit " + std::to_string(WEXITSTATUS(wst));
            } else {
                n_pass++;
            }
            uint64_t ops = prog->ops.load(std::memory_order_relaxed);
            uint64_t steals = prog->timeouts.load(std::memory_order_relaxed);
            if (ops > max_ops) max_ops = ops;
            if (steals > max_steals) max_steals = steals;
            if (!outcome.empty() && first_fails.size() < 3) {
                char lname[64] = {0};
                memcpy(lname, prog->last_name, sizeof(lname) - 1);
                first_fails.push_back(std::string(scns[si].name) + " seed=" +
                                      std::to_string(seed) + ": " + outcome +
                                      " ops=" + std::to_string(ops) +
                                      " last_point=" + (lname[0] ? lname : "?") +
                                      " steals=" + std::to_string(steals) +
                                      " — replay: CKV_DST_SEED=" + std::to_string(seed) +
                                      " CKV_DST_SEEDS=1 (scenario " + std::to_string(si) + ")");
            }
            munmap((void*)prog, sizeof(dst::Progress));
            std::error_code ec;
            std::filesystem::remove_all(wd, ec);   // best-effort after a kill
            // 0.27.0 hardening: periodic progress — a slow shard must be
            // diagnosable from its log (which seed window it is grinding
            // on, whether hangs are accumulating) instead of opaque.
            if (nseeds >= 1000 && (s + 1) % 1000 == 0) {
                std::cout << "      (" << scns[si].name << ": " << (s + 1)
                          << "/" << nseeds << " seeds — pass=" << n_pass
                          << " hang=" << n_hang << " crash=" << n_crash
                          << " invariant=" << n_assert << ")" << std::flush;
                std::cout << "\n" << std::flush;
            }
        }
        std::string nm = std::string("dst: ") + scns[si].name + " x " +
                         std::to_string(nseeds) + " seeds";
        bool ok = (n_hang + n_crash + n_assert) == 0 && n_pass == nseeds;
        check(nm.c_str(), ok, first_fails.empty() ? "" : first_fails[0]);
        std::cout << "      (pass=" << n_pass << " hang=" << n_hang << " crash=" << n_crash
                  << " invariant=" << n_assert << ", max child ops=" << max_ops
                  << ", max steals=" << max_steals << ")\n";
        for (size_t f = 1; f < first_fails.size(); ++f)
            std::cout << "      (also: " << first_fails[f] << ")\n";
    }
    if (fails == 0) std::cout << "   DST TEST PASSED\n";
    else std::cout << "   DST FAILURES: " << fails << "\n";
    return fails;
#endif
}
#endif // CHRONOKV_TEST_HOOKS (extracted battery TU)
