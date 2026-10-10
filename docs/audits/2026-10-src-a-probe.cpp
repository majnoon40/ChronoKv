#include "chronokv.hpp"
#include <iostream>
int main() {
    using namespace chronokv;
    crc_init();
    namespace fs = std::filesystem;
    std::string base = "/tmp/src_a_probe"; fs::remove_all(base);
    std::string wal = base + "/wal", ck = base + "/ck";
    fs::create_directories(wal); fs::create_directories(ck);
    Options o; o.wal_dir = wal; o.checkpoint_path = ck + "/ckpt";
    o.durability = DurabilityMode::Sync; o.page_pool_bytes = 16u << 20;
    { auto db = Database::open(o); db.put("a", "1"); db.put("b", "2"); db.close(); }
    { auto db = Database::open(o); db.force_wal_rotation_for_test(); db.put("c", "3"); db.close(); }
    std::cout << "segments on disk:";
    for (auto& e : fs::directory_iterator(wal)) std::cout << " " << e.path().filename().string();
    std::cout << "\n";
    fault::arm(fault::Kind::ManifestOpenFail, 1);
    try {
        auto db = Database::open(o);
        std::cout << "OPEN SUCCEEDED (silent path)\n";
        auto st = db.put("d", "4");
        std::cout << "put(d) -> " << (int)st << "\n";
        std::cout << "health level=" << db.health().level << "\n";
        db.close();
        std::cout << "CLOSE OK\n";
    } catch (const std::exception& e) {
        std::cout << "THREW: " << e.what() << "\n";
    }
    std::cout << "fault remaining=" << fault::remaining.load() << "\n";
    fault::disarm();
    try {
        auto db = Database::open(o);
        std::cout << "reopen: a=" << db.get("a").has_value() << " b=" << db.get("b").has_value()
                  << " c=" << db.get("c").has_value() << " d=" << db.get("d").has_value() << "\n";
        db.close();
    } catch (const std::exception& e) { std::cout << "reopen THREW: " << e.what() << "\n"; }
    return 0;
}
