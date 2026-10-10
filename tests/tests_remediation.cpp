// tests/tests_remediation.cpp (a2 namespace + Audit-2 battery + v28 CKV-001..021 battery) — extracted from main.cpp (v29 M2 item 5: the test-suite TU
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
// v28 — audit remediation regression battery (CKV-001..CKV-021).
//
// Every test in this function is fail-first against the audited commit
// 8c77a7c: it asserts the INVARIANT the audit found violated (not merely
// "no crash"), and each was verified failing before its fix landed. Tests
// whose pre-fix failure mode is memory corruption or process death run in
// forked children (the crash-fuzz pattern) so a regression FAILS the check
// instead of taking the suite down.
// =====================================================================
// =====================================================================
// Audit-2 regression battery (TXN-1..BT-2 + EXTRA-2), appended to
// CKV_ONLY_REMEDIATION. Each case is a trimmed PoC body and FAILS on
// HEAD 8863bc8 (fail-first). API-1 is a build check (also exercised by the
// double #include at the top of this file); CI 2-TU link check:
//   printf '#include "chronokv.hpp"\n#include "chronokv.hpp"\nint f1(){return 1;}\n' >a.cpp
//   printf '#include "chronokv.hpp"\nint f1();\nint main(){return f1()-1;}\n' >b.cpp
//   g++ -std=c++20 -I. -pthread a.cpp b.cpp -o ab && ./ab
// EXTRA-1 (latch inside group_append) is covered by pocs run_all.sh's
// widened-window race (needs a scratch header with an injected sleep).
// =====================================================================
namespace a2 {
static std::string mk(const char* tag) {
    std::string t = std::string("/tmp/") + tag + ".XXXXXX";
    std::vector<char> b(t.begin(), t.end()); b.push_back(0);
    if (!mkdtemp(b.data())) throw std::runtime_error("mkdtemp");
    return b.data();
}
static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void spew(const std::string& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
}
static uint32_t le32(const std::vector<uint8_t>& b, size_t p) {
    return (uint32_t)b[p] | ((uint32_t)b[p+1] << 8) | ((uint32_t)b[p+2] << 16) | ((uint32_t)b[p+3] << 24);
}
// fork + deadline: returns child exit code, or -1 on deadline/signal.
static int run_forked(const std::function<int()>& body, int deadline_ms) {
    std::cout.flush();
    pid_t pid = fork();
    if (pid == 0) { int rc = 5; try { rc = body(); } catch (...) { rc = 3; } _exit(rc); }
    auto t0 = std::chrono::steady_clock::now();
    int st = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(deadline_ms)) {
        if (waitpid(pid, &st, WNOHANG) == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        usleep(10000);
    }
    kill(pid, SIGKILL); waitpid(pid, &st, 0);
    return -1;
}
}  // namespace a2

static int run_audit2_regression_tests() {
    using namespace chronokv;
    namespace fs = std::filesystem;
    int fails = 0;
    auto check = [&](const char* name, bool ok, const std::string& d = "") {
        std::cout << "   " << name << ":  " << (ok ? "PASS" : "FAIL") << "\n";
        if (!ok) { ++fails; if (!d.empty()) std::cout << "      (" << d << ")\n"; }
    };
    auto guard = [&](const char* name, const std::function<void()>& fn) {
        try { fn(); } catch (const std::exception& e) { check(name, false, std::string("exception: ") + e.what()); }
    };
    Options mem; mem.page_pool_bytes = 16u << 20;

    // ---- TXN-1: a scanned key UPDATED after the snapshot must conflict.
    guard("A2 TXN-1 scan/update conflict", [&] {
        auto db = Database::open(mem); db.put("k", "0"); db.put("x", "0");
        auto t1 = db.begin(); (void)t1.range_scan("k", "k");
        auto t2 = db.begin(); (void)t2.get("x"); t2.put("k", "1"); Status s2 = t2.commit();
        t1.put("x", "1"); Status s1 = t1.commit();
        check("A2 TXN-1 scan/update conflict", s2 == Status::OK && s1 == Status::Conflict,
              "T2=" + std::to_string((int)s2) + " T1=" + std::to_string((int)s1));
    });
    guard("A2 TXN-1 SSI write-skew still rejected", [&] {
        auto db = Database::open(mem); db.put("a", "1"); db.put("b", "1");
        auto t1 = db.begin(); auto t2 = db.begin();
        (void)t1.get("a"); (void)t1.get("b"); (void)t2.get("a"); (void)t2.get("b");
        t1.put("a", "0"); Status s1 = t1.commit();
        t2.put("b", "0"); Status s2 = t2.commit();
        check("A2 TXN-1 SSI write-skew still rejected", s1 == Status::OK && s2 == Status::Conflict);
    });

    // ---- WAL-1: corrupted length field of an interior frame -> CORRUPT, file untouched.
    guard("A2 WAL-1 corrupt length is not a torn tail", [&] {
        std::string dir = a2::mk("ckv_a2_wal1");
        Options o; o.wal_dir = dir; o.durability = DurabilityMode::Sync; o.page_pool_bytes = 16u << 20;
        { auto db = Database::open(o); db.put("a", "A"); db.put("b", "B"); db.put("c", "C"); db.close(); }
        auto base = a2::slurp(dir + "/wal_000001.log");
        std::vector<size_t> offs; size_t pos = 0;
        while (pos + 8 <= base.size()) { offs.push_back(pos); pos += 8 + a2::le32(base, pos); }
        bool ok = offs.size() >= 3; std::string why;
        auto trial = [&](const char* name, const std::function<void(std::vector<uint8_t>&)>& mut, bool expect_throw) {
            std::string d2 = dir + "_" + name; fs::copy(dir, d2, fs::copy_options::recursive);
            std::string p = d2 + "/wal_000001.log"; auto b = a2::slurp(p); mut(b); a2::spew(p, b);
            size_t before = b.size(); bool threw = false; bool a_ok = false;
            Options o2 = o; o2.wal_dir = d2;
            try { auto db = Database::open(o2); a_ok = db.get("a").has_value() && db.get("b").has_value(); db.close(); }
            catch (const std::exception&) { threw = true; }
            if (expect_throw && (!threw || fs::file_size(p) != before)) { ok = false; why += std::string(name) + " not rejected/file changed; "; }
            if (!expect_throw && (threw || !a_ok)) { ok = false; why += std::string(name) + " torn tail not recovered; "; }
            fs::remove_all(d2);
        };
        if (ok) {
            trial("len_ffffffff", [&](auto& b) { for (int i = 0; i < 4; ++i) b[offs[1] + i] = 0xFF; }, true);
            trial("len_plus1", [&](auto& b) { uint32_t l = a2::le32(b, offs[1]) + 1; for (int i = 0; i < 4; ++i) b[offs[1] + i] = (l >> (8 * i)) & 0xFF; }, true);
            trial("torn_tail", [&](auto& b) { b.resize(offs[2] + 5); }, false);
        }
        fs::remove_all(dir);
        check("A2 WAL-1 corrupt length is not a torn tail", ok, why);
    });

#ifdef CHRONOKV_FAULT_INJECTION
    // ---- WAL-2: creating a segment must fsync the WAL directory before any ack.
    guard("A2 WAL-2 new segment dir fsync", [&] {
        std::string dir = a2::mk("ckv_a2_wal2");
        Options o; o.wal_dir = dir; o.durability = DurabilityMode::Sync; o.page_pool_bytes = 16u << 20;
        ::fault::arm(::fault::Kind::DirFsyncFail, 1);
        Status s = Status::OK; bool threw = false;
        try { auto db = Database::open(o); s = db.put("a", "1"); db.close(); } catch (const std::exception&) { threw = true; }
        int rem = ::fault::remaining.load(); ::fault::disarm(); fs::remove_all(dir);
        check("A2 WAL-2 new segment dir fsync", rem == 0 && (threw || s != Status::OK),
              "remaining=" + std::to_string(rem) + " put=" + std::to_string((int)s));
    });
#endif

    // ---- TXN-2: negative lookups must not consume the index/pool; check-then-insert still conflicts.
    guard("A2 TXN-2 absent reads do not leak", [&] {
        Options o; o.page_pool_bytes = 1u << 20; auto db = Database::open(o); db.put("w", "0");
        int i = 0; bool ok = true;
        for (; i < 30000; ++i) {
            auto t = db.begin(); (void)t.get("absent_user_" + std::to_string(i)); t.put("w", std::to_string(i));
            if (t.commit() != Status::OK) { ok = false; break; }
        }
        check("A2 TXN-2 absent reads do not leak", ok, "stopped at " + std::to_string(i));
    });
    guard("A2 TXN-2 check-then-insert conflicts", [&] {
        auto db = Database::open(mem); db.put("w", "0");
        auto t1 = db.begin(); bool absent = !t1.get("user_x").has_value();
        auto t2 = db.begin(); t2.put("user_x", "bob"); Status s2 = t2.commit();
        t1.put("w", "1"); Status s1 = t1.commit();
        check("A2 TXN-2 check-then-insert conflicts", absent && s2 == Status::OK && s1 == Status::Conflict,
              "T1=" + std::to_string((int)s1));
    });

    // ---- BT-1: pool exhaustion during a split must not lose acked keys.
    guard("A2 BT-1 exhaustion keeps acked keys", [&] {
        int bad = 0, combos = 0;
        for (int shuffled = 0; shuffled < 2; ++shuffled)
        for (size_t pages : {2, 3, 4, 5, 8, 12})
        for (size_t S : {2000, 1000, 300, 100}) {
            chronokv_page::PagePool pool(pages * 4096); chronokv_btree::BTree t(pool);
            std::vector<std::string> keys;
            for (int i = 0; i < 4000; ++i) { char b[16]; snprintf(b, sizeof b, "%06d", i); std::string k = b; k.resize(S, 'x'); keys.push_back(k); }
            if (shuffled) std::shuffle(keys.begin(), keys.end(), std::mt19937(7));
            std::vector<std::string> ok;
            bool threw = false;
            for (auto& k : keys) { try { t.put(k, "v"); ok.push_back(k); } catch (const std::bad_alloc&) { threw = true; break; } }
            if (!threw) continue;
            ++combos;
            for (auto& k : ok) { std::string out; if (!t.get(k, &out)) { ++bad; break; } }
        }
        check("A2 BT-1 exhaustion keeps acked keys", combos > 0 && bad == 0,
              std::to_string(bad) + " of " + std::to_string(combos) + " combos lost keys");
    });

    // ---- WAL-4: absurd MANIFEST active_id must fail fast (pre-fix: ~2^64 loop).
    guard("A2 WAL-4 huge active_id fails fast", [&] {
        std::string base = a2::mk("ckv_a2_wal4"), wal = base + "/wal", ck = base + "/ck";
        fs::create_directories(wal); fs::create_directories(ck);
        Options o; o.wal_dir = wal; o.checkpoint_path = ck + "/ckpt"; o.durability = DurabilityMode::Sync; o.page_pool_bytes = 16u << 20;
        { auto db = Database::open(o); db.put("a", "1"); db.checkpoint(); db.put("b", "2"); db.close(); }
        auto mf = a2::slurp(wal + "/MANIFEST");
        bool ok = true; std::string why;
        auto variant = [&](const char* name, uint64_t active, int want) {
            std::string d2 = base + "_" + name; fs::copy(base, d2, fs::copy_options::recursive);
            auto b = mf; for (int i = 0; i < 8; ++i) b[8 + i] = (active >> (8 * i)) & 0xFF;
            uint32_t c = ::crc32(b.data() + 8, b.size() - 8); for (int i = 0; i < 4; ++i) b[4 + i] = (c >> (8 * i)) & 0xFF;
            a2::spew(d2 + "/wal/MANIFEST", b);
            Options o2 = o; o2.wal_dir = d2 + "/wal"; o2.checkpoint_path = d2 + "/ck/ckpt";
            int rc = a2::run_forked([&] { auto db = Database::open(o2); return 0; }, 5000);
            if (rc != want) { ok = false; why += std::string(name) + " rc=" + std::to_string(rc) + "; "; }
            fs::remove_all(d2);
        };
        uint64_t cur = 0; for (int i = 0; i < 8; ++i) cur |= (uint64_t)mf[8 + i] << (8 * i);
        variant("control", cur, 0);
        variant("huge", 100000000ULL, 3);
        variant("u64max", UINT64_MAX, 3);
        fs::remove_all(base);
        check("A2 WAL-4 huge active_id fails fast", ok, why);
    });

    // ---- API-3: observers follow a moved Database.
    guard("A2 API-3 observers survive move", [&] {
        int cb1 = 0, cb2 = 0;
        { auto a = Database::open(mem); auto h = a.observe("k", [&](auto&&...) { cb1++; });
          Database b = std::move(a); b.put("k", "v"); }
        { auto a = Database::open(mem); auto h = a.observe("k", [&](auto&&...) { cb2++; });
          Database b = Database::open(mem); b = std::move(a); b.put("k", "v"); }
        check("A2 API-3 observers survive move", cb1 == 1 && cb2 == 1,
              "construct=" + std::to_string(cb1) + " assign=" + std::to_string(cb2));
    });

    // ---- TXN-3: close() with a live txn must not pin its reader registration.
    guard("A2 TXN-3 close with live txn unpins", [&] {
        auto db = Database::open(mem); db.put("a", "1");
        std::shared_ptr<ChronoKV> eng = db.engine_for_test();
        { auto txn = db.begin(); (void)txn.get("a"); (void)txn.range_scan("a", "z");
          db.close(); try { txn.abort(); } catch (const std::exception&) {} }
        size_t pinned = eng->phantom_reader_count_for_test();
        check("A2 TXN-3 close with live txn unpins", pinned == 0, "pinned=" + std::to_string(pinned));
    });

    // ---- BT-2: standalone BTree concurrent writers lose nothing.
    guard("A2 BT-2 concurrent writers", [&] {
        chronokv_page::PagePool pool(128ull << 20); chronokv_btree::BTree t(pool);
        auto key = [](int th, int n) { char b[40]; snprintf(b, sizeof b, "t%02d_%08d", th, n); return std::string(b); };
        std::vector<std::thread> w;
        for (int th = 0; th < 4; ++th) w.emplace_back([&, th] { for (int n = 0; n < 20000; ++n) t.put(key(th, n), "v"); });
        for (auto& x : w) x.join();
        size_t miss = 0;
        for (int th = 0; th < 4; ++th) for (int n = 0; n < 20000; ++n) { std::string o; if (!t.get(key(th, n), &o)) ++miss; }
        check("A2 BT-2 concurrent writers", miss == 0, "missing=" + std::to_string(miss));
    });

    // ---- EXTRA-2: growing a value past free_hi must not corrupt other pages.
    guard("A2 EXTRA-2 update-path wrap", [&] {
        chronokv_page::PagePool pool(64ull << 20); chronokv_btree::BTree t(pool);
        auto val = [](int i) { std::string v(100, (char)('a' + (i % 26))); v += std::to_string(i); return v; };
        char kb[16];
        for (int i = 0; i < 3000; ++i) { snprintf(kb, sizeof kb, "k%06d", i); t.put(kb, val(i)); }
        t.put("k000000", std::string(2500, 'Z'));
        size_t bad = 0;
        for (int i = 1; i < 3000; ++i) { snprintf(kb, sizeof kb, "k%06d", i); std::string o; if (!t.get(kb, &o) || o != val(i)) ++bad; }
        std::string o; bool f = t.get("k000000", &o) && o.size() == 2500;
        check("A2 EXTRA-2 update-path wrap", bad == 0 && f, std::to_string(bad) + " other keys corrupt");
    });

#ifdef CHRONOKV_FAULT_INJECTION
    // ---- WAL-2 retry: a segment left EMPTY by a failed dir fsync must be synced again on the next open.
    guard("A2 WAL-2 retry after failed dir fsync", [&] {
        std::string dir = a2::mk("ckv_a2_wal2r");
        Options o; o.wal_dir = dir; o.durability = DurabilityMode::Sync; o.page_pool_bytes = 16u << 20; o.auto_start_gc = false;
        ::fault::arm(::fault::Kind::DirFsyncFail, 1);
        { try { auto db = Database::open(o); (void)db.put("a", "1"); db.close(); } catch (const std::exception&) {} }
        int r1 = ::fault::remaining.load();
        ::fault::arm(::fault::Kind::DirFsyncFail, 1);
        { try { auto db = Database::open(o); (void)db.put("a", "1"); db.close(); } catch (const std::exception&) {} }
        int r2 = ::fault::remaining.load(); ::fault::disarm(); fs::remove_all(dir);
        check("A2 WAL-2 retry after failed dir fsync", r1 == 0 && r2 == 0,
              "first remaining=" + std::to_string(r1) + " second remaining=" + std::to_string(r2));
    });
#ifdef CHRONOKV_TEST_HOOKS
    // ---- EXTRA-1: the fail-stop latch must already be set when group_append releases batch_mu_.
    guard("A2 EXTRA-1 latch set before unlock", [&] {
        std::string dir = a2::mk("ckv_a2_x1");
        Options o; o.wal_dir = dir; o.durability = DurabilityMode::Sync; o.page_pool_bytes = 16u << 20; o.auto_start_gc = false;
        bool ok = true, b_acked = false; std::string why;
        {
            auto db = Database::open(o);
            (void)db.put("pre", "1");
            std::atomic<bool> in_hook{false}, release{false};
            ckv_post_reservation_throw_hook_for_test() = [&] { in_hook = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
            ::fault::arm(::fault::Kind::AllocFail, 1);
            std::thread ta([&] { try { (void)db.put("A", "1"); } catch (...) {} });
            auto t0 = std::chrono::steady_clock::now();
            while (!in_hook && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!in_hook) { ok = false; why = "hook never reached (AllocFail not consumed in the reservation window)"; }
            else {
                // A has thrown and unwound out of group_append (batch_mu_ free) but has NOT run commit_txn's latch yet.
                Status sb = db.put("B", "1");
                b_acked = (sb == Status::OK);
                if (b_acked) { ok = false; why = "B was acknowledged past the burned timestamp"; }
            }
            release = true; ta.join(); ckv_post_reservation_throw_hook_for_test() = nullptr; ::fault::disarm();
            db.close();
        }
        bool reopened = false;
        try { auto db = Database::open(o); reopened = db.get("pre").has_value() && (!b_acked || db.get("B").has_value()); db.close(); }
        catch (const std::exception& e) { why += std::string(" reopen threw: ") + e.what(); }
        fs::remove_all(dir);
        check("A2 EXTRA-1 latch set before unlock", ok && reopened, why);
    });
#endif
#endif

    guard("A2 BT-1 in-place update after exhaustion", [&] {
        chronokv_page::PagePool pool(16 * 4096); chronokv_btree::BTree t(pool);
        std::string first; size_t n = 0;
        for (; n < 100000; ++n) { char b[24]; snprintf(b, sizeof b, "%08zu", n); std::string k = b; k.resize(100, 'x'); try { t.put(k, "v"); if (n == 0) first = k; } catch (const std::bad_alloc&) { break; } }
        bool upd_ok = true; try { t.put(first, "w"); } catch (const std::bad_alloc&) { upd_ok = false; }
        // 0.28.1 (external-review probe): put_with_old PARITY — the in-place
        // exemption must hold on BOTH standalone tree write entry points;
        // put_with_old already holds the old value, so its check is free.
        // Fail-first: pre-fix this threw bad_alloc on the exhausted pool
        // despite the update needing ZERO allocation.
        bool upd_old_ok = true; std::string oldv;
        try { t.put_with_old(first, "z", &oldv); } catch (const std::bad_alloc&) { upd_old_ok = false; }
        std::string out; bool readable = t.get(first, &out) && out == "z";
        check("A2 BT-1 in-place update after exhaustion",
              n > 0 && upd_ok && upd_old_ok && readable && oldv == "w",
              "n=" + std::to_string(n) + " put_ok=" + std::to_string(upd_ok) +
              " put_with_old_ok=" + std::to_string(upd_old_ok) + " oldv=" + oldv);
    });

    std::cout << (fails == 0 ? "   AUDIT-2 REGRESSION TESTS PASSED\n"
                             : "   AUDIT-2 REGRESSION FAILURES: " + std::to_string(fails) + "\n");
    return fails;
}

static int run_remediation_tests_v28();
int run_remediation_tests() {
    int f = run_remediation_tests_v28();
    f += run_audit2_regression_tests();
    return f;
}
static int run_remediation_tests_v28() {
    using namespace chronokv;
    int fails = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail = "") {
        std::cout << "   " << name << ":  " << (ok ? "PASS" : "FAIL") << "\n";
        if (!ok) {
            ++fails;
            if (!detail.empty()) std::cout << "      (" << detail << ")\n";
        }
    };
    // Forked-child runner with a parent-side deadline: the child returns 0
    // on success; anything else (nonzero exit, signal, deadline) is a FAIL
    // with the classification in the detail string. Pre-fix children die by
    // corruption — that is the fail-first evidence, safely contained.
    auto run_child = [&](const char* name, int (*body)(), int deadline_ms,
                         const std::string& expect_note) {
        std::cout.flush();
        pid_t child = fork();
        if (child < 0) { check(name, false, std::string("fork failed: ") + strerror(errno)); return; }
        if (child == 0) {
            int rc = 3;
            try { rc = body(); }
            catch (const std::exception& e) { fprintf(stderr, "child exception: %s\n", e.what()); rc = 5; }
            catch (...) { rc = 5; }
            fflush(stderr);
            _exit(rc);
        }
        int wst = 0;
        bool reaped = false;
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count() < deadline_ms) {
            pid_t r = waitpid(child, &wst, WNOHANG);
            if (r == child) { reaped = true; break; }
            if (r < 0) { reaped = true; wst = 0; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::string fr;
        bool ok;
        if (!reaped) {
            kill(child, SIGKILL);
            waitpid(child, &wst, 0);
            ok = false; fr = "child HUNG (deadline " + std::to_string(deadline_ms) + "ms)";
        } else if (WIFSIGNALED(wst)) {
            ok = false; fr = "child died on signal " + std::to_string(WTERMSIG(wst));
        } else if (WEXITSTATUS(wst) != 0) {
            ok = false; fr = "child exit " + std::to_string(WEXITSTATUS(wst));
        } else {
            ok = true;
        }
        if (!ok) fr += " — expected pre-fix: " + expect_note;
        check(name, ok, fr);
    };

    // ---- CKV-001: tree-oversized-key-rejection ----
    // Invariant: a key that cannot physically fit a B+ tree page is
    // rejected with Status::TooLarge BEFORE any reservation/WAL/index
    // mutation, and the tree stays verifiably healthy. Pre-fix: keys in
    // (4044, 65535] passed the WAL-framing gate and wrapped the uint16
    // slab offsets in the split-rebuild writers — a 60,000-byte key
    // memcpy'd ~55 KB past its 4 KiB page (audit PoC: SEGV / silent
    // cross-page corruption from ONE legal put).
    static int (*const ckv001_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv001_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.auto_start_gc = false;
        o.durability = DurabilityMode::Sync;
        auto db = Database::open(o);
        // (a) boundary-legal key (exactly TREE_MAX_KEY_BYTES = 4096-32-8-8):
        // accepted and byte-exact round-trip.
        const std::string k4048(4048, 'K');
        if (db.put(k4048, "v") != Status::OK) return 10;
        if (db.get(k4048).value_or("") != "v") return 11;
        // (b) over-bound matrix: TooLarge, zero mutation (get stays absent).
        for (size_t sz : {(size_t)4049, (size_t)4090, (size_t)4096, (size_t)60000, (size_t)65535}) {
            const std::string k(sz, 'X');
            Status st = db.put(k, "v");
            if (st != Status::TooLarge) return 12;
            if (db.get(k).has_value()) return 13;
        }
        // (c) same gate through Transaction and Batch.
        {
            auto t = db.begin();
            t.put(std::string(5000, 'Y'), "v");
            if (t.commit() != Status::TooLarge) return 14;
        }
        {
            auto b = db.create_batch();
            b.put(std::string(5000, 'Z'), "v");
            if (b.commit() != Status::TooLarge) return 15;
        }
        // (d) tree health after the rejections: another boundary-ish key
        //     inserts, range_scan sees both survivors, and the state
        //     persists across close+reopen (recovery replays the WAL).
        const std::string k4000(4000, 'B');
        if (db.put(k4000, "b") != Status::OK) return 16;
        {
            auto scan = db.range_scan(std::string(4000, 'A'), std::string(4048, 'Z'));
            if (scan.size() != 2) return 17;
        }
        db.close();
        {
            auto db2 = Database::open(o);
            if (db2.get(k4048).value_or("") != "v") return 18;
            if (db2.get(k4000).value_or("") != "b") return 19;
            if (db2.get(std::string(60000, 'X')).has_value()) return 20;
            db2.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-001: oversized keys rejected TooLarge, zero mutation, tree healthy (tree-oversized-key-rejection)",
              ckv001_body, 120000,
              "put(4049..65535) accepted (not TooLarge) and the split-rebuild path corrupts memory");

    // ---- CKV-002: byte-aware splitting (engine, end-to-end + recovery) ----
    // Two ~2000-byte keys sharing a leaf plus one page-filling 4048-byte
    // key between them: entry costs 2016 + 4064 + 2016 = 8096 against a
    // 4064-byte per-page budget admit NO valid two-way split — the fix must
    // plan a three-page cascade, allocate it up front, and only then
    // mutate. Pre-fix: the count-based split puts {BIG, B} in one page;
    // B does not fit behind BIG, and the rebuild discovers that AFTER
    // destroying the original page (Blocker 1) — loud throw (post-CKV-001
    // guard) or uint16-wrapped OOB memcpy (raw baseline), with the tree
    // left half-rewritten either way.
    static int (*const ckv002a_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv002a_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.auto_start_gc = false;
        o.durability = DurabilityMode::Sync;
        auto db = Database::open(o);
        const std::string A(2000, 'a'), BIG(4048, 'm'), B(2000, 'z');
        if (db.put(A, "va") != Status::OK) return 10;
        if (db.put(B, "vb") != Status::OK) return 11;
        if (db.put(BIG, "vbig") != Status::OK) return 12;   // pre-fix: throws/corrupts
        if (db.get(A).value_or("") != "va") return 13;
        if (db.get(BIG).value_or("") != "vbig") return 14;
        if (db.get(B).value_or("") != "vb") return 15;
        if (db.range_scan(A, B).size() != 3) return 16;
        db.close();
        {   // recovery: the cascade must survive checkpoint-free WAL replay
            auto db2 = Database::open(o);
            if (db2.get(A).value_or("") != "va") return 17;
            if (db2.get(BIG).value_or("") != "vbig") return 18;
            if (db2.get(B).value_or("") != "vb") return 19;
            if (db2.range_scan(A, B).size() != 3) return 20;
            db2.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-002a: engine 3-page leaf cascade for mixed large keys + reopen (byte-aware-split-cascade)",
              ckv002a_body, 120000,
              "count-based split discovers mid-rebuild that the right half cannot fit");

    // ---- CKV-002 / Blocker 3: byte-aware INTERIOR split with large separators ----
    // Build a root-level interior page byte-full of ~100-byte separators,
    // then promote a 4048-byte separator into it. Pre-fix: count-based
    // mid lands the big separator together with ~half the small ones in
    // one half — over budget — and the rebuild wraps uint16 offsets
    // (raw baseline) or throws AFTER memsetting the original page
    // (post-CKV-001 guard). Post-fix: the planner promotes the big entry
    // itself (or cascades) with every page proven to fit BEFORE mutation.
    static int (*const ckv002b_body)() = +[]() -> int {
        using namespace chronokv_btree;
        PagePool pool(256ULL * 1024 * 1024);
        BTree tree(pool);
        char buf[32];
        const std::string pad(92, 'x');
        int n = 0;
        for (int leaf = 0; leaf < 40; ++leaf) {
            for (int i = 0; i < 40; ++i) {
                snprintf(buf, sizeof buf, "k%07d", n);
                tree.put(std::string(buf) + pad, "v");
                ++n;
            }
        }
        auto sv = tree.verify_separator_invariants();
        if (sv.found) return 10;
        auto cv = tree.verify_leaf_chain();
        if (cv.found) return 11;
        const std::string big(4048, 'm');   // sorts after every k-key
        tree.put(big, "big");                // promotes a 4048B separator upward
        sv = tree.verify_separator_invariants();
        if (sv.found) return 12;
        cv = tree.verify_leaf_chain();
        if (cv.found) return 13;
        auto cc = tree.verify_chain_completeness();
        if (cc.found) return 14;
        std::string got;
        if (!tree.get(big, &got) || got != "big") return 15;
        snprintf(buf, sizeof buf, "k%07d", n / 2);
        if (!tree.get(std::string(buf) + pad, &got) || got != "v") return 16;
        if (tree.size() != (size_t)n + 1) return 17;
        // a second big separator, forcing another byte-full interior split
        const std::string big2(4048, 'n');
        tree.put(big2, "big2");
        sv = tree.verify_separator_invariants();
        if (sv.found) return 18;
        cc = tree.verify_chain_completeness();
        if (cc.found) return 19;
        if (!tree.get(big, &got) || got != "big") return 20;
        if (!tree.get(big2, &got) || got != "big2") return 21;
        return 0;
    };
    run_child("remediation CKV-002b: interior split with 4048-byte separators (interior-byte-aware-split)",
              ckv002b_body, 300000,
              "count-based interior split overflows the half containing the big separator");

    // ---- CKV-002 / Blocker 1: rejection during planning leaves the tree INTACT ----
    // A standalone-tree entry that can never fit a page must be rejected
    // during the PLAN phase — before any page mutation. Pre-fix the
    // rejection happens inside the half-destroyed rebuild (original page
    // already memset to the left half): the neighbor key is LOST. This is
    // the exception-safety contract of RULES 1/2/6.
    static int (*const ckv002c_body)() = +[]() -> int {
        using namespace chronokv_btree;
        PagePool pool(16ULL * 1024 * 1024);
        BTree tree(pool);
        tree.put("alpha", "1");
        tree.put("beta", "2");
        bool threw = false;
        try {
            tree.put(std::string(5000, 'x'), "v");
        } catch (const PageCapacityError&) {
            threw = true;
        }
        if (!threw) return 10;
        std::string got;
        if (!tree.get("alpha", &got) || got != "1") return 11;   // pre-fix: LOST
        if (!tree.get("beta", &got) || got != "2") return 12;    // pre-fix: LOST
        auto cv = tree.verify_leaf_chain();
        if (cv.found) return 13;
        auto cc = tree.verify_chain_completeness();
        if (cc.found) return 14;
        // exact standalone boundary: any leaf key can be promoted to an
        // interior separator (slot 12 + key <= 4064), so the tree-level
        // key bound is 4052 — one byte more must throw with the tree
        // intact.
        tree.put(std::string(4052, 'y'), "v");
        if (!tree.get(std::string(4052, 'y'), &got) || got != "v") return 15;
        bool threw2 = false;
        try { tree.put(std::string(4053, 'z'), "v"); }
        catch (const PageCapacityError&) { threw2 = true; }
        if (!threw2) return 16;
        if (!tree.get("alpha", &got) || got != "1") return 17;
        cv = tree.verify_leaf_chain();
        if (cv.found) return 18;
        return 0;
    };
    run_child("remediation CKV-002c: un-storable entry rejected during planning, tree intact (split-plan-atomicity)",
              ckv002c_body, 120000,
              "rejection fires mid-rebuild; original page already destroyed, neighbors lost");

    // ---- CKV-002 / Blocker 2: ordinary splits stay BYTE-BALANCED ----
    // Uniform keys, repeated splits: average page occupancy must stay
    // healthy (>= 50%). A greedy maximal-left split would leave the right
    // page nearly empty and, with the fixed non-reclaiming pool, tank
    // capacity. NOTE: this property holds for the old count-based split
    // too (uniform keys => count-balanced == byte-balanced) — it PASSES
    // pre-fix BY DESIGN and exists to constrain the NEW planner (Blocker 2
    // forbids replacing balanced two-way splits with greedy packing).
    static int (*const ckv002d_body)() = +[]() -> int {
        using namespace chronokv_btree;
        PagePool pool(512ULL * 1024 * 1024);
        BTree tree(pool);
        char buf[32];
        const int N = 20000;
        const size_t entry_cost = 8 /*slot*/ + 20 /*key*/ + 20 /*value*/;
        // Part 1 — RANDOM insertion order: byte-balanced splitting keeps
        // average occupancy at the B+ tree's ~69%; greedy maximal-left
        // splitting leaves near-empty right pages and drops well below.
        std::vector<int> order(N);
        for (int i = 0; i < N; ++i) order[i] = i;
        std::mt19937_64 rng(0xC0FFEE);   // fixed seed — deterministic
        std::shuffle(order.begin(), order.end(), rng);
        for (int i = 0; i < N; ++i) {
            snprintf(buf, sizeof buf, "uniform-key-%07d", order[i] * 2);  // evens
            tree.put(buf, "01234567890123456789");
        }
        const size_t pages1 = pool.allocated() / 4096;
        const double occ = (double)N * (double)entry_cost / ((double)pages1 * 4064.0);
        if (occ < 0.55) return 10;
        if (tree.size() != (size_t)N) return 11;
        // Part 2 — the greedy pathology proper: with maximal-left splits
        // the left page comes out ~100% full, so EVERY further insert into
        // an established range re-splits immediately (~1 page per insert).
        // Byte-balanced splits leave ~50% headroom, so page growth is
        // amortized (~1 page per 40+ inserts). Insert the 19,999 odd keys
        // between the even ones and bound the growth.
        const size_t M = 20000;
        for (int i = 1; i < 2 * N; i += 2) {   // odds: each lands INSIDE an established range
            snprintf(buf, sizeof buf, "uniform-key-%07d", i);
            tree.put(buf, "01234567890123456789");
        }
        const size_t pages2 = pool.allocated() / 4096;
        const size_t growth = pages2 - pages1;
        if (growth > M / 10) return 14;    // greedy: ~M; balanced: ~M/42
        if (tree.size() != (size_t)2 * N) return 15;
        auto sv = tree.verify_separator_invariants();
        if (sv.found) return 12;
        auto cc = tree.verify_chain_completeness();
        if (cc.found) return 13;
        return 0;
    };
    run_child("remediation CKV-002d: uniform-key splits stay byte-balanced (occupancy >= 50%) (split-balance-occupancy)",
              ckv002d_body, 300000,
              "n/a pre-fix (constraint on the new planner — passes by design on both)");

    // ---- CKV-002: leaf split with a large key among small neighbors (PoC shape) ----
    // The audit PoC: small keys + one large legal key. Pre-fix the right
    // half {8 small, big} exceeds the budget and the rebuild wraps/throws
    // after destroying the page; post-fix the balanced planner picks the
    // only valid split (all smalls left, big right) and every verifier
    // stays green through follow-up inserts and scans.
    static int (*const ckv002e_body)() = +[]() -> int {
        using namespace chronokv_btree;
        PagePool pool(64ULL * 1024 * 1024);
        BTree tree(pool);
        char buf[32];
        // 24 smalls (cost 18 each = 432) + big (cost 3911): total 4343 >
        // 4064. The count-based mid (12) puts 12 smalls + big in the right
        // half: 216 + 3911 = 4127 > 4064 — overflow discovered AFTER the
        // original page was memset and rebuilt. The byte-aware planner
        // picks the only valid balanced split (16 smalls | 8 smalls + big).
        for (int i = 0; i < 24; ++i) {
            snprintf(buf, sizeof buf, "before-%02d", i);
            tree.put(buf, "x");
        }
        tree.put(std::string(3900, 'm'), "big");
        for (int i = 0; i < 24; ++i) {
            snprintf(buf, sizeof buf, "zafter-%02d", i);
            tree.put(buf, "x");
        }
        auto sv = tree.verify_separator_invariants();
        if (sv.found) return 10;
        auto cv = tree.verify_leaf_chain();
        if (cv.found) return 11;
        auto cc = tree.verify_chain_completeness();
        if (cc.found) return 12;
        std::string got;
        if (!tree.get(std::string(3900, 'm'), &got) || got != "big") return 13;
        if (!tree.get("before-07", &got) || got != "x") return 14;
        if (!tree.get("zafter-23", &got) || got != "x") return 15;
        auto scan = tree.range_scan("before-00", "zafter-23");
        if (scan.size() != 49) return 16;
        if (tree.size() != 49) return 17;
        return 0;
    };
    run_child("remediation CKV-002e: large legal key splits leaf among small neighbors (leaf-split-large-key)",
              ckv002e_body, 120000,
              "right half {smalls, big} overflows; rebuild wraps offsets / throws post-destroy");

    // ---- CKV-003b: index fail-stop, invariant D4 (engine-index-failstop) ----
    // The audit probe14 chain, end to end: bounded page pool -> many acked
    // Sync commits -> pool exhaustion throws bad_alloc out of ensure_index
    // DURING a commit. Pre-fix the engine stayed "healthy": the next put
    // returned OK against the diverged index, checkpoint() persisted the
    // diverged tree and TRUNCATED THE WAL, and every acknowledged write was
    // permanently lost on reopen (19,677 commits in the audit PoC).
    // Post-fix (D4): the failure latches index_failed_ -> health() level 2,
    // subsequent commits return Failed, checkpoint() throws, and reopen
    // replays the intact WAL with every acked key present.
    static int (*const ckv003b_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv003b_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.checkpoint_path = wd + "/ckpt.bin";
        o.auto_start_gc = false;
        o.durability = DurabilityMode::Sync;
        o.page_pool_bytes = 1ULL * 1024 * 1024;   // bounded: exhausts mid-ensure_index
        auto db = Database::open(o);
        std::vector<std::string> acked_keys;
        char buf[64];
        bool saw_failure = false;
        for (int i = 0; i < 12000 && !saw_failure; ++i) {
            snprintf(buf, sizeof buf, "key-%08d", i);
            std::string k(buf);
            if (i % 50 == 49) k += std::string(1900, 'p');   // burn pages via splits
            try {
                if (db.put(k, "value") != Status::OK) return 10;
                acked_keys.push_back(k);
            } catch (const std::exception&) {
                saw_failure = true;   // pool exhaustion -> bad_alloc from the tree
            }
        }
        if (!saw_failure) return 11;             // pool not small enough — test vacuous
        if (db.health().level != 2) return 12;   // (a) fail-stop must be reported
        {   // (b) subsequent commits must NOT succeed (pre-fix: returned OK
            // against the diverged index)
            Status st = Status::OK;
            try { st = db.put("post-oom", "v"); }
            catch (const std::exception&) { st = Status::Failed; }
            if (st != Status::Failed) return 13;
        }
        {   // (c) checkpoint must be REFUSED (pre-fix: succeeded and
            // truncated the WAL — the loss event)
            bool ckpt_threw = false;
            try { db.checkpoint(); }
            catch (const std::exception&) { ckpt_threw = true; }
            if (!ckpt_threw) return 14;
        }
        db.close();
        {   // (d) reopen: every acknowledged write must be present
            Options o2 = o;
            o2.page_pool_bytes = 64ULL * 1024 * 1024;   // replay headroom
            auto db2 = Database::open(o2);
            for (const auto& k : acked_keys)
                if (db2.get(k).value_or("") != "value") return 15;
            if (db2.get("post-oom").has_value()) return 16;
            db2.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-003b: index fail-stop on tree-mutation OOM; acked writes survive reopen (engine-index-failstop, invariant D4)",
              ckv003b_body, 900000,
              "post-OOM put returns OK, checkpoint truncates the WAL, acked writes lost on reopen");

    // ---- CKV-012: burn-on-throw in group_append's reservation window ----
    // (wal-reserve-burn-on-throw, publication-prefix progress). Pre-fix, an
    // exception escaping the window between the cts reservation and the
    // record enqueue (OOM-class: records.push_back / make_shared bad_alloc)
    // left the cts neither completed nor burned. The published prefix then
    // stalled below it forever, and the v27 strict-serializability barrier
    // (await_published) hangs EVERY later commit — a permanent silent wedge.
    // Injection: the AllocFail fault kind (§12's chosen mechanism) throws
    // bad_alloc at the enqueue site. Thread A's commit must surface as a
    // failure; thread B's later commit must complete within a bounded wait
    // (the burned hole advanced the prefix). Pre-fix — with the injection
    // point present but the burn missing (the mutation variant) — B blocks
    // forever and the bounded wait fails the test; without the injection
    // point the leg cannot arm (charge-fired assertion fails).
    static int (*const ckv012_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv012_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.durability = DurabilityMode::Sync;
        o.auto_start_gc = false;
        auto db = Database::open(o);
        if (db.put("seed", "v") != Status::OK) return 10;

        fault::arm(fault::Kind::AllocFail, 1);
        Status st_a = Status::OK;
        try { st_a = db.put("a-key", "av"); } catch (const std::exception&) { st_a = Status::Failed; }
        bool fired = fault::remaining.load() == 0;   // sample BEFORE disarm
        fault::disarm();
        if (!fired) return 11;                       // injection never reached — vacuous
        if (st_a == Status::OK) return 12;           // the throwing commit must NOT ack

        // Thread B: a LATER commit on a different key. Pre-CKV-012 it
        // wedged at await_published (prefix stalled below A's orphaned
        // cts); the wait is bounded at 5s, matching the lincheck barrier
        // convention.
        // v28 CKV-012R (Phase 2): B must complete bounded BUT FAIL — the
        // reservation-window throw now latches the WAL fail-stop (D3
        // model). The burn leaves an INTERIOR cts hole with NO WAL frame
        // (the noop-frame contiguity device the validation-conflict burn
        // uses is exactly what an OOM-class throw cannot safely write — it
        // would itself have to allocate). Letting later commits proceed
        // wrote HIGHER cts values past that hole, and recovery's
        // interior-gap check rejected the WHOLE directory at the next
        // open: a transient OOM silently doomed the database and every
        // later ACKED write became unreachable — a durability-contract
        // violation the old NOTE below documented but did not fix. With
        // the latch, no higher cts is ever written: the on-disk WAL stays
        // a contiguous prefix, reopen recovers everything acked before the
        // OOM, and the fresh instance is writable again.
        std::atomic<bool> b_done{false};
        std::atomic<int> b_status{0};
        std::thread tb([&] {
            Status st = Status::OK;
            try { st = db.put("b-key", "bv"); } catch (const std::exception&) { st = Status::Failed; }
            b_status.store((int)st);
            b_done.store(true);
        });
        for (int i = 0; i < 500 && !b_done.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!b_done.load()) { tb.detach(); return 13; }   // WEDGED: later commit hangs
        tb.join();
        if (b_status.load() == (int)Status::OK) return 14;   // fail-stop NOT latched: B wrote past the hole
        if (b_status.load() != (int)Status::Failed) return 15; // expected DatabaseFailed -> Failed
        if (db.health().level < 1) return 20;               // fail-stop must be observable

        // Reopen leg (new in CKV-012R — the old NOTE deliberately skipped
        // it): the directory must open CLEANLY (the WAL is a contiguous
        // prefix — nothing was written past the frameless hole), everything
        // acked before the OOM is recovered, nothing unacked appears, and
        // the fresh instance is writable.
        db.close();
        {
            Options o2 = o;
            auto db2 = Database::open(o2);
            if (db2.get("seed").value_or("") != "v") return 16;   // acked pre-OOM: recovered
            if (db2.get("a-key").has_value()) return 17;          // A never acked: absent
            if (db2.get("b-key").has_value()) return 18;          // B never acked: absent
            if (db2.put("c-key", "cv") != Status::OK) return 19;  // writable again
            if (db2.get("c-key").value_or("") != "cv") return 19;
            db2.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-012(+R): reservation-window throw burns the cts, later commits neither wedge nor write past the hole, reopen is clean (wal-reserve-burn-on-throw)",
              ckv012_body, 60000,
              "pre-CKV-012: orphaned cts wedges every later commit at the barrier; pre-CKV-012R: a later commit SUCCEEDS past the frameless hole, dooming the directory at reopen");

    // ---- CKV-006: stream truncated at the first tombstone ----
    // (stream-tombstone-differential). ChronoKVRangeScanCursorState cached
    // exactly one (key, value) pair; a key invisible at the stream's
    // snapshot (tombstone — and tree_->erase is never called, so
    // tombstones are permanent) landed in the cache as (key, nullopt),
    // and stream_has_next read that as end-of-stream: every key after the
    // first deleted one in the range was silently dropped, while the
    // vector range_scan skipped invisible keys correctly — the two public
    // scan surfaces disagreed. Differential test: stream output ==
    // vector output == the visible set computed independently from the
    // operation log, over ranges shaped to hit each truncation mode:
    // (a) first key deleted, (b) all-tombstone range, (c) mid-range
    // tombstone, (d) empty and full-universe ranges.
    static int (*const ckv006_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv006_" + std::to_string(getpid());
        uint64_t s = 0x5EED06ULL;   // fixed seed
        auto lcg = [&] { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s >> 33; };
        auto keyname = [](int i) { char b[8]; snprintf(b, sizeof b, "k%02d", i); return std::string(b); };
        const std::pair<const char*, const char*> ranges[] = {
            {"k10", "k20"},   // (a) first key deleted
            {"k30", "k35"},   // (b) entirely tombstones
            {"k40", "k50"},   // (c) tombstone in the middle
            {"k99", "k99"},   // (d) empty range
            {"k00", "k63"},   // (d) full universe
            {"k21", "k29"},   // random-content control
        };
        for (int iter = 0; iter < 5; ++iter) {
            std::filesystem::remove_all(wd);
            Options o;
            o.wal_dir = wd;
            o.auto_start_gc = false;
            auto db = Database::open(o);
            std::map<std::string, std::optional<std::string>> logv;   // op log -> expected
            std::vector<bool> present(64, false);
            // Forced keys so every range shape exists, then random fill to
            // a 20-50-key set.
            for (int i = 10; i <= 20; ++i) present[i] = true;
            for (int i = 30; i <= 35; ++i) present[i] = true;
            for (int i = 40; i <= 50; ++i) present[i] = true;
            int target = 28 + (int)(lcg() % 23);           // 28..50
            int have = 28;
            while (have < target) {
                int i = (int)(lcg() % 64);
                if (present[i]) continue;
                present[i] = true;
                ++have;
            }
            for (int i = 0; i < 64; ++i) {
                if (!present[i]) continue;
                std::string k = keyname(i), v = "v" + std::to_string(iter) + "_" + std::to_string(i);
                if (db.put(k, v) != Status::OK) { db.close(); return 10; }
                logv[k] = v;
            }
            // Forced tombstones for the shapes + a random 25% erase.
            auto do_erase = [&](const std::string& k) { (void)db.erase(k); logv[k] = std::nullopt; };
            do_erase(keyname(10));                          // (a)
            for (int i = 30; i <= 35; ++i) do_erase(keyname(i));   // (b)
            do_erase(keyname(42));                          // (c)
            for (int i = 0; i < 64; ++i) {
                if (!present[i] || !logv[keyname(i)].has_value()) continue;
                if (lcg() % 4 == 0) do_erase(keyname(i));
            }
            // Differentials, quiesced (both surfaces resolve the same snapshot).
            for (auto& [lo, hi] : ranges) {
                std::vector<std::pair<std::string, std::string>> expected;
                for (auto& [k, v] : logv)
                    if (v.has_value() && k >= lo && k <= hi) expected.emplace_back(k, *v);
                auto vec = db.range_scan(lo, hi);
                if (vec != expected) {
                    fprintf(stderr, "iter %d range %s..%s: vector scan diverged (%zu vs %zu)\n",
                            iter, lo, hi, vec.size(), expected.size());
                    db.close(); return 11;
                }
                Database::RangeScanStream st(db, lo, hi);
                std::vector<std::pair<std::string, std::string>> streamed;
                while (st.has_next()) streamed.push_back(st.next());
                if (streamed != expected) {
                    fprintf(stderr, "iter %d range %s..%s: STREAM diverged (%zu vs %zu",
                            iter, lo, hi, streamed.size(), expected.size());
                    if (!streamed.empty() && !expected.empty())
                        fprintf(stderr, ", stream last=%s expected last=%s",
                                streamed.back().first.c_str(), expected.back().first.c_str());
                    fprintf(stderr, ")\n");
                    db.close(); return 12;
                }
                for (size_t i = 1; i < streamed.size(); ++i)
                    if (!(streamed[i - 1].first < streamed[i].first)) { db.close(); return 13; }
            }
            db.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-006: stream skips tombstones — stream == vector == snapshot-visible set (stream-tombstone-differential)",
              ckv006_body, 120000,
              "stream truncates at the first tombstone; every later key in the range is silently dropped");

    // ---- CKV-003c: checkpoint dirty-coverage validation (ckpt-dirty-coverage) ----
    // Simulated index divergence: a dirty key's entry vanishes from the
    // serialized snapshot. Pre-fix the incremental checkpoint silently
    // skipped it, "succeeded", rotated/truncated the WAL — and the key was
    // permanently lost on reopen. Post-fix the checkpoint refuses BEFORE
    // any file write, latches invariant D4, and reopen replays the intact
    // WAL with every acked key.
    static int (*const ckv003c_neg_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv003c_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.checkpoint_path = wd + "/ckpt.bin";
        o.auto_start_gc = false;
        o.durability = DurabilityMode::Sync;
        auto db = Database::open(o);
        char buf[64];
        for (int i = 0; i < 200; ++i) {
            snprintf(buf, sizeof buf, "key-%09d", i);
            if (db.put(buf, "value") != Status::OK) return 9;
        }
        db.checkpoint();   // base (full)
        for (int i = 200; i < 250; ++i) {
            snprintf(buf, sizeof buf, "key-%09d", i);
            if (db.put(buf, "value") != Status::OK) return 9;
        }
        // Simulate the divergence: drop one dirty key from the snapshot.
        db.set_ckpt_dirty_capture_hook_for_test(
            [](std::vector<std::tuple<std::string, uint64_t, std::string, bool>>& snap) {
                for (auto it = snap.begin(); it != snap.end(); ++it)
                    if (std::get<0>(*it) == "key-000000225") { snap.erase(it); return; }
            });
        bool threw = false;
        try { db.checkpoint(); }
        catch (const std::exception&) { threw = true; }
        if (!threw) return 10;                       // pre-fix: silent skip + truncate
        if (db.put("post-divergence", "v") != Status::Failed) return 11;   // D4 latched
        if (db.health().level != 2) return 12;
        db.close();
        {
            auto db2 = Database::open(o);
            for (int i = 0; i < 250; ++i) {
                snprintf(buf, sizeof buf, "key-%09d", i);
                if (db2.get(buf).value_or("") != "value") return 13;   // pre-fix: 225 lost
            }
            if (db2.get("post-divergence").has_value()) return 14;
            db2.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-003c: checkpoint refuses on dirty-coverage divergence; WAL preserved (ckpt-dirty-coverage)",
              ckv003c_neg_body, 300000,
              "diverged dirty key silently skipped; checkpoint truncates the WAL; key lost on reopen");

    // ---- CKV-003c: checkpoint-vs-WAL differential + empty-checkpoint edge ----
    // Full lifecycle: empty-DB checkpoint (count-0 snapshot must recover),
    // then base ckpt -> incremental ckpt -> uncheckpointed tail -> reopen
    // reconciles checkpoint + WAL against the oracle. ALSO fail-first, for
    // a second pre-existing defect this differential exposed: recovery
    // never re-marked replayed WAL-tail keys as dirty, so the FIRST
    // incremental checkpoint after a reopen captured only the post-reopen
    // writes and its rotation then unlinked the WAL segments holding the
    // replayed tail — silent permanent loss (pre-fix: exit 17, the
    // pre-reopen edge keys gone). Fixed by dirty re-marking in both
    // recovery replay loops.
    static int (*const ckv003c_diff_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv003cd_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.checkpoint_path = wd + "/ckpt.bin";
        o.auto_start_gc = false;
        o.durability = DurabilityMode::Sync;
        {
            auto db = Database::open(o);
            db.checkpoint();                       // empty DB, count-0 snapshot
            char buf[64];
            for (int i = 0; i < 5; ++i) {
                snprintf(buf, sizeof buf, "e%d", i);
                if (db.put(buf, "edge") != Status::OK) return 10;
            }
            db.close();
        }
        {
            auto db = Database::open(o);
            for (int i = 0; i < 5; ++i)
                if (db.get("e" + std::to_string(i)).value_or("") != "edge") return 11;
            char buf[64];
            for (int i = 0; i < 300; ++i) {
                snprintf(buf, sizeof buf, "key-%09d", i);
                if (db.put(buf, "v1") != Status::OK) return 12;
            }
            db.checkpoint();                       // base
            for (int i = 300; i < 500; ++i) {
                snprintf(buf, sizeof buf, "key-%09d", i);
                if (db.put(buf, "v1") != Status::OK) return 13;
            }
            for (int i = 0; i < 150; ++i) {        // updates over base keys
                snprintf(buf, sizeof buf, "key-%09d", i);
                if (db.put(buf, "v2") != Status::OK) return 14;
            }
            db.checkpoint();                       // incremental
            for (int i = 500; i < 600; ++i) {
                snprintf(buf, sizeof buf, "key-%09d", i);
                if (db.put(buf, "v1") != Status::OK) return 15;
            }
            db.close();                            // uncheckpointed tail
        }
        {
            auto db = Database::open(o);
            char buf[64];
            for (int i = 0; i < 600; ++i) {
                snprintf(buf, sizeof buf, "key-%09d", i);
                std::string want = (i < 150) ? "v2" : "v1";
                if (db.get(buf).value_or("") != want) return 16;
            }
            for (int i = 0; i < 5; ++i)
                if (db.get("e" + std::to_string(i)).value_or("") != "edge") return 17;
            db.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-003c: checkpoint-vs-WAL differential incl. empty-checkpoint edge (ckpt-wal-differential)",
              ckv003c_diff_body, 300000,
              "replayed WAL tail not marked dirty; first post-reopen incremental checkpoint truncates it");

    // ---- CKV-005: max-representable scan boundary (sentinel-max-key) ----
    // Pre-fix, every internal full-range walk was tree_scan("", 255x0xFF):
    // keys AT or ABOVE the 255-byte 0xFF sentinel (legal — up to 4048
    // bytes) sorted above the bound and were silently skipped. Audit PoC:
    // a 256x0xFF key was lost across checkpoint+reopen (the full
    // checkpoint never saw it, then rotation removed its WAL records);
    // GC and free_all never saw its versions either. Post-fix the walks
    // are unbounded (Cursor hi_unbounded / tree_scan_all).
    static int (*const ckv005_body)() = +[]() -> int {
        using namespace chronokv;
        const std::string wd = "/tmp/ckv_ckv005_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.checkpoint_path = wd + "/ckpt.bin";
        o.auto_start_gc = false;
        o.durability = DurabilityMode::Sync;
        const std::string K254(254, '\xFF'), K255(255, '\xFF'), K256(256, '\xFF');
        {
            auto db = Database::open(o);
            if (db.put("a", "1") != Status::OK) return 9;
            if (db.put(K254, "v254") != Status::OK) return 10;
            if (db.put(K255, "v255") != Status::OK) return 11;   // == old sentinel
            if (db.put(K256, "v256") != Status::OK) return 12;   // > old sentinel
            if (db.put("zz", "2") != Status::OK) return 9;
            // in-memory visibility
            if (db.get(K255).value_or("") != "v255") return 13;
            if (db.get(K256).value_or("") != "v256") return 14;
            // bounded user scans keep exact inclusive semantics
            if (db.range_scan(K254, K256).size() != 3) return 15;
            if (db.range_scan("a", K256).size() != 5) return 16;
            // absent-key behavior at the boundary
            if (db.get(std::string(257, '\xFF')).has_value()) return 17;
            // FULL checkpoint (base) — pre-fix this walk skipped K255/K256
            db.checkpoint();
            db.close();
        }
        {   // reopen: the sentinel keys must survive checkpoint+recovery
            auto db = Database::open(o);
            if (db.get(K254).value_or("") != "v254") return 18;
            if (db.get(K255).value_or("") != "v255") return 19;   // pre-fix: lost
            if (db.get(K256).value_or("") != "v256") return 20;   // pre-fix: lost
            if (db.get("a").value_or("") != "1") return 21;
            if (db.range_scan(K254, K256).size() != 3) return 22;
            // incremental checkpoint over an updated sentinel key
            if (db.put(K256, "v256b") != Status::OK) return 23;
            db.checkpoint();
            db.close();
        }
        {   // second reopen: the update must be visible, chain consistent
            auto db = Database::open(o);
            if (db.get(K256).value_or("") != "v256b") return 24;
            if (db.get(K255).value_or("") != "v255") return 25;
            db.close();
        }
        std::filesystem::remove_all(wd);
        return 0;
    };
    run_child("remediation CKV-005: keys at/above the 255x0xFF sentinel survive checkpoint+reopen (sentinel-max-key)",
              ckv005_body, 300000,
              "full-checkpoint walk skips sentinel keys; rotation then destroys their WAL records");

    // ---- CKV-007: observer exceptions must not invert a committed result ----
    // Invariant: an observer callback is a best-effort notification. A
    // throwing callback is contained (and counted in diag); put() /
    // Batch::commit() / Transaction::commit() report the TRUE result of the
    // already-completed commit. Pre-fix: the exception from notify_observers
    // propagated to the committing caller AFTER the write was durable — a
    // successful commit surfaced as an exception (and, on the async path,
    // as a rethrown future — CKV-016).
    {
        const std::string wd = "/tmp/ckv_ckv007_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.auto_start_gc = false;
        auto db = Database::open(o);
        auto h = db.observe("w:", [](const std::string&,
                                     const std::optional<std::string>&,
                                     const std::optional<std::string>&) {
            throw std::runtime_error("observer boom (CKV-007 test)");
        });
        bool put_ok = false; std::string d1;
        try { put_ok = (db.put("w:k1", "v1") == Status::OK); }
        catch (const std::exception& e) { d1 = e.what(); }
        check("remediation CKV-007: sync put reports the true (OK) result despite a throwing observer (observer-exception-containment)",
              put_ok, d1);
        bool durable = false;
        try { auto v = db.get("w:k1"); durable = v.has_value() && *v == "v1"; } catch (...) {}
        check("remediation CKV-007: the committed write is durable even though the observer threw", durable);
        bool batch_ok = false; std::string d2;
        try {
            auto b = db.create_batch();
            b.put("w:k2", "v2");
            batch_ok = (b.commit() == Status::OK);
        } catch (const std::exception& e) { d2 = e.what(); }
        check("remediation CKV-007: Batch::commit reports OK despite a throwing observer", batch_ok, d2);
        bool txn_ok = false; std::string d3;
        try {
            auto t = db.begin();
            t.put("w:k3", "v3");
            txn_ok = (t.commit() == Status::OK);
        } catch (const std::exception& e) { d3 = e.what(); }
        check("remediation CKV-007: Transaction::commit reports OK despite a throwing observer", txn_ok, d3);
        bool async_ok = false; std::string d4;
        try {
            auto f = db.put_async("w:k4", "v4");
            async_ok = (f.get() == Status::OK);
        } catch (const std::exception& e) { d4 = std::string("future rethrew: ") + e.what(); }
        check("remediation CKV-016: put_async resolves to Status::OK despite a throwing observer (commit result never inverted)",
              async_ok, d4);
        db.close();
        std::filesystem::remove_all(wd);
    }

    // ---- CKV-016: async futures report engine failures as Status, never rethrow ----
    // Invariant: put_async/erase_async futures resolve to a Status for ANY
    // engine failure (public contract: "errors via Result<T> / Status").
    // Pre-fix: an exception thrown out of commit_txn (std::bad_alloc from an
    // exhausted bounded page pool — the CKV-003b trigger, which latches the
    // D4 index fail-stop and RETHROWS) was captured by the std::async future
    // and rethrown at get(), breaking the contract. This was also one of the
    // two std::terminate escape points the forced-coverage runs died on
    // ("async get() rethrows" — 0.27.0 hardening notes).
    {
        Options o;                       // in-memory: the WAL is not involved
        o.auto_start_gc = false;
        o.page_pool_bytes = 4 * 4096;    // bounded pool -> deterministic exhaustion
        auto db = Database::open(o);
        bool rethrew = false; std::string what;
        bool saw_failure = false;
        try {
            for (int round = 0; round < 20 && !saw_failure; ++round) {
                std::vector<std::future<Status>> fs;
                for (int i = 0; i < 10; ++i) {
                    const int n = round * 10 + i;
                    fs.push_back(db.put_async(std::string(160, 'k') + std::to_string(n),
                                              std::string(160, 'v')));
                }
                for (auto& f : fs) {
                    Status s = f.get();      // pre-fix: rethrows bad_alloc at exhaustion
                    if (s != Status::OK) saw_failure = true;
                }
            }
        } catch (const std::exception& e) { rethrew = true; what = e.what(); }
        check("remediation CKV-016: put_async futures never rethrow engine exceptions (pool exhaustion surfaces as Status)",
              !rethrew, what);
        check("remediation CKV-016: exhaustion is reported as a failure Status (test non-vacuous)",
              saw_failure && !rethrew);
        db.close();
    }

    // ---- CKV-014: Batch duplicate-key validation is deterministic and non-consuming ----
    // Invariant: a Batch whose write-set contains duplicate keys is rejected
    // with Status::InvalidTransaction with ZERO mutation, and the batch is
    // left INTACT for the caller to inspect/correct/clear — a failed
    // validation must not silently consume it. Pre-fix: Batch::commit
    // cleared entries_ unconditionally after the commit_txn attempt, so a
    // rejected duplicate batch came back EMPTY and an immediate second
    // commit() "succeeded" as a no-op — the API silently swallowed its own
    // validation error.
    {
        Options o;
        o.auto_start_gc = false;   // in-memory: the WAL is not involved
        auto db = Database::open(o);
        auto b = db.create_batch();
        b.put("dup", "v1");
        b.put("other", "vo");
        b.put("dup", "v2");                   // duplicate key
        Status s = b.commit();
        check("remediation CKV-014: duplicate-key batch rejected with InvalidTransaction (batch-duplicate-deterministic)",
              s == Status::InvalidTransaction);
        check("remediation CKV-014: rejected batch is left INTACT (not silently consumed)",
              b.size() == 3, "size=" + std::to_string(b.size()));
        bool nothing_written = !db.get("dup").has_value() && !db.get("other").has_value();
        check("remediation CKV-014: rejected batch mutated nothing", nothing_written);
        // The intact batch stays usable: clear() then a valid commit on the
        // SAME object must work.
        b.clear();
        b.put("dup", "v1");
        b.put("other", "vo");
        check("remediation CKV-014: after clear(), the same Batch object commits cleanly",
              b.commit() == Status::OK && db.get("dup").value_or("") == "v1");
        db.close();
    }

    // ---- CKV-010: restore_pitr refuses a dirty/non-empty destination ----
    // Invariant (documented contract): restore_pitr MATERIALIZES the as-of
    // database into a FRESH directory. Pre-fix an existing non-empty
    // dest_dir was silently accepted: the export wrote dest/ckpt while the
    // subsequent open adopted whatever already sat in dest/wal (e.g.
    // segments of an unrelated database) — a hybrid of the PITR state and
    // the pre-existing state, with no error.
    {
        const std::string src = "/tmp/ckv_ckv010_src_" + std::to_string(getpid());
        const std::string dst = "/tmp/ckv_ckv010_dst_" + std::to_string(getpid());
        std::filesystem::remove_all(src);
        std::filesystem::remove_all(dst);
        std::filesystem::create_directories(src + "/wal");   // engine mkdir is single-level today (F2)
        uint64_t as_of = 0;
        {
            Options o;
            o.wal_dir = src + "/wal";
            o.checkpoint_path = src + "/ckpt";
            o.auto_start_gc = false;
            auto db = Database::open(o);
            if (db.put("p1", "v1") != Status::OK) { check("CKV-010 setup", false); }
            db.checkpoint();
            as_of = db.published_watermark();
            if (db.put("p2", "v2") != Status::OK) { check("CKV-010 setup2", false); }
            db.close();
        }
        // Dirty destination: a pre-existing file AND a stale wal directory.
        std::filesystem::create_directories(dst + "/wal");
        { std::ofstream f(dst + "/preexisting.txt"); f << "do not touch"; }
        bool threw = false; std::string what;
        try {
            auto db = Database::restore_pitr(src + "/wal", src + "/ckpt", dst, as_of);
            db.close();
        } catch (const std::exception& e) { threw = true; what = e.what(); }
        check("remediation CKV-010: restore_pitr into a dirty destination throws (pitr-dirty-dest)",
              threw, threw ? what : std::string("restore silently proceeded into a dirty dest"));
        check("remediation CKV-010: the refused restore left the pre-existing file alone",
              std::filesystem::exists(dst + "/preexisting.txt"));
        // Fresh (nonexistent) destination still works and is writable.
        const std::string dst2 = dst + "_fresh";
        std::filesystem::remove_all(dst2);
        bool ok_fresh = false;
        try {
            auto db = Database::restore_pitr(src + "/wal", src + "/ckpt", dst2, as_of);
            ok_fresh = db.get("p1").value_or("") == "v1" && !db.get("p2").has_value();
            if (db.put("p3", "v3") == Status::OK)
                ok_fresh = ok_fresh && db.get("p3").value_or("") == "v3";
            db.close();
        } catch (const std::exception& e) { what = e.what(); ok_fresh = false; }
        check("remediation CKV-010: restore_pitr into a fresh directory still materializes a writable as-of db",
              ok_fresh, what);
        // An existing but EMPTY destination directory is accepted.
        const std::string dst3 = dst + "_empty";
        std::filesystem::remove_all(dst3);
        std::filesystem::create_directories(dst3);
        bool ok_empty = false;
        try {
            auto db = Database::restore_pitr(src + "/wal", src + "/ckpt", dst3, as_of);
            ok_empty = db.get("p1").value_or("") == "v1";
            db.close();
        } catch (const std::exception& e) { what = e.what(); ok_empty = false; }
        check("remediation CKV-010: restore_pitr into an existing EMPTY directory is accepted",
              ok_empty, what);
        std::filesystem::remove_all(src);
        std::filesystem::remove_all(dst);
        std::filesystem::remove_all(dst2);
        std::filesystem::remove_all(dst3);
    }

    // ---- CKV-011: a PITR open is strictly read-only on the SOURCE ----
    // Invariant (README Safety properties): "A PITR open does not modify the
    // source directory at all." Pre-fix the open violated it three ways:
    //   (1) the engine built full WAL machinery on the source — creating
    //       .chronokv.lock when absent and taking the WRITER flock (which
    //       also made as-of views of a live/locked source impossible);
    //   (2) open_segment()'s torn-tail repair ftruncate'd the source's
    //       active segment;
    //   (3) recover_with_checkpoint() deleted orphaned .tmp checkpoint files
    //       (the stale-delta cleanup was already guarded in v26.1; the
    //       .tmp sweep was not).
    {
        const std::string src = "/tmp/ckv_ckv011_" + std::to_string(getpid());
        std::filesystem::remove_all(src);
        std::filesystem::create_directories(src + "/wal");   // engine mkdir is single-level today (F2)
        const std::string wal = src + "/wal", ckpt = src + "/ckpt";
        uint64_t as_of = 0;
        {
            Options o;
            o.wal_dir = wal;
            o.checkpoint_path = ckpt;
            o.auto_start_gc = false;
            auto db = Database::open(o);
            if (db.put("s1", "v1") != Status::OK) { check("CKV-011 setup", false); }
            db.checkpoint();
            as_of = db.published_watermark();
            if (db.put("s2", "v2") != Status::OK) { check("CKV-011 setup2", false); }
            db.close();
        }
        // Crash residue: a torn tail on the ACTIVE (highest) segment, orphan
        // .tmp files, and a missing lock file (as in a backup copy).
        std::string active_seg;
        for (auto& e : std::filesystem::directory_iterator(wal)) {
            std::string fn = e.path().filename().string();
            if (fn.rfind("wal_", 0) == 0 && fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".log") == 0)
                if (active_seg.empty() || fn > std::filesystem::path(active_seg).filename().string())
                    active_seg = e.path().string();
        }
        if (active_seg.empty()) { check("CKV-011 setup: found active segment", false); }
        else {
            std::ofstream f(active_seg, std::ios::binary | std::ios::app);
            uint32_t bogus_len = 24;                    // plausible frame length
            uint32_t bogus_crc = 0xDEADBEEF;            // wrong CRC -> parse fail
            f.write(reinterpret_cast<const char*>(&bogus_len), 4);
            f.write(reinterpret_cast<const char*>(&bogus_crc), 4);
            const std::string junk(24, '\xA5');
            f.write(junk.data(), (std::streamsize)junk.size());
        }
        { std::ofstream f(ckpt + ".tmp"); f << "orphan"; }
        { std::ofstream f(ckpt + ".delta.9.tmp"); f << "orphan"; }
        std::error_code rmec;
        std::filesystem::remove(wal + "/.chronokv.lock", rmec);

        auto snapshot = [](const std::string& root) {
            std::map<std::string, std::string> out;
            for (auto& e : std::filesystem::recursive_directory_iterator(root)) {
                if (!e.is_regular_file()) continue;
                std::ifstream f(e.path(), std::ios::binary);
                out[std::filesystem::relative(e.path(), root).string()] =
                    std::string((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            }
            return out;
        };
        auto before = snapshot(src);
        {
            Options po;
            po.wal_dir = wal;
            po.checkpoint_path = ckpt;
            po.pitr_as_of_cts = as_of;
            po.auto_start_gc = false;
            auto view = Database::open(po);
            check("remediation CKV-011: as-of view serves the boundary state (s1 yes, s2 no)",
                  view.get("s1").value_or("") == "v1" && !view.get("s2").has_value());
            bool write_refused = false;
            try { write_refused = (view.put("s3", "v3") == Status::Failed); } catch (...) {}
            check("remediation CKV-011: writes are refused on the PITR open", write_refused);
            bool ckpt_refused = false;
            try { view.checkpoint(); } catch (const std::exception&) { ckpt_refused = true; }
            check("remediation CKV-011: checkpoint() is refused on the PITR open", ckpt_refused);
            bool backup_refused = false;
            try { view.backup(src + "_bk_attempt"); } catch (const std::exception&) { backup_refused = true; }
            check("remediation CKV-011: backup() is refused on the PITR open (it would rewrite the source ckpt)",
                  backup_refused);
            view.close();
        }
        auto after = snapshot(src);
        bool identical = (before == after);
        std::string diff;
        if (!identical) {
            for (auto& [k, v] : before) {
                auto it = after.find(k);
                if (it == after.end()) { diff = "removed: " + k; break; }
                if (it->second != v) {
                    diff = "modified: " + k + " (" + std::to_string(v.size()) +
                           " -> " + std::to_string(it->second.size()) + " bytes)";
                    break;
                }
            }
            if (diff.empty())
                for (auto& [k, v] : after)
                    if (before.find(k) == before.end()) { diff = "created: " + k; break; }
        }
        check("remediation CKV-011: PITR open left the source tree byte-identical (pitr-source-immutability)",
              identical, diff);
        // A later NORMAL open still recovers everything (it may now repair
        // the torn tail and sweep the orphans — that is a normal open's job).
        {
            Options o;
            o.wal_dir = wal;
            o.checkpoint_path = ckpt;
            o.auto_start_gc = false;
            auto db = Database::open(o);
            check("remediation CKV-011: a later normal open still recovers the full state",
                  db.get("s1").value_or("") == "v1" && db.get("s2").value_or("") == "v2");
            db.close();
        }
        std::filesystem::remove_all(src);
        std::filesystem::remove_all(src + "_bk_attempt");
    }

    // ---- CKV-019: a corrupt segment (interior hole) must fail LOUD, never be ----
    // ---- silently repaired, truncated, or LSN-reseeded                      ----
    // Invariant (D1 recovery policy): wal_recover_buf classifies "unparseable
    // bytes with a PARSEABLE record after them" as CORRUPT (an interior hole —
    // the power-loss zero-fill shape whose trailing frames may be
    // ACKNOWLEDGED commits), distinct from TORN_TAIL (garbage to EOF).
    // Pre-fix, the append path destroyed that distinction three ways:
    //   (1) open_segment()'s truncate_torn_tail cut the active segment at the
    //       first invalid frame — DELETING the valid frames after the hole
    //       (silent loss of acknowledged writes) before recovery could see
    //       CORRUPT;
    //   (2) the truncation result was ignored ((void)truncate_torn_tail), so
    //       even an I/O-failed repair proceeded to append after garbage;
    //   (3) find_max_lsn_in_segment() mapped CORRUPT to 0 — reseeding the LSN
    //       counter below the segment's real max, re-issuing LSNs that later
    //       recovery rejects as duplicates (bricking the database).
    {
        const std::string dir = "/tmp/ckv_ckv019_" + std::to_string(getpid());
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        {
            Options o; o.wal_dir = dir; o.auto_start_gc = false; o.recover_on_open = false;
            auto db = Database::open(o);
            if (db.put("k1", "v1") != Status::OK) { check("CKV-019 setup1", false); }
            if (db.put("k2", "v2") != Status::OK) { check("CKV-019 setup2", false); }
            db.close();
        }
        // Locate the active segment and append: [bad-CRC frame with a valid
        // length field][fully valid frame lsn=3 cts=3] — the interior-hole
        // shape wal_recover_buf classifies as CORRUPT (valid_after == true).
        std::string seg;
        for (auto& e : std::filesystem::directory_iterator(dir)) {
            std::string fn = e.path().filename().string();
            if (fn.rfind("wal_", 0) == 0 && fn.compare(fn.size() - 4, 4, ".log") == 0)
                seg = e.path().string();
        }
        if (seg.empty()) { check("CKV-019 setup: found segment", false); }
        const auto orig_bytes = [] (const std::string& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }(seg);
        {
            std::ofstream f(seg, std::ios::binary | std::ios::app);
            WriteSet ws3 = {{"k3", "v3", false}};
            auto valid3 = wal_make_record(3, 3, ws3);          // [len][crc][payload]
            uint32_t hole_len = 24;                            // plausible payload len
            uint32_t hole_crc = 0xDEADBEEF;                    // wrong CRC -> parse fail
            f.write(reinterpret_cast<const char*>(&hole_len), 4);
            f.write(reinterpret_cast<const char*>(&hole_crc), 4);
            const std::string junk(24, '\xA5');
            f.write(junk.data(), (std::streamsize)junk.size());
            f.write(reinterpret_cast<const char*>(valid3.data()), (std::streamsize)valid3.size());
        }
        const std::string planted = orig_bytes + [] {
            std::string s; uint32_t l = 24, c = 0xDEADBEEF;
            s.append(reinterpret_cast<const char*>(&l), 4);
            s.append(reinterpret_cast<const char*>(&c), 4);
            s.append(24, '\xA5');
            WriteSet ws3 = {{"k3", "v3", false}};
            auto v = wal_make_record(3, 3, ws3);
            s.append(reinterpret_cast<const char*>(v.data()), v.size());
            return s;
        }();

        // (B) recovery open must fail LOUD (CorruptionError), not open a
        //     silently-truncated view missing the post-hole record.
        bool threw = false; std::string what;
        {
            Options o; o.wal_dir = dir; o.auto_start_gc = false; o.recover_on_open = true;
            try {
                auto db = Database::open(o);
                (void)db.get("k1");
                db.close();
            } catch (const std::exception& e) { threw = true; what = e.what(); }
        }
        check("remediation CKV-019: recovery open over an interior-hole segment throws (corrupt-segment-loud)",
              threw, threw ? what : std::string("open succeeded — hole was silently repaired"));
        // (C) the failed open must leave the segment byte-identical (no
        //     destructive repair behind the loud failure).
        const std::string after_open = [] (const std::string& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }(seg);
        check("remediation CKV-019: the segment survives the failed open byte-identical (no silent truncation)",
              after_open == planted,
              "size " + std::to_string(planted.size()) + " -> " + std::to_string(after_open.size()));

        // (A) append-only open (recover_on_open=false): fail-stop handle —
        //     writes refused, no LSN reseed, file untouched.
        {
            Options o; o.wal_dir = dir; o.auto_start_gc = false; o.recover_on_open = false;
            bool open_threw = false; Status put_status = Status::OK; int level = 0;
            try {
                auto db = Database::open(o);
                put_status = db.put("k4", "v4");
                level = db.health().level;
                db.close();
            } catch (const std::exception& e) { open_threw = true; what = e.what(); }
            check("remediation CKV-019: append-only open over a corrupt segment fail-stops (writes refused, health degraded)",
                  open_threw || (put_status == Status::Failed && level >= 1),
                  open_threw ? what : "put=" + std::to_string((int)put_status) + " health=" + std::to_string(level));
        }
        const std::string after_append_open = [] (const std::string& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }(seg);
        check("remediation CKV-019: the append-only open left the corrupt segment untouched",
              after_append_open == planted);

        // (D) CONTROL — a PURE torn tail (garbage to EOF, nothing parseable
        //     after it) is still repaired silently and recovers: the fix must
        //     not break legitimate torn-tail recovery.
        {
            const std::string dir2 = "/tmp/ckv_ckv019b_" + std::to_string(getpid());
            std::filesystem::remove_all(dir2);
            std::filesystem::create_directories(dir2);
            {
                Options o; o.wal_dir = dir2; o.auto_start_gc = false; o.recover_on_open = false;
                auto db = Database::open(o);
                if (db.put("t1", "v1") != Status::OK) { check("CKV-019 control setup", false); }
                db.close();
            }
            std::string seg2;
            for (auto& e : std::filesystem::directory_iterator(dir2)) {
                std::string fn = e.path().filename().string();
                if (fn.rfind("wal_", 0) == 0 && fn.compare(fn.size() - 4, 4, ".log") == 0)
                    seg2 = e.path().string();
            }
            {
                std::ofstream f(seg2, std::ios::binary | std::ios::app);
                uint32_t l = 24, c = 0xDEADBEEF;
                f.write(reinterpret_cast<const char*>(&l), 4);
                f.write(reinterpret_cast<const char*>(&c), 4);
                const std::string junk(24, '\xA5');
                f.write(junk.data(), (std::streamsize)junk.size());   // garbage to EOF: TORN
            }
            bool ok = false;
            try {
                Options o; o.wal_dir = dir2; o.auto_start_gc = false; o.recover_on_open = true;
                auto db = Database::open(o);
                ok = db.get("t1").value_or("") == "v1";
                if (db.put("t2", "v2") == Status::OK)     // append-after-repair works
                    ok = ok && db.get("t2").value_or("") == "v2";
                db.close();
            } catch (const std::exception& e) { what = e.what(); }
            check("remediation CKV-019: CONTROL — a pure torn tail is still repaired and the db appends after it",
                  ok, what);
            std::filesystem::remove_all(dir2);
        }
        std::filesystem::remove_all(dir);
    }

    // ---- CKV-020: close() is called EXACTLY once — no EINTR retry, no fd-reuse hazard ----
    // On Linux a close() that returned EINTR has ALREADY released the
    // descriptor (close(2) NOTES: "it is unspecified whether the descriptor
    // is closed" is resolved by Linux as closed); retrying the close closes
    // whatever fd NUMBER the kernel handed another thread in the meantime —
    // the classic fd-reuse hazard (the pre-fix loop did exactly that). The
    // single-close contract: report the failure (callers fail-stop per D3),
    // relinquish ownership unconditionally (callers null their handle), and
    // never call close() twice for one descriptor.
    {
        int calls = 0;
        const std::string tf = "/tmp/ckv_ckv020_" + std::to_string(getpid());
        int real_fd = ::open(tf.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        ckv_close_hook_for_test() = [&](int) -> int {
            ++calls;
            errno = (calls == 1) ? EINTR : EIO;
            return -1;
        };
        bool result = checked_close(real_fd);   // hook stands in; no real close
        ckv_close_hook_for_test() = nullptr;
        ::close(real_fd);                        // release for real
        std::filesystem::remove(tf);
        check("remediation CKV-020: checked_close calls close() EXACTLY once (EINTR is not retried; no fd-reuse hazard)",
              calls == 1, "close() calls=" + std::to_string(calls));
        check("remediation CKV-020: a failed close is still reported (fail-stop signal preserved)",
              result == false);
        // A failed close on an already-closed fd: reported false, once, no hang.
        calls = 0;
        const std::string tf2 = "/tmp/ckv_ckv020b_" + std::to_string(getpid());
        int fd2 = ::open(tf2.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        ::close(fd2);
        ckv_close_hook_for_test() = [&](int) -> int { ++calls; errno = EBADF; return -1; };
        bool r2 = checked_close(fd2);
        ckv_close_hook_for_test() = nullptr;
        std::filesystem::remove(tf2);
        check("remediation CKV-020: a failed close on an invalid fd reports false exactly once",
              calls == 1 && !r2, "calls=" + std::to_string(calls));
    }

    // ---- CKV-013: registered-file slot identity must survive fd-number reuse ----
    // WAL segment rotation closes the old fd and opens the new segment; the
    // kernel hands back the SAME fd number (lowest free). Pre-fix,
    // ensure_file_registered short-circuited on fd-number equality, leaving
    // io_uring file slot 0 pointed at the SEALED old inode (closed, but still
    // kernel-referenced through the registered-files table): every
    // WRITE_FIXED after a rotation would silently append acknowledged
    // records to the WRONG segment — the new segment stays empty (cts gap at
    // recovery) while the sealed file grows past its MANIFEST horizon.
    // Identity must be (st_dev, st_ino), not the fd number.
    {
        chronokv_iouring::IoUring ring;
        if (!ring.available()) {
            std::cout << "   remediation CKV-013: SKIP (io_uring ring unavailable on this platform)\n";
        } else {
            const std::string base = "/tmp/ckv_ckv013_" + std::to_string(getpid());
            std::filesystem::remove_all(base);
            std::filesystem::create_directories(base);
            int fdA = ::open((base + "/segA").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
            bool regA = fdA >= 0 && ring.ensure_file_registered_for_test(fdA);
            if (!regA) {
                std::cout << "   remediation CKV-013: SKIP (IORING_REGISTER_FILES unsupported on this kernel)\n";
                if (fdA >= 0) ::close(fdA);
            } else {
                const uint64_t after_first = ring.reg_updates_for_test();
                ::close(fdA);
                int fdB = ::open((base + "/segB").c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
                if (fdB != fdA) {
                    // The hazard needs fd-number reuse; a quiet test process
                    // gets it essentially always (lowest free fd). If not,
                    // say so instead of passing vacuously.
                    check("remediation CKV-013: precondition — kernel reused the fd number",
                          false, "fdA=" + std::to_string(fdA) + " fdB=" + std::to_string(fdB));
                } else {
                    bool regB = ring.ensure_file_registered_for_test(fdB);
                    const uint64_t after_second = ring.reg_updates_for_test();
                    check("remediation CKV-013: same fd NUMBER over a different file re-registers the slot (registered-file-identity)",
                          regB && after_second == after_first + 1,
                          "updates " + std::to_string(after_first) + " -> " + std::to_string(after_second) +
                          " (expected +1: slot must move to the new inode)");
                    // Same file, same fd: the no-syscall fast path must stay.
                    bool regB2 = ring.ensure_file_registered_for_test(fdB);
                    check("remediation CKV-013: unchanged identity keeps the no-syscall fast path",
                          regB2 && ring.reg_updates_for_test() == after_second);
                }
                ::close(fdB);
            }
            std::filesystem::remove_all(base);
        }
    }

    // ---- CKV-008: OLC leaf fence detects count/min/max-preserving mutation ----
    // Invariant: fence_unchanged answers "was this leaf mutated since the
    // descent captured it". The (key_count, min, max) triple ALONE cannot
    // answer that: a delete+insert of a DIFFERENT interior key preserves all
    // three. The leaf mutation epoch (PageHeader::mut_epoch, bumped on every
    // leaf mutation under the exclusive latch — inserts, in-place updates,
    // erases, split rebuilds) is the fence's PRIMARY field, compared as one
    // plain uint32 (Blocker 5: no packed hi/lo halves, no precedence
    // ambiguity, no truncation). Pages are never reclaimed, so a page id
    // stays valid and a stale fence can never be validated by id reuse.
    {
        chronokv_page::PagePool pool(64 * 4096);
        chronokv_btree::BTree bt(pool);
        bt.put("a", "1");
        bt.put("b", "2");
        bt.put("c", "3");
        const auto leaf = bt.find_leaf_for_test("b");
        auto fence = bt.capture_leaf_fence_for_test(leaf);
        check("remediation CKV-008: a fresh fence validates UNCHANGED (baseline)",
              bt.leaf_fence_unchanged_for_test(leaf, fence));
        // Count/min/max-preserving mutation: erase an interior key, insert a
        // different interior key (count 3->2->3, min "a" / max "c" identical).
        bt.erase("b");
        bt.put("bb", "22");
        check("remediation CKV-008: fence detects a count/min/max-preserving delete+insert (mutation-epoch fence)",
              !bt.leaf_fence_unchanged_for_test(leaf, fence),
              "fence still 'unchanged' after erase(b)+put(bb) — epoch missing or not bumped");
        // An in-place value update must bump too (the fence answers "was this
        // leaf touched", not "did its shape change").
        auto fence2 = bt.capture_leaf_fence_for_test(leaf);
        bt.put("a", "999");
        check("remediation CKV-008: fence detects an in-place value update",
              !bt.leaf_fence_unchanged_for_test(leaf, fence2));
        // A split must resume the rebuilt original page above its old epoch
        // (a fence captured pre-split must not validate post-split).
        auto fence3 = bt.capture_leaf_fence_for_test(leaf);
        for (int i = 0; i < 300; ++i) {
            char kb[8];
            snprintf(kb, sizeof kb, "%03d", i);
            bt.put(std::string("aa") + kb, std::string(60, 'v'));  // lands between "a" and "bb"
        }
        check("remediation CKV-008: fence detects a split-rebuild of the captured page",
              !bt.leaf_fence_unchanged_for_test(leaf, fence3));
    }

    // ---- CKV-009: authoritative commit-time conflict validation (VERIFICATION) ----
    // Traced, per the remediation directive to verify rather than invent:
    //   * EVERY public write path (put/erase/Batch::commit/Transaction::
    //     commit) funnels through commit_txn, whose on_reserve callback
    //     records existence transitions in the phantom tracker UNDER the
    //     reserved cts (commit-ts order == phantom order), on both the WAL
    //     and no-WAL paths; replay_records (the v18 replication spike) is
    //     not reachable from the public API.
    //   * Point reads are validated authoritatively at commit:
    //     Transaction::read registers the key in rs_, and commit_txn checks
    //     last_write_ts > read_ts for EVERY read-set entry under the
    //     per-key commit locks — so a point-read/write skew conflicts even
    //     when the writer's commit touches a disjoint key.
    // No engine defect found; these deterministic tests pin the contract
    // (they pass pre-fix and post-fix — verification, not a fix).
    {
        Options o;
        o.auto_start_gc = false;   // in-memory, fully deterministic
        auto db = Database::open(o);
        // (a) point-read/write skew over disjoint write keys
        {
            auto a = db.begin();
            const bool x_absent = !a.get("x").has_value();
            {
                auto b = db.begin();
                b.put("x", "1");
                if (b.commit() != Status::OK) check("CKV-009 setup B", false);
            }
            a.put("y", "1");                       // writes a DIFFERENT key
            const Status sa = a.commit();
            check("remediation CKV-009: point-read/write skew over disjoint write keys conflicts (rs_ validation)",
                  x_absent && sa == Status::Conflict,
                  "status=" + std::to_string((int)sa));
        }
        // (b) a BATCH insert into a txn's scanned range is phantom-detected
        {
            auto a = db.begin();
            auto scan = a.range_scan("g", "k");    // registers RangeRead [g,k]
            (void)scan;
            {
                auto batch = db.create_batch();
                batch.put("h", "inserted-by-batch");
                if (batch.commit() != Status::OK) check("CKV-009 setup batch", false);
            }
            a.put("z", "1");
            const Status sa = a.commit();
            check("remediation CKV-009: a BATCH insert into a scanned range is phantom-detected at commit",
                  sa == Status::Conflict, "status=" + std::to_string((int)sa));
        }
        // (c) CONTROL: the same shape with the insert OUTSIDE the range
        {
            auto a = db.begin();
            auto scan = a.range_scan("g", "k");
            (void)scan;
            {
                auto batch = db.create_batch();
                batch.put("q", "outside");
                if (batch.commit() != Status::OK) check("CKV-009 setup batch2", false);
            }
            a.put("z2", "1");
            check("remediation CKV-009: CONTROL — an insert outside the scanned range does not conflict",
                  a.commit() == Status::OK);
        }
        db.close();
    }

    // ---- CKV-015: abandoned-transaction engine lifetime/pinning (VERIFICATION) ----
    // Traced: a public Transaction holds (1) a shared_ptr ENGINE KEEPALIVE,
    // so ~ChronoKV (GC join, free_all, flock release) cannot run while the
    // transaction lives, and (2) a weak_ptr into the Database liveness flag;
    // every operation gates on check_active(), which throws LifecycleError
    // once the Database is closed OR destroyed. ~Transaction calls
    // std::abort() only when the transaction is active AND the engine is
    // still live (a true abandonment); with the Database gone, the
    // destructor and ~ReadWriteTransaction skip kv_ cleanup via the same
    // flag. close() moves the engine out under close_mu_, deferring teardown
    // to the last keepalive. No defect found; these tests pin the contract.
    {
        Options o;
        o.auto_start_gc = false;
        auto db = Database::open(o);
        if (db.put("k", "v0") != Status::OK) check("CKV-015 setup", false);
        auto txn = db.begin();
        txn.put("k", "v1");                 // staged write, txn active
        db.close();                          // close UNDER the live transaction
        bool get_threw = false;
        try { (void)txn.get("k"); }
        catch (const chronokv::LifecycleError&) { get_threw = true; }
        catch (...) {}
        check("remediation CKV-015: ops on a transaction whose Database closed throw LifecycleError (no UAF)",
              get_threw);
        bool abort_ok = true;
        try { txn.abort(); } catch (...) { abort_ok = false; }
        check("remediation CKV-015: abort() after Database::close() completes cleanly", abort_ok);
        // txn (now inactive) is destroyed at scope exit: destructor takes the
        // engine-gone path — no std::abort. Reaching the next check IS the
        // assertion that the destructor survived.
    }
    check("remediation CKV-015: destroying a closed-database transaction did not abort the process", true);
    {
        // A transaction outliving the DATABASE OBJECT ITSELF (destroyed
        // without close()): the keepalive pins the engine; the liveness flag
        // expires with the Database.
        std::optional<chronokv::Transaction> held;
        {
            Options o;
            o.auto_start_gc = false;
            auto db = Database::open(o);
            if (db.put("k", "v") != Status::OK) check("CKV-015 setup2", false);
            held.emplace(db.begin());
        }   // ~Database here — no close() call
        bool threw = false;
        try { (void)held->get("k"); }
        catch (const chronokv::LifecycleError&) { threw = true; }
        catch (...) {}
        check("remediation CKV-015: a transaction outliving its Database object fails cleanly (keepalive prevents UAF)",
              threw);
        try { held->abort(); } catch (...) {}
        held.reset();   // destructor with an expired flag: no process abort
        check("remediation CKV-015: destroying the abandoned transaction after its Database died does not abort",
              true);
    }

    // ---- Review F1b: a ring whose WRITE CQEs keep failing must degrade permanently ----
    // Invariant: io_uring "availability" means I/O OPS work, not merely that
    // the ring sets up. Real platforms exist (seccomp-restricted containers;
    // a 4.19-generation backport kernel measured during the external review:
    // io_uring_setup OK, io_uring_enter OK, every IORING_OP_WRITE completes
    // with -EINVAL) where enter_with_timeout's "failed enter degrades the
    // ring" latch never fires, so EVERY durability batch built the SQE
    // chain, entered, drained failing CQEs, and then did the pwrite+fsync
    // fallback anyway — forever. The engine must degrade to the fallback
    // after repeated CQE-level write failures. (MOCK_FAILURE models exactly
    // this shape: enter succeeds, the write CQE reports -1, data lands via
    // the internal pwrite so durability is preserved.)
    {
        const std::string wd = "/tmp/ckv_f1_degrade_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        std::filesystem::create_directories(wd);
        Options o;
        o.wal_dir = wd;
        o.auto_start_gc = false;
        o.recover_on_open = false;
        auto db = Database::open(o);
        auto mock = std::make_unique<chronokv_iouring::MockIoUring>(
            chronokv_iouring::MockIoUring::MOCK_FAILURE);
        chronokv_iouring::MockIoUring* mp = mock.get();
        db.inject_iouring_for_test(std::move(mock));
        bool all_ok = true;
        for (int i = 0; i < 6; ++i)
            all_ok = all_ok && (db.put("k" + std::to_string(i), "v") == Status::OK);
        check("remediation F1b: writes stay durable via the fallback while every write CQE fails",
              all_ok && !db.last_batch_used_iouring());
        check("remediation F1b: the ring PERMANENTLY DEGRADES after repeated CQE-level write failures (iouring-cqe-degradation)",
              !mp->available(),
              "ring still 'available' after 6 failed-CQE batches — every future batch would keep paying the io_uring attempt");
        db.close();
        o.recover_on_open = true;
        bool rec = true;
        {
            auto db2 = Database::open(o);
            for (int i = 0; i < 6; ++i)
                rec = rec && db2.get("k" + std::to_string(i)).has_value();
            db2.close();
        }
        check("remediation F1b: recovery sees every record written under the failing ring", rec);
        std::filesystem::remove_all(wd);
    }

    // ---- Review F2: a nested, nonexistent wal_dir is created at open ----
    // The README quick-start (wal_dir = "/tmp/mydb/wal") aborted on a fresh
    // machine: the engine's single-level ::mkdir could not create the nested
    // path, its failure was ignored, and the error surfaced as a misleading
    // "inter-process lock: cannot open .../.chronokv.lock" LifecycleError.
    // Every in-suite test pre-created its directories, so CI never saw it.
    {
        const std::string root = "/tmp/ckv_f2_" + std::to_string(getpid());
        std::filesystem::remove_all(root);
        bool opened = false; std::string what;
        {
            Options o;
            o.wal_dir = root + "/nested/deeper/wal";   // parents do NOT exist
            o.checkpoint_path = root + "/nested/ckpt";
            o.auto_start_gc = false;
            try {
                auto db = Database::open(o);
                opened = (db.put("k", "v") == Status::OK);
                db.checkpoint();
                db.close();
            } catch (const std::exception& e) { what = e.what(); }
        }
        check("remediation F2: open() creates a nested wal_dir — the README quick-start runs verbatim (nested-waldir-create)",
              opened, what);
        bool recovered = false;
        try {
            Options o;
            o.wal_dir = root + "/nested/deeper/wal";
            o.checkpoint_path = root + "/nested/ckpt";
            o.auto_start_gc = false;
            auto db = Database::open(o);
            recovered = db.get("k").value_or("") == "v";
            db.close();
        } catch (const std::exception& e) { what = e.what(); }
        check("remediation F2: data survives close+reopen of the created tree", recovered, what);
        // A genuinely uncreatable wal_dir must name the REAL cause.
        bool msg_ok = false;
        const std::string blocker = root + "/blocker_file";
        { std::ofstream f(blocker); f << "x"; }        // a regular FILE as parent
        try {
            Options o;
            o.wal_dir = blocker + "/wal";
            o.auto_start_gc = false;
            o.recover_on_open = false;
            auto db = Database::open(o);
            db.close();
        } catch (const std::exception& e) {
            what = e.what();
            msg_ok = what.find("cannot create wal_dir") != std::string::npos;
        }
        check("remediation F2: an uncreatable wal_dir reports the real cause (not the lock file)",
              msg_ok, what);
        std::filesystem::remove_all(root);
    }

    // ---- CKV-004R (Phase 3): complete frame on disk + reported write failure — no resurrection ----
    // Phase 3 judged the pre-existing CKV-004 coverage VACUOUS for the
    // partial-write shape: WriteFail fails BEFORE any byte is written, and
    // a one-byte prefix is just a torn tail. The resurrection hazard needs
    // at least one COMPLETE, CRC-valid frame on disk while the write call
    // reports failure — deterministically produced here by the
    // pwrite_fail_after_bytes injector (the audit PoC drove it with
    // RLIMIT_FSIZE EFBIG mid-batch). Scenario (async durability, MOCK_FAILURE
    // ring so the production pwrite_all path runs on EVERY platform):
    // put_async -> the batch's single frame is fully written -> EIO ->
    // WRITE-stage failure: `written` was never published, so the waiter
    // exits WalFailure (never acked). The CKV-004 gate (`written &&
    // has_async` is the ONLY keep-case) must TRUNCATE: the pre-CKV-004 gate
    // keyed on has_async alone, so the complete frame survived and recovery
    // replayed it — a transaction whose caller observed failure
    // RESURRECTED at the next open (invariant D2 violated).
    {
        const std::string wd = "/tmp/ckv_ckv004r_" + std::to_string(getpid());
        std::filesystem::remove_all(wd);
        std::filesystem::create_directories(wd);
        Options o;
        o.wal_dir = wd;
        o.durability = DurabilityMode::Async;
        o.auto_start_gc = false;
        o.recover_on_open = false;
        auto db = Database::open(o);
        if (db.put("seed", "s") != Status::OK) check("CKV-004R setup", false);
        // Derive the EXACT frame size from the real framing (lsn/cts are
        // fixed-width; only write-set content determines the length).
        WriteSet probe_ws = {{"rr", "vv", false}};
        const size_t frame_sz = wal_make_record(1, 1, probe_ws).size();
        // Force the production pwrite path on every platform, then arm:
        // let exactly one COMPLETE frame land, then fail the call.
        db.inject_iouring_for_test(
            std::make_unique<chronokv_iouring::MockIoUring>(
                chronokv_iouring::MockIoUring::MOCK_FAILURE));
        chronokv_iouring::pwrite_fail_after_bytes_for_test().store((int64_t)frame_sz);
        Status st = Status::OK;
        try {
            auto f = db.put_async("rr", "vv");
            st = f.get();     // WRITE-stage failure: `written` never published -> WalFailure
        } catch (const std::exception&) { st = Status::Failed; }
        chronokv_iouring::pwrite_fail_after_bytes_for_test().store(-1);  // one-shot; belt
        check("remediation CKV-004R: the async committer observes the write failure (never acked)",
              st != Status::OK, "status=" + std::to_string((int)st));
        db.close();
        {   // Reopen: the failed frame must NOT resurrect; the earlier acked
            // write must survive; the database must be writable again.
            Options o2 = o;
            o2.recover_on_open = true;
            auto db2 = Database::open(o2);
            const bool no_resurrect = !db2.get("rr").has_value();
            const bool seed_ok = db2.get("seed").value_or("") == "s";
            const bool writable = (db2.put("after", "x") == Status::OK);
            check("remediation CKV-004R: a COMPLETE CRC-valid frame of an unacked async batch does not resurrect on recovery (wal-partial-frame-no-resurrection)",
                  no_resurrect, "'rr' present after reopen — the failed batch's frame was not truncated away");
            check("remediation CKV-004R: the earlier acknowledged write survives", seed_ok);
            check("remediation CKV-004R: the database is writable after the failed batch", writable);
            db2.close();
        }
        std::filesystem::remove_all(wd);
    }

    if (fails == 0) std::cout << "   REMEDIATION TESTS PASSED\n";
    else std::cout << "   REMEDIATION FAILURES: " << fails << "\n";
    return fails;
}

#endif // CHRONOKV_TEST_HOOKS (extracted battery TU)
