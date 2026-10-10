// SRC-B reproduction probe (contract question — REPORT ONLY, no engine change).
// B1: rollback ftruncate SUCCEEDS, its fsync FAILS  -> FATAL, fail-stop; clean
//     restart: truncation was in page cache -> visible -> no resurrection
//     (power loss could still undo it — that is the documented window).
// B2: rollback ftruncate ITSELF FAILS               -> WARNING; the complete
//     CRC-valid frame of the REJECTED batch stays in the file; clean restart
//     replays it -> resurrection WITHOUT any power loss?
#include "chronokv.hpp"
#include <iostream>
static void scenario(const char* name, bool force_truncate_fail) {
    using namespace chronokv;
    namespace fs = std::filesystem;
    std::cout << "\n===== " << name << " =====\n";
    std::string base = std::string("/tmp/src_b_") + (force_truncate_fail ? "b2" : "b1");
    fs::remove_all(base);
    std::string wal = base + "/wal";
    fs::create_directories(wal);
    Options o; o.wal_dir = wal; o.durability = DurabilityMode::Sync; o.page_pool_bytes = 16u << 20;
    o.auto_start_gc = false;
    auto db = Database::open(o);
    std::cout << "setup put(acked) -> " << (int)db.put("acked", "1") << " (0=OK)\n";
    if (force_truncate_fail) {
        fault::force_for_coverage(fault::Kind::TruncateFail, 4);  // fires at EVERY ftruncate site (there is one)
        fault::arm(fault::Kind::FsyncFail, 1);                     // fails the batch -> rollback path
    } else {
        fault::arm(fault::Kind::FsyncFail, 2);   // hit1: batch fsync; hit2: rollback fsync (truncate succeeded)
    }
    Status s1 = Status::OK;
    try { s1 = db.put("rejected", "2"); }
    catch (const std::exception& e) { std::cout << "put THREW: " << e.what() << "\n"; s1 = Status::Failed; }
    Health h = db.health();
    auto ws = db.wal_stats();
    std::cout << "put(rejected) -> " << (int)s1 << "  (WalFailure=" << (int)Status::WalFailure
              << ", Failed=" << (int)Status::Failed << ")\n";
    std::cout << "health: level=" << h.level;
    for (auto& r : h.reasons) std::cout << "  reason='" << r << "'";
    std::cout << "\nwal_stats: fsync_fails=" << ws.fsync_fails << " truncations=" << ws.truncations
              << " truncate_fails=" << ws.truncate_fails << "\n";
    std::cout << "post-fail put(after) -> " << (int)db.put("after", "3") << " (expect Failed=6: fail-stop)\n";
    std::cout << "read of durable data still works: get(acked)=" << (db.get("acked") ? 1 : 0) << "\n";
    try { db.close(); } catch (const std::exception& e) { std::cout << "close threw: " << e.what() << "\n"; }
    // CLEAN restart (no power loss — page cache intact; _exit-style crash coverage
    // is not claimed here, per the task's modeling limits).
    try {
        auto db2 = Database::open(o);
        bool res = db2.get("rejected").has_value();
        std::cout << "reopen: acked=" << db2.get("acked").has_value()
                  << "  RESURRECTED(rejected)=" << res
                  << "  after=" << db2.get("after").has_value() << "\n";
        db2.close();
    } catch (const std::exception& e) { std::cout << "reopen THREW: " << e.what() << "\n"; }
    fault::disarm();
    fs::remove_all(base);
}
int main() {
    crc_init();
    scenario("B1: rollback fsync fails AFTER a successful ftruncate", false);
    scenario("B2: the rollback ftruncate ITSELF fails", true);
    return 0;
}
