#include "chronokv.hpp"
#include <iostream>
#include <chrono>
#include <random>
#include <algorithm>
using namespace std::chrono;

static int g_fail = 0;
static void chk(const char* n, bool ok, const std::string& d = "") {
    std::cout << "  [" << (ok ? "PASS" : "FAIL") << "] " << n;
    if (!ok || !d.empty()) std::cout << "  (" << d << ")";
    std::cout << "\n";
    if (!ok) ++g_fail;
}

// ================= Hunt class 1: tracker completeness =================
static void class1() {
    std::cout << "== class 1: TXN-1 tracker growth / prune liveness\n";
    // 1a: write-only, GC OFF, no readers -> growth is real; a single slot acquire must prune it all
    {
        chronokv::Options o; o.page_pool_bytes = 64u << 20; o.auto_start_gc = false;
        auto db = chronokv::Database::open(o);
        for (int i = 0; i < 50000; ++i) db.put("k" + std::to_string(i), "v");
        size_t grown = db.epoch_stats().entries;
        { auto t = db.begin(); t.commit(); }           // slot churn does NOT prune (prune lives in gc_once only)
        size_t after = db.epoch_stats().entries;
        chk("1a GC-off write-only: tracker grows and slot churn does not prune (documented contract: reclamation is the GC's job)",
            grown >= 49000 && after >= 49000, "grown=" + std::to_string(grown) + " after=" + std::to_string(after));
    }
    // 1b: GC ON default -> tracker stays bounded under write-only load
    {
        chronokv::Options o; o.page_pool_bytes = 64u << 20;
        auto db = chronokv::Database::open(o);
        for (int i = 0; i < 50000; ++i) db.put("k" + std::to_string(i), "v");
        std::this_thread::sleep_for(milliseconds(300));   // let GC passes run
        size_t e = db.epoch_stats().entries;
        chk("1b GC-on write-only keeps tracker bounded", e < 5000, "entries=" + std::to_string(e));
    }
    // 1c: long reader pins entries ABOVE its snapshot; release + new slot drops them
    {
        chronokv::Options o; o.page_pool_bytes = 64u << 20;   // GC ON (default)
        auto db = chronokv::Database::open(o);
        db.put("x", "0");
        auto t = db.begin();                                  // reader registered at snapshot S
        for (int i = 0; i < 20000; ++i) db.put("w" + std::to_string(i), "v");
        std::this_thread::sleep_for(milliseconds(200));       // GC passes run WHILE t pins
        size_t pinned = db.epoch_stats().entries;             // must stay high: t needs them
        t.commit();
        std::this_thread::sleep_for(milliseconds(400));       // GC passes prune after release
        size_t after = db.epoch_stats().entries;
        chk("1c long reader pins mods under live GC; release lets GC prune", pinned >= 19000 && after <= 100,
            "pinned=" + std::to_string(pinned) + " after=" + std::to_string(after));
    }
    // 1d: validation cost with a huge live mod-set (report, not gate)
    {
        chronokv::Options o; o.page_pool_bytes = 256u << 20; o.auto_start_gc = false;
        auto db = chronokv::Database::open(o);
        auto t = db.begin();                                   // pins everything above its snapshot
        for (int i = 0; i < 200000; ++i) db.put("m" + std::to_string(i), "v");
        auto t0 = steady_clock::now();
        (void)t.range_scan("zz0", "zz9");                      // range read over empty subrange
        t.put("fresh_key_never_written", "1");                 // NEW key: no write-write conflict
        auto st = t.commit();                                  // validation scans 200k ts entries
        auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        chk("1d commit validation with 200k live mods completes", st == chronokv::Status::OK,
            "r=" + std::to_string((int)st) + " scan+commit=" + std::to_string(ms) + "ms over 200k mods (cost is O(mods-above-snapshot))");
    }
}

// ================= Hunt class 2: TXN-2 point-range path =================
static void class2() {
    std::cout << "== class 2: TXN-2 [k,k] point range reads\n";
    chronokv::Options mem; mem.page_pool_bytes = 32u << 20;
    // 2a: absent read, prune pressure from unrelated txns, then insert -> must still conflict
    {
        auto db = chronokv::Database::open(mem);
        auto t1 = db.begin(); (void)t1.get("absent_k");
        for (int i = 0; i < 50; ++i) { auto x = db.begin(); x.put("noise" + std::to_string(i), "v"); x.commit(); }
        db.put("absent_k", "inserted");
        t1.put("w", "1");
        chk("2a absent-read conflict survives prune pressure", t1.commit() == chronokv::Status::Conflict);
    }
    // 2b: insert-then-delete (net absent again) -> still conflicts (conservative, sound)
    {
        auto db = chronokv::Database::open(mem);
        auto t1 = db.begin(); (void)t1.get("absent_k");
        { auto w = db.begin(); w.put("absent_k", "v"); (void)w.commit(); }
        db.erase("absent_k");
        t1.put("w", "1");
        auto r = t1.commit();
        chk("2b insert+delete net-absent still conflicts (conservative)", r == chronokv::Status::Conflict,
            "r=" + std::to_string((int)r));
    }
    // 2c: absent read + self-insert in SAME txn -> must NOT self-conflict
    {
        auto db = chronokv::Database::open(mem);
        auto t1 = db.begin(); bool absent = !t1.get("k").has_value(); t1.put("k", "v");
        auto r = t1.commit();
        chk("2c check-then-insert same txn commits", absent && r == chronokv::Status::OK, "r=" + std::to_string((int)r));
    }
    // 2d: empty key edge
    {
        auto db = chronokv::Database::open(mem);
        auto t1 = db.begin(); bool absent = !t1.get("").has_value();
        db.put("", "inserted");
        t1.put("w", "1");
        auto r = t1.commit();
        chk("2d empty-key absent read conflicts on insert", absent && r == chronokv::Status::Conflict, "r=" + std::to_string((int)r));
    }
    // 2e: max-size key edge
    {
        auto db = chronokv::Database::open(mem);
        std::string big(4048, 'B');
        auto t1 = db.begin(); bool absent = !t1.get(big).has_value();
        db.put(big, "inserted");
        t1.put("w", "1");
        auto r = t1.commit();
        chk("2e 4048-byte key absent read conflicts on insert", absent && r == chronokv::Status::Conflict, "r=" + std::to_string((int)r));
    }
    // 2f: oversized key in read set must not break commit
    {
        auto db = chronokv::Database::open(mem);
        auto t1 = db.begin(); (void)t1.get(std::string(5000, 'X')); t1.put("w", "1");
        auto r = t1.commit();
        chk("2f 5000-byte key in read set: commit still OK", r == chronokv::Status::OK, "r=" + std::to_string((int)r));
    }
}

// ================= Hunt class 3: WAL-1 classifier =================
static std::vector<uint8_t> frame(uint64_t lsn, uint64_t cts, const std::string& k, const std::string& v) {
    WriteSet ws{{k, v, false}};
    return wal_frame(wal_prepend_lsn(lsn, wal_ser(cts, ws)));
}
static void class3() {
    std::cout << "== class 3: WAL-1 adversarial classification\n";
    auto f1 = frame(1, 10, "a", "1"), f2 = frame(2, 11, "b", "2"), f3 = frame(3, 12, "c", "3");
    auto cat = [](std::initializer_list<std::vector<uint8_t>> parts) {
        std::vector<uint8_t> b; for (auto& p : parts) b.insert(b.end(), p.begin(), p.end()); return b;
    };
    auto corrupt_len_at = [](std::vector<uint8_t> b, size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[off + i] = (v >> (8 * i)) & 0xFF; return b;
    };
    // 3a: interior len corruption with valid frame later -> CORRUPT
    {
        auto b = corrupt_len_at(cat({f1, f2, f3}), f1.size(), 0xFFFFFFFFu);
        auto [st, out] = wal_recover_buf(b);
        chk("3a interior len=0xFFFFFFFF -> CORRUPT", st == WalStatus::CORRUPT && out.size() == 1);
    }
    // 3b: interior len+1 (CRC breaks) -> CORRUPT (valid f3 found by offset scan)
    {
        uint32_t l2 = 0; for (int i = 0; i < 4; ++i) l2 |= uint32_t(f2[i]) << (8*i);
        auto b = corrupt_len_at(cat({f1, f2, f3}), f1.size(), l2 + 1);
        auto [st, out] = wal_recover_buf(b);
        chk("3b interior len+1 -> CORRUPT", st == WalStatus::CORRUPT && out.size() == 1);
    }
    // 3c: TWO consecutive damaged frames, valid f3 after -> still CORRUPT
    {
        auto f4 = frame(4, 13, "d", "4");
        auto b = corrupt_len_at(cat({f1, f2, f3, f4}), f1.size(), 0xFFFFFFFFu);
        b = corrupt_len_at(std::move(b), f1.size() + f2.size(), 0xFFFFFFFEu);
        auto [st, out] = wal_recover_buf(b);
        chk("3c two damaged frames, VALID f4 after -> CORRUPT", st == WalStatus::CORRUPT && out.size() == 1,
            "st=" + std::to_string((int)st) + " out=" + std::to_string(out.size()));
    }
    // 3d: true torn tail -> TORN_TAIL, prefix recovered
    {
        auto b = cat({f1, f2, f3}); b.resize(b.size() - 5);
        auto [st, out] = wal_recover_buf(b);
        chk("3d torn tail -> TORN_TAIL with prefix", st == WalStatus::TORN_TAIL && out.size() == 2);
    }
    // 3e: stale duplicate (valid CRC, LOWER lsn) after damage -> TORN_TAIL (D2 shape: truncate rollback debris)
    {
        auto b = corrupt_len_at(cat({f1, f2}), f1.size(), 0xFFFFFFFFu);
        b.insert(b.end(), f1.begin(), f1.end());   // stale lsn=1 duplicate, CRC-valid
        auto [st, out] = wal_recover_buf(b);
        chk("3e stale CRC-valid duplicate -> TORN_TAIL", st == WalStatus::TORN_TAIL && out.size() == 1);
    }
    // 3f: far-future LSN jump (>2^32) after damage -> TORN_TAIL (contiguous allocation makes it unreachable)
    {
        auto far = frame(uint64_t(1ull << 33) + 5, 99, "z", "9");
        auto b = corrupt_len_at(cat({f1, f2}), f1.size(), 0xFFFFFFFFu);
        b.insert(b.end(), far.begin(), far.end());
        auto [st, out] = wal_recover_buf(b);
        chk("3f LSN jump > 2^32 -> TORN_TAIL", st == WalStatus::TORN_TAIL && out.size() == 1);
    }
    // 3g: worst-case scan cost — corruption at offset 0 of an 8 MiB buffer of valid frames
    {
        std::vector<uint8_t> big; uint64_t lsn = 1;
        while (big.size() < (8u << 20)) { auto f = frame(lsn, 1000 + lsn, "key" + std::to_string(lsn), std::string(90, 'v')); big.insert(big.end(), f.begin(), f.end()); ++lsn; }
        for (int i = 0; i < 4; ++i) big[i] = 0xFF;
        auto t0 = steady_clock::now();
        auto [st, out] = wal_recover_buf(big);
        auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        chk("3g 8MiB damage-at-offset-0 classification", st == WalStatus::TORN_TAIL || st == WalStatus::CORRUPT,
            "status=" + std::to_string((int)st) + " time=" + std::to_string(ms) + "ms (one-shot on the loud-failure path)");
    }
}

// ================= Hunt class 4: BT-1 headroom vs planner worst case =================
static void class4() {
    std::cout << "== class 4: BT-1 adversarial packing regimes\n";
    const size_t B = 4064; // PAGE_ENTRY_BUDGET
    // regimes: (a) entry cost just over B/2 -> one per greedy group; (b) near-max entries
    struct Regime { const char* name; size_t keylen; std::string val; };
    std::vector<Regime> regimes = {
        {"half-plus (key " , 2030, ""},          // cost 8+2030 = 2038 > B/2
        {"near-max", 4050, ""},                  // cost 8+4050 = 4058 ~ B
        {"thirds", 1350, ""},                    // cost 1358 ~ B/3: packs 3 per page
    };
    regimes[0].name = "half-plus";
    int lost_total = 0, combos = 0, threw = 0;
    size_t max_alloc_delta = 0;
    for (auto& rg : regimes)
    for (int shuffled = 0; shuffled < 2; ++shuffled)
    for (size_t pages : {3, 5, 8, 13, 21, 34, 55})
    for (size_t S : {200, 400}) {
        chronokv_page::PagePool pool(pages * 4096);
        chronokv_btree::BTree t(pool);
        std::vector<std::string> keys;
        for (size_t i = 0; i < S; ++i) { char b[16]; snprintf(b, sizeof b, "%06zu", i); std::string k = b; k.resize(rg.keylen, 'x'); keys.push_back(k); }
        if (shuffled) std::shuffle(keys.begin(), keys.end(), std::mt19937(11));
        std::vector<std::string> ok; bool bad_throw = false;
        for (auto& k : keys) {
            size_t a0 = pool.allocated();
            try { t.put(k, rg.val); ok.push_back(k); size_t d = pool.allocated() - a0; if (d > max_alloc_delta) max_alloc_delta = d; }
            catch (const std::bad_alloc&) { threw = 1; break; }
            catch (...) { bad_throw = true; break; }
        }
        if (bad_throw) { std::cout << "  [FAIL] unexpected non-bad_alloc throw in regime " << rg.name << "\n"; ++g_fail; continue; }
        ++combos;
        for (auto& k : ok) { std::string out; if (!t.get(k, &out) || out != rg.val) { ++lost_total; break; } }
    }
    chk("4a adversarial regimes: zero lost acked keys on exhaustion", lost_total == 0 && combos > 0 && threw,
        std::to_string(lost_total) + " lost across " + std::to_string(combos) + " combos");
    chk("4b observed per-put alloc delta within headroom bound", max_alloc_delta / 4096 <= 5 * 7 + 1,
        "max delta=" + std::to_string(max_alloc_delta / 4096) + " pages (bound 5*(h+1)+1; h<=6 at these sizes)");
    // 4c: in-place exemption boundary — equal size OK, +1 byte refuses with tree unchanged
    {
        chronokv_page::PagePool pool(16 * 4096); chronokv_btree::BTree t(pool);
        std::string first; size_t n = 0;
        for (; n < 100000; ++n) { char b[24]; snprintf(b, sizeof b, "%08zu", n); std::string k = b; k.resize(100, 'x'); try { t.put(k, "v"); if (n == 0) first = k; } catch (const std::bad_alloc&) { break; } }
        bool eq_ok = true, plus1_threw = false; std::string out;
        try { t.put(first, "w"); } catch (const std::bad_alloc&) { eq_ok = false; }          // same size
        try { t.put(first, "wx"); } catch (const std::bad_alloc&) { plus1_threw = true; }      // +1 byte
        bool unchanged = t.get(first, &out) && out == "w";
        chk("4c exemption boundary: equal-size OK, +1 refuses, tree unchanged", eq_ok && plus1_threw && unchanged);
    }
}

int main() {
    crc_init();
    class1(); class2(); class3(); class4();
    std::cout << (g_fail == 0 ? "DELTA CONFIRMATION PROBES: ALL PASS\n" : "DELTA PROBES FAILURES: " + std::to_string(g_fail) + "\n");
    return g_fail == 0 ? 0 : 1;
}
