// tests/tests_dst.cpp (dstscn namespace + run_dst_test) — v27 M0 deterministic-scheduler harness — extracted from main.cpp (v29 M2 item 5: the test-suite TU
// split, unlocked by API-1's ODR-safe header). Content is byte-identical to
// the extracted region except: the battery entry point loses `static` (it is
// declared in tests/test_decls.hpp and called from main.cpp), and the whole
// body rides under CHRONOKV_TEST_HOOKS exactly as it did inside main.cpp's
// self-tests section. No engine or test logic changed in the move.
#include "chronokv.hpp"
#include "chronokv.hpp"   // API-1: header must be idempotent (#pragma once)
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
