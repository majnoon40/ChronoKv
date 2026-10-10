// bench/baselines.cpp — vendored-baseline adapters for the ChronoKV arena
// (v29 M1 step 3, START).
//
// Roadmap anchor: "Vendored baselines, same harness, same hardware,
// methodology written down once and applied identically: SQLite (WAL mode;
// synchronous=FULL and =NORMAL), LMDB (defaults), RocksDB (defaults + one
// tuned config)." Rule 10: baselines are BENCHMARKS, NOT DEPENDENCIES —
// this file is the only place in the repository that includes a third-party
// storage header, it lives under bench/, and nothing here is reachable from
// chronokv.hpp or main.cpp.
//
// Fairness protocol (identical to arena.cpp, by construction):
//   * same 16-byte key encoding (key16), same value geometry, same seeds,
//     same uniform/zipfian(0.99)-CDF generators (duplicated below with
//     attribution — consolidate into bench/arena_util.hpp when the CI
//     ledger lands, M1 step 4);
//   * one logical operation == one durability unit: every write is its own
//     transaction/commit (SQLite BEGIN IMMEDIATE..COMMIT, LMDB write txn,
//     RocksDB Put with the variant's WriteOptions);
//   * every run prints the rule-9 methodology header (engine, config,
//     engine version, hardware, geometry, durability class) before the TSV;
//   * the durability-class mapping is printed, not implied:
//       sqlite-full   WAL + synchronous=FULL   power-loss safe  (vs ChronoKV Sync/Group)
//       sqlite-normal WAL + synchronous=NORMAL process-crash    (vs ChronoKV Async)
//       lmdb          default synchronous txns power-loss safe
//       rocksdb-sync  WriteOptions.sync=true   power-loss safe
//       rocksdb-tuned sync=false + WAL, 128 MB memtable, no compression,
//                     4 bg jobs                process-crash   (documented tune)
//
// Workloads (the comparable core — the full arena set, step 3 COMPLETE):
//   fillseq, fillrandom, readrandom (uniform), overwrite, ycsb_a..ycsb_f.
//   D/E/F mirror bench/arena.cpp's w_ycsb semantics exactly: D's
//   read-latest is the geometric(0.001) tail over the inserted prefix
//   (documented deviation from YCSB's exact latest generator, same shape,
//   identical in both harnesses); E scans zipfian starts with len 1..100
//   and materializes rows; F records the combined read+write latency.
//
// Build:  make arena-baselines        (auto-detects sqlite3/lmdb/rocksdb headers)
//         small boxes: make arena-baselines ARENA_FLAGS="-O1"
// Run:    ./build/arena/baselines --engine sqlite-full --keys 100000 ...
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/utsname.h>
#include <unistd.h>

#ifdef CKV_BASE_SQLITE
#include <sqlite3.h>
#endif
#ifdef CKV_BASE_LMDB
#include <lmdb.h>
#endif
#ifdef CKV_BASE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#endif

#ifndef ARENA_BUILD_INFO
#define ARENA_BUILD_INFO "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
double elapsed_s(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// ---- shared utilities, duplicated from bench/arena.cpp (attribution per
// ---- rule 9; consolidate into bench/arena_util.hpp at M1 step 4) ----

std::string key16(uint64_t i) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llu", (unsigned long long)i);
    return std::string(b, 16);
}

std::string make_value(std::mt19937_64& rng, size_t n) {
    std::string v(n, '\0');
    for (size_t i = 0; i < n; ++i) v[i] = char('a' + (rng() % 26));
    return v;
}

class Zipfian {   // same CDF variant as the arena — fairness is within-harness
    std::vector<double> cdf_;
public:
    explicit Zipfian(size_t n, double theta = 0.99) {
        if (n == 0) return;
        cdf_.resize(n);
        double sum = 0.0, acc = 0.0;
        for (size_t i = 0; i < n; ++i)
            sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
        for (size_t i = 0; i < n; ++i) {
            acc += 1.0 / std::pow(static_cast<double>(i + 1), theta);
            cdf_[i] = acc / sum;
        }
    }
    size_t next(std::mt19937_64& rng) const {
        if (cdf_.empty()) return 0;
        const double u = std::generate_canonical<double, 53>(rng);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        if (it == cdf_.end()) --it;
        return static_cast<size_t>(it - cdf_.begin());
    }
};

class Recorder {
    std::vector<double> us_;
public:
    void reserve(size_t n) { us_.reserve(n); }
    void record(double us) { us_.push_back(us); }
    void merge(Recorder&& o) {
        us_.insert(us_.end(), o.us_.begin(), o.us_.end());
        o.us_.clear();
    }
    size_t size() const { return us_.size(); }
    double pct(double q) {
        if (us_.empty()) return -1.0;
        std::sort(us_.begin(), us_.end());
        return us_[(size_t)(q * (double)(us_.size() - 1) + 0.5)];
    }
};

long proc_status_kb(const char* field) {
    std::ifstream f("/proc/self/status");
    std::string line;
    const size_t flen = std::strlen(field);
    while (std::getline(f, line))
        if (line.compare(0, flen, field) == 0 && line.size() > flen && line[flen] == ':')
            return std::strtol(line.c_str() + flen + 1, nullptr, 10);
    return -1;
}

std::string first_line_containing(const char* path, const char* needle) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return line;
    return "(unknown)";
}

struct Result {
    std::string workload;
    uint64_t ops = 0;
    double seconds = 0.0;
    double p50 = -1.0, p99 = -1.0;
    std::string note;
};

void emit(const Result& r) {
    const long rss = proc_status_kb("VmRSS");
    const std::string p50s = r.p50 < 0 ? "-" : std::to_string((long long)r.p50);
    const std::string p99s = r.p99 < 0 ? "-" : std::to_string((long long)r.p99);
    char line[1024];
    std::snprintf(line, sizeof line, "%s\t%llu\t%.3f\t%.0f\t%s\t%s\t%.1f\t%s",
        r.workload.c_str(), (unsigned long long)r.ops, r.seconds,
        r.seconds > 0 ? (double)r.ops / r.seconds : 0.0,
        p50s.c_str(), p99s.c_str(), rss / 1024.0, r.note.c_str());
    std::cout << line << "\n";
}

// ---- engine adapter interface ----

struct Engine {
    virtual ~Engine() = default;
    virtual std::string describe() const = 0;   // methodology: engine+config+version
    virtual std::string durability_class() const = 0;
    virtual bool put(unsigned tid, const std::string& k, const std::string& v,
                     std::string* err) = 0;
    virtual bool get(unsigned tid, const std::string& k, std::string* out,
                     std::string* err) = 0;
    // Range scan [lo, hi] INCLUSIVE over the key16 space, materializing
    // every row (key AND value bytes touched — the arena's range_scan
    // returns vector<pair<string,string>>, so a count-only baseline scan
    // would do strictly less work: a fairness requirement, not a detail).
    // Appends to *rows; returns false on engine error (*err set).
    virtual bool scan(unsigned tid, const std::string& lo, const std::string& hi,
                      std::vector<std::pair<std::string, std::string>>* rows,
                      std::string* err) = 0;
    virtual void close() = 0;
};

// ---- SQLite (WAL; synchronous=FULL|NORMAL; one txn per write op) ----
#ifdef CKV_BASE_SQLITE
class SqliteEngine : public Engine {
    std::vector<sqlite3*> conns_;
    std::vector<sqlite3_stmt*> puts_, gets_, scans_;
    bool full_;
    std::string path_;
    static int exec_(sqlite3* db, const char* sql) {
        char* e = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &e);
        sqlite3_free(e);
        return rc;
    }
public:
    SqliteEngine(const std::string& dir, bool full, unsigned threads, std::string* err)
        : full_(full), path_(dir + "/sqlite.db") {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        conns_.resize(threads, nullptr);
        puts_.resize(threads, nullptr);
        gets_.resize(threads, nullptr);
        scans_.resize(threads, nullptr);
        for (unsigned t = 0; t < threads; ++t) {
            if (sqlite3_open(path_.c_str(), &conns_[t]) != SQLITE_OK) {
                *err = "sqlite3_open failed"; return;
            }
            exec_(conns_[t], "PRAGMA busy_timeout=30000");
            exec_(conns_[t], "PRAGMA journal_mode=WAL");
            exec_(conns_[t], full_ ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=NORMAL");
            if (exec_(conns_[t],
                      "CREATE TABLE IF NOT EXISTS kv(k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID")
                != SQLITE_OK) { *err = "create table failed"; return; }
            sqlite3_prepare_v2(conns_[t], "INSERT OR REPLACE INTO kv VALUES(?,?)", -1, &puts_[t], nullptr);
            sqlite3_prepare_v2(conns_[t], "SELECT v FROM kv WHERE k=?", -1, &gets_[t], nullptr);
            sqlite3_prepare_v2(conns_[t], "SELECT k,v FROM kv WHERE k>=? AND k<=?", -1, &scans_[t], nullptr);
        }
    }
    std::string describe() const override {
        return std::string("sqlite3 WAL synchronous=") + (full_ ? "FULL" : "NORMAL") +
               " table=WITHOUT-ROWID blob-pk; one txn per write; version=" + sqlite3_libversion();
    }
    std::string durability_class() const override {
        return full_ ? "power-loss (compare: ChronoKV Sync/Group)"
                     : "process-crash (compare: ChronoKV Async)";
    }
    bool put(unsigned tid, const std::string& k, const std::string& v, std::string* err) override {
        sqlite3* db = conns_[tid];
        if (exec_(db, "BEGIN IMMEDIATE") != SQLITE_OK) {
            *err = std::string("BEGIN: ") + sqlite3_errmsg(db);
            return false;
        }
        sqlite3_reset(puts_[tid]);
        sqlite3_bind_blob(puts_[tid], 1, k.data(), (int)k.size(), SQLITE_STATIC);
        sqlite3_bind_blob(puts_[tid], 2, v.data(), (int)v.size(), SQLITE_STATIC);
        if (sqlite3_step(puts_[tid]) != SQLITE_DONE) {
            *err = sqlite3_errmsg(db); exec_(db, "ROLLBACK"); return false;
        }
        if (exec_(db, "COMMIT") != SQLITE_OK) {
            *err = std::string("COMMIT: ") + sqlite3_errmsg(db);
            return false;
        }
        return true;
    }
    bool get(unsigned tid, const std::string& k, std::string* out, std::string* err) override {
        sqlite3_reset(gets_[tid]);
        sqlite3_bind_blob(gets_[tid], 1, k.data(), (int)k.size(), SQLITE_STATIC);
        const int rc = sqlite3_step(gets_[tid]);
        bool found = false;
        if (rc == SQLITE_ROW) {
            const unsigned char* b = (const unsigned char*)sqlite3_column_blob(gets_[tid], 0);
            const int n = sqlite3_column_bytes(gets_[tid], 0);
            out->assign((const char*)b, (size_t)n);
            found = true;
        } else if (rc != SQLITE_DONE) {
            *err = sqlite3_errmsg(conns_[tid]);
        }
        // RESET IMMEDIATELY after extracting the row (scan() below obeys
        // the same rule for the same reason). A SELECT left sitting
        // on its result row keeps this connection's WAL read snapshot OPEN;
        // the next BEGIN IMMEDIATE on the SAME connection then conflicts
        // with its own read txn and — per SQLite's deadlock-avoidance rule
        // — returns SQLITE_BUSY IMMEDIATELY WITHOUT invoking the busy
        // handler ("database is locked" despite busy_timeout=30000). In a
        // mixed read/update workload that also deflates update latencies
        // (a failed BEGIN is fast) — a fairness bug, not just a flake.
        sqlite3_reset(gets_[tid]);
        return found || rc == SQLITE_DONE;
    }
    bool scan(unsigned tid, const std::string& lo, const std::string& hi,
              std::vector<std::pair<std::string, std::string>>* rows,
              std::string* err) override {
        sqlite3_stmt* st = scans_[tid];
        sqlite3_reset(st);
        sqlite3_bind_blob(st, 1, lo.data(), (int)lo.size(), SQLITE_STATIC);
        sqlite3_bind_blob(st, 2, hi.data(), (int)hi.size(), SQLITE_STATIC);
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const unsigned char* kb = (const unsigned char*)sqlite3_column_blob(st, 0);
            const unsigned char* vb = (const unsigned char*)sqlite3_column_blob(st, 1);
            rows->emplace_back(std::string((const char*)kb, (size_t)sqlite3_column_bytes(st, 0)),
                               std::string((const char*)vb, (size_t)sqlite3_column_bytes(st, 1)));
        }
        // RESET BEFORE RETURNING — the WAL read-snapshot trap documented at
        // get() is LIVE for ycsb_e: the same connection alternates scans and
        // insert txns, and an unreset scan statement makes the next BEGIN
        // IMMEDIATE fail SQLITE_BUSY without consulting the busy handler.
        sqlite3_reset(st);
        if (rc != SQLITE_DONE) { *err = sqlite3_errmsg(conns_[tid]); return false; }
        return true;
    }
    void close() override {
        for (size_t t = 0; t < conns_.size(); ++t) {
            if (puts_[t]) sqlite3_finalize(puts_[t]);
            if (gets_[t]) sqlite3_finalize(gets_[t]);
            if (scans_[t]) sqlite3_finalize(scans_[t]);
            if (conns_[t]) sqlite3_close(conns_[t]);
        }
        conns_.clear();
    }
};
#endif

// ---- LMDB (defaults: synchronous commits; single-writer by design) ----
#ifdef CKV_BASE_LMDB
class LmdbEngine : public Engine {
    MDB_env* env_ = nullptr;
    MDB_dbi dbi_ = 0;
    std::string dir_;
public:
    LmdbEngine(const std::string& dir, std::string* err) : dir_(dir) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        int rc = mdb_env_create(&env_);
        if (rc) { *err = mdb_strerror(rc); return; }
        mdb_env_set_mapsize(env_, 64ULL * 1024 * 1024 * 1024);   // sparse reservation
        mdb_env_set_maxdbs(env_, 1);
        mdb_env_set_maxreaders(env_, 4096);
        rc = mdb_env_open(env_, dir.c_str(), 0, 0664);
        if (rc) { *err = mdb_strerror(rc); return; }
        // PRIME the default DBI: until mdb_dbi_open runs, txn->mt_numdbs is
        // 0 and EVERY mdb_put/mdb_get on dbi 0 fails EINVAL (verified on
        // Debian lmdb 0.9.24; the official LMDB samples all open the dbi
        // first — skipping it is the classic 'Invalid argument' trap).
        MDB_txn* txn = nullptr;
        if ((rc = mdb_txn_begin(env_, nullptr, 0, &txn))) { *err = mdb_strerror(rc); return; }
        if ((rc = mdb_dbi_open(txn, nullptr, 0, &dbi_))) {
            *err = std::string("dbi_open: ") + mdb_strerror(rc);
            mdb_txn_abort(txn); return;
        }
        if ((rc = mdb_txn_commit(txn))) { *err = mdb_strerror(rc); return; }
    }
    std::string describe() const override {
        int major, minor, patch; const char* ver;
        ver = mdb_version(&major, &minor, &patch);
        char b[128];
        std::snprintf(b, sizeof b, "lmdb defaults (synchronous commits), mapsize=64GiB sparse; version=%s %d.%d.%d",
                      ver, major, minor, patch);
        return b;
    }
    std::string durability_class() const override {
        return "power-loss (compare: ChronoKV Sync/Group)";
    }
    bool put(unsigned, const std::string& k, const std::string& v, std::string* err) override {
        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(env_, nullptr, 0, &txn);   // LMDB serializes writers internally
        if (rc) { *err = mdb_strerror(rc); return false; }
        MDB_val mk{(size_t)k.size(), (void*)k.data()}, mv{(size_t)v.size(), (void*)v.data()};
        rc = mdb_put(txn, dbi_, &mk, &mv, 0);
        if (rc) { *err = mdb_strerror(rc); mdb_txn_abort(txn); return false; }
        rc = mdb_txn_commit(txn);
        if (rc) { *err = mdb_strerror(rc); return false; }
        return true;
    }
    bool get(unsigned, const std::string& k, std::string* out, std::string* err) override {
        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
        if (rc) { *err = mdb_strerror(rc); return false; }
        MDB_val mk{(size_t)k.size(), (void*)k.data()}, mv{};
        rc = mdb_get(txn, dbi_, &mk, &mv);
        if (rc == 0) out->assign((const char*)mv.mv_data, mv.mv_size);
        else if (rc != MDB_NOTFOUND) { *err = mdb_strerror(rc); mdb_txn_abort(txn); return false; }
        mdb_txn_abort(txn);
        return true;
    }
    bool scan(unsigned, const std::string& lo, const std::string& hi,
              std::vector<std::pair<std::string, std::string>>* rows,
              std::string* err) override {
        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
        if (rc) { *err = mdb_strerror(rc); return false; }
        MDB_cursor* cur = nullptr;
        if ((rc = mdb_cursor_open(txn, dbi_, &cur))) {
            *err = mdb_strerror(rc); mdb_txn_abort(txn); return false;
        }
        MDB_val mk{(size_t)lo.size(), (void*)lo.data()}, mv{};
        rc = mdb_cursor_get(cur, &mk, &mv, MDB_SET_RANGE);
        while (rc == 0) {
            std::string k((const char*)mk.mv_data, mk.mv_size);
            if (k > hi) break;
            rows->emplace_back(std::move(k), std::string((const char*)mv.mv_data, mv.mv_size));
            rc = mdb_cursor_get(cur, &mk, &mv, MDB_NEXT);
        }
        if (rc != MDB_NOTFOUND) { if (rc) *err = mdb_strerror(rc); }
        mdb_cursor_close(cur);
        mdb_txn_abort(txn);
        return rc == MDB_NOTFOUND || rc == 0;
    }
    void close() override { if (env_) { mdb_env_close(env_); env_ = nullptr; } }
};
#endif

// ---- RocksDB (defaults + sync writes; one documented tuned config) ----
#ifdef CKV_BASE_ROCKSDB
class RocksEngine : public Engine {
    std::unique_ptr<rocksdb::DB> db_;
    rocksdb::WriteOptions wo_;
    bool tuned_;
public:
    RocksEngine(const std::string& dir, bool tuned, std::string* err) : tuned_(tuned) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        // create_if_missing makes the DB dir itself but NOT missing parents;
        // the arena passes nested dirs (data_dir/engine) — create them here
        // like the other adapters do.
        std::filesystem::create_directories(dir, ec);
        if (ec) { *err = "cannot create " + dir + ": " + ec.message(); return; }
        rocksdb::Options o;
        o.create_if_missing = true;
        if (tuned_) {
            o.write_buffer_size = 128ULL * 1024 * 1024;
            o.max_background_jobs = 4;
            o.compression = rocksdb::kNoCompression;
        }
        wo_.sync = !tuned_;   // rocksdb-sync: per-write fsync; tuned: WAL on, sync off (documented)
        rocksdb::DB* raw = nullptr;
        auto s = rocksdb::DB::Open(o, dir, &raw);
        if (!s.ok()) { *err = s.ToString(); return; }
        db_.reset(raw);
    }
    std::string describe() const override {
        return std::string(tuned_ ? "rocksdb TUNED (sync=false + WAL, memtable=128MiB, no compression, 4 bg jobs)"
                                  : "rocksdb defaults + WriteOptions.sync=true") +
               "; version=" + rocksdb::GetRocksVersionAsString();
    }
    std::string durability_class() const override {
        return tuned_ ? "process-crash (compare: ChronoKV Async)"
                      : "power-loss (compare: ChronoKV Sync/Group)";
    }
    bool put(unsigned, const std::string& k, const std::string& v, std::string* err) override {
        auto s = db_->Put(wo_, k, v);
        if (!s.ok()) { *err = s.ToString(); return false; }
        return true;
    }
    bool get(unsigned, const std::string& k, std::string* out, std::string* err) override {
        auto s = db_->Get(rocksdb::ReadOptions(), k, out);
        if (s.ok() || s.IsNotFound()) return true;
        *err = s.ToString();
        return false;
    }
    bool scan(unsigned, const std::string& lo, const std::string& hi,
              std::vector<std::pair<std::string, std::string>>* rows,
              std::string* err) override {
        const rocksdb::Slice hi_s(hi);
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
        for (it->Seek(lo); it->Valid() && it->key().compare(hi_s) <= 0; it->Next())
            rows->emplace_back(it->key().ToString(), it->value().ToString());
        if (!it->status().ok()) { *err = it->status().ToString(); return false; }
        return true;
    }
    void close() override { db_.reset(); }
};
#endif

// ---- config / CLI ----

struct Config {
    size_t keys = 1000000;
    size_t valsize = 100;
    unsigned threads = 4;
    uint64_t seed = 42;
    size_t ops = 0;
    std::string dir = "/tmp/ckv_baselines";
    std::string engine = "sqlite-full";
    std::string workloads = "fillseq,fillrandom,readrandom,overwrite,ycsb_a,ycsb_b,ycsb_c,ycsb_d,ycsb_e,ycsb_f";
    bool keep = false;
};

std::unique_ptr<Engine> make_engine(const Config& c, std::string* err) {
    const std::string d = c.dir + "/" + c.engine;
    std::unique_ptr<Engine> out;
#ifdef CKV_BASE_SQLITE
    if (c.engine == "sqlite-full")
        out = std::make_unique<SqliteEngine>(d, true, c.threads, err);
    else if (c.engine == "sqlite-normal")
        out = std::make_unique<SqliteEngine>(d, false, c.threads, err);
#endif
#ifdef CKV_BASE_LMDB
    if (!out && c.engine == "lmdb")
        out = std::make_unique<LmdbEngine>(d, err);
#endif
#ifdef CKV_BASE_ROCKSDB
    if (!out && c.engine == "rocksdb-sync")
        out = std::make_unique<RocksEngine>(d, false, err);
    else if (!out && c.engine == "rocksdb-tuned")
        out = std::make_unique<RocksEngine>(d, true, err);
#endif
    if (!out) {
        if (err->empty())
            *err = "engine '" + c.engine + "' not compiled in (see make arena-baselines detection)";
        return nullptr;
    }
    // Constructor failures must not return a hollow engine (the pre-fix
    // Rocks path segfaulted on the first put with a null db_ because the
    // Open failure only wrote to *err).
    if (!err->empty()) return nullptr;
    return out;
}

void print_methodology(const Config& c, const Engine& e) {
    struct utsname u{};
    uname(&u);
    std::cout << "# chronokv-arena BASELINES (v29 M1 step 3) — methodology (rule 9)\n"
              << "# engine=" << e.describe() << "\n"
              << "# durability_class=" << e.durability_class() << "\n"
              << "# build=" << ARENA_BUILD_INFO << "\n"
              << "# kernel=" << u.sysname << " " << u.release << " (" << u.machine << ")\n"
              << "# cpus=" << sysconf(_SC_NPROCESSORS_ONLN) << "\n"
              << "# " << first_line_containing("/proc/meminfo", "MemTotal") << "\n"
              << "# " << first_line_containing("/proc/cpuinfo", "model name") << "\n"
              << "# data_dir=" << c.dir << " (backing fs not classified in the skeleton)\n"
              << "# threads=" << c.threads << " keys=" << c.keys << " valsize=" << c.valsize
              << " seed=" << c.seed << " ops=" << (c.ops ? c.ops : c.keys) << "\n"
              << "# fairness: same key/value/seed/distribution code as bench/arena.cpp;"
                 " one txn/commit per write op; rule 10 — baselines live under bench/ only\n"
              << "# columns: workload\tops\tseconds\tops_per_sec\tp50_us\tp99_us\trss_mb\tnote\n";
}

// ---- workload drivers (engine-agnostic) ----

bool prep_fill(Engine& e, const Config& c, std::mt19937_64& rng, bool shuffle,
               std::string* err) {
    std::vector<uint64_t> order(c.keys);
    for (size_t i = 0; i < c.keys; ++i) order[i] = i;
    if (shuffle)
        for (size_t i = c.keys; i-- > 1;) {
            size_t j = (size_t)(rng() % (i + 1));
            std::swap(order[i], order[j]);
        }
    for (size_t i = 0; i < c.keys; ++i)
        if (!e.put(0, key16(order[i]), make_value(rng, c.valsize), err))
            return false;
    return true;
}

Result w_fill(Engine& e, const Config& c, bool sequential) {
    Result r; r.workload = sequential ? "fillseq" : "fillrandom";
    std::mt19937_64 rng(c.seed);
    std::string err;
    const auto t0 = Clock::now();
    const bool ok = prep_fill(e, c, rng, !sequential, &err);
    r.seconds = elapsed_s(t0);
    r.ops = ok ? c.keys : 0;
    r.note = ok ? "single-writer" : std::string("ABORTED: ") + err;
    return r;
}

Result w_readrandom(Engine& e, const Config& c) {
    Result r; r.workload = "readrandom";
    std::mt19937_64 rng(c.seed);
    std::string err;
    if (!prep_fill(e, c, rng, false, &err)) { r.note = "prep failed: " + err; return r; }
    const size_t per = std::max<size_t>(1000, c.keys / 4 / c.threads);
    std::vector<Recorder> recs(c.threads);
    const auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < c.threads; ++t) ts.emplace_back([&, t] {
        std::mt19937_64 lrng(c.seed * 1000 + t);
        recs[t].reserve(per);
        std::string out, e2;
        for (size_t i = 0; i < per; ++i) {
            const auto s0 = Clock::now();
            e.get(t, key16(lrng() % c.keys), &out, &e2);
            recs[t].record(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
        }
    });
    for (auto& th : ts) th.join();
    r.seconds = elapsed_s(t0);
    Recorder m; for (auto& rc : recs) m.merge(std::move(rc));
    r.ops = m.size(); r.p50 = m.pct(0.50); r.p99 = m.pct(0.99);
    r.note = "uniform random; threads=" + std::to_string(c.threads);
    return r;
}

Result w_overwrite(Engine& e, const Config& c) {
    Result r; r.workload = "overwrite";
    std::mt19937_64 rng(c.seed);
    std::string err;
    if (!prep_fill(e, c, rng, false, &err)) { r.note = "prep failed: " + err; return r; }
    const size_t n = std::max<size_t>(1000, c.keys / 4);
    std::vector<Recorder> recs(c.threads);
    std::atomic<bool> failed{false};
    const auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < c.threads; ++t) ts.emplace_back([&, t] {
        std::mt19937_64 lrng(c.seed * 2000 + t);
        const size_t per = n / c.threads;
        recs[t].reserve(per);
        std::string e2;
        for (size_t i = 0; i < per; ++i) {
            const auto s0 = Clock::now();
            if (!e.put(t, key16(lrng() % c.keys), make_value(lrng, c.valsize), &e2))
                failed.store(true);
            recs[t].record(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
        }
    });
    for (auto& th : ts) th.join();
    r.seconds = elapsed_s(t0);
    Recorder m; for (auto& rc : recs) m.merge(std::move(rc));
    r.ops = m.size(); r.p50 = m.pct(0.50); r.p99 = m.pct(0.99);
    r.note = std::string("threads=") + std::to_string(c.threads) +
             (failed.load() ? " FAILED-PUTS" : "");
    return r;
}

void w_ycsb(Engine& e, const Config& c, char mix, unsigned read_pct, unsigned update_pct) {
    const std::string wl = std::string("ycsb_") + mix;
    std::mt19937_64 rng(c.seed);
    std::string err;
    if (!prep_fill(e, c, rng, false, &err)) {
        Result r; r.workload = wl; r.note = "ABORTED prep: " + err; emit(r); return;
    }
    const size_t ops = c.ops ? c.ops : c.keys;
    const size_t per = std::max<size_t>(100, ops / c.threads);
    const Zipfian zipf(c.keys);
    std::vector<Recorder> reads(c.threads), updates(c.threads);
    std::atomic<uint64_t> misses{0};
    std::atomic<bool> failed{false};
    std::mutex err_mu; std::string first_err;
    auto capture = [&](const std::string& e2) {
        if (!e2.empty()) { std::lock_guard<std::mutex> g(err_mu); if (first_err.empty()) first_err = e2; }
    };
    const auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < c.threads; ++t) ts.emplace_back([&, t] {
        std::mt19937_64 lrng(c.seed * 7919 + t);
        reads[t].reserve(per); updates[t].reserve(per / 4);
        std::string out, e2;
        for (size_t i = 0; i < per; ++i) {
            const size_t k = zipf.next(lrng);
            const auto s0 = Clock::now();
            const auto us = [&] {
                return std::chrono::duration<double, std::micro>(Clock::now() - s0).count();
            };
            if ((unsigned)(lrng() % 100) < read_pct) {
                if (!e.get(t, key16(k), &out, &e2)) { failed.store(true); capture(e2); }
                else if (out.empty()) misses.fetch_add(1, std::memory_order_relaxed);
                reads[t].record(us());
            } else if (update_pct) {
                if (!e.put(t, key16(k), make_value(lrng, c.valsize), &e2)) { failed.store(true); capture(e2); }
                updates[t].record(us());
            }
        }
    });
    for (auto& th : ts) th.join();
    const double wall = elapsed_s(t0);
    Recorder mr, mu;
    for (auto& rc : reads) mr.merge(std::move(rc));
    for (auto& rc : updates) mu.merge(std::move(rc));
    std::string note = "zipfian(0.99)-cdf threads=" + std::to_string(c.threads) +
        (failed.load() ? " FAILED-OPS" : "") +
        (misses.load() ? " misses=" + std::to_string(misses.load()) : "");
    { std::lock_guard<std::mutex> g(err_mu); if (!first_err.empty()) note += " first_err=" + first_err; }
    if (mr.size()) {
        Result r; r.workload = wl + ".read"; r.ops = mr.size(); r.seconds = wall;
        r.p50 = mr.pct(0.50); r.p99 = mr.pct(0.99); r.note = note; emit(r);
    }
    if (mu.size()) {
        Result r; r.workload = wl + ".update"; r.ops = mu.size(); r.seconds = wall;
        r.p50 = mu.pct(0.50); r.p99 = mu.pct(0.99); r.note = note; emit(r);
    }
}

// ---- YCSB D/E/F (v29 M1 step-3 completion) ----
// Mirrors bench/arena.cpp's w_ycsb operation-for-operation so the ledger's
// D/E/F rows are same-methodology comparisons: per-thread rng seeded
// seed*7919+t; one roll per op with the arena's pick() ORDER (read ->
// update -> rmw -> scan -> insert-fallback); D's read-latest samples the
// geometric(0.001) tail over keys+inserted (the arena's documented
// deviation from YCSB's exact latest generator — IDENTICAL shape in both
// harnesses is what makes the row comparable); inserts extend the keyspace
// past the prep range via one shared counter; E scans zipfian starts with
// len = 1 + rng()%100 (YCSB maxscanlength) and MATERIALIZES rows (the
// arena's range_scan returns pairs; a count-only scan would do less work);
// F's rmw records the combined read+write latency and rewrites the value
// it read (fresh value on a miss), exactly as the arena does.
void w_ycsb_def(Engine& e, const Config& c, char mix) {
    const std::string wl = std::string("ycsb_") + mix;
    // (insert needs no percentage: it is the pick() fallback, exactly as
    // in the arena's w_ycsb — D and E both fall through to it at 5%.)
    unsigned read_pct = 0, scan_pct = 0, rmw_pct = 0;
    if (mix == 'd')      { read_pct = 95; }
    else if (mix == 'e') { scan_pct = 95; }
    else if (mix == 'f') { read_pct = 50; rmw_pct = 50; }
    std::mt19937_64 rng(c.seed);
    std::string err;
    if (!prep_fill(e, c, rng, false, &err)) {
        Result r; r.workload = wl; r.note = "ABORTED prep: " + err; emit(r); return;
    }
    const size_t ops = c.ops ? c.ops : c.keys;
    const size_t per = std::max<size_t>(100, ops / c.threads);
    const Zipfian zipf(c.keys);

    struct St {
        Recorder read, insert, scan, rmw;
        size_t scan_rows = 0;
    };
    std::vector<St> st(c.threads);
    std::atomic<size_t> next_insert{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<bool> failed{false};
    std::mutex err_mu; std::string first_err;
    auto capture = [&](const std::string& e2) {
        if (!e2.empty()) { std::lock_guard<std::mutex> g(err_mu); if (first_err.empty()) first_err = e2; }
    };

    const auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < c.threads; ++t) ts.emplace_back([&, t] {
        std::mt19937_64 lrng(c.seed * 7919 + t);
        St& s = st[t];
        s.read.reserve(per); s.insert.reserve(per / 10);
        std::string out, e2;
        std::vector<std::pair<std::string, std::string>> rows;
        for (size_t i = 0; i < per; ++i) {
            const unsigned roll = (unsigned)(lrng() % 100);
            unsigned acc = 0;
            const auto pick = [&](unsigned pct) { acc += pct; return roll < acc; };
            const auto s0 = Clock::now();
            const auto us = [&] {
                return std::chrono::duration<double, std::micro>(Clock::now() - s0).count();
            };
            if (read_pct && pick(read_pct)) {
                size_t k;
                if (mix == 'd') {   // read-latest: geometric tail over inserted prefix
                    const size_t total = c.keys + next_insert.load(std::memory_order_relaxed);
                    std::geometric_distribution<size_t> geo(0.001);
                    size_t back = std::min(geo(lrng), total - 1);
                    k = total - 1 - back;
                } else {
                    k = zipf.next(lrng);
                }
                out.clear();
                if (!e.get(t, key16(k), &out, &e2)) { failed.store(true); capture(e2); }
                else if (out.empty()) misses.fetch_add(1, std::memory_order_relaxed);
                s.read.record(us());
            } else if (rmw_pct && pick(rmw_pct)) {
                const size_t k = zipf.next(lrng);
                out.clear();
                if (!e.get(t, key16(k), &out, &e2)) { failed.store(true); capture(e2); }
                const std::string nv = out.empty() ? make_value(lrng, c.valsize) : out;
                if (!e.put(t, key16(k), nv, &e2)) { failed.store(true); capture(e2); }
                s.rmw.record(us());                       // combined R+W latency (YCSB F)
            } else if (scan_pct && pick(scan_pct)) {
                const size_t start = zipf.next(lrng);
                const size_t len = 1 + (size_t)(lrng() % 100);   // YCSB maxscanlength=100
                rows.clear();
                if (!e.scan(t, key16(start), key16(start + len), &rows, &e2)) {
                    failed.store(true); capture(e2);
                }
                s.scan_rows += rows.size();
                s.scan.record(us());
            } else {                                        // insert (D and E)
                const size_t k = c.keys + next_insert.fetch_add(1, std::memory_order_relaxed);
                if (!e.put(t, key16(k), make_value(lrng, c.valsize), &e2)) {
                    failed.store(true); capture(e2);
                }
                s.insert.record(us());
            }
        }
    });
    for (auto& th : ts) th.join();
    const double wall = elapsed_s(t0);

    St m;
    for (auto& s : st) {
        m.read.merge(std::move(s.read));
        m.insert.merge(std::move(s.insert));
        m.scan.merge(std::move(s.scan));
        m.rmw.merge(std::move(s.rmw));
        m.scan_rows += s.scan_rows;
    }
    std::string note = "zipfian(0.99)-cdf threads=" + std::to_string(c.threads) +
        (failed.load() ? " FAILED-OPS" : "") +
        (misses.load() ? " misses=" + std::to_string(misses.load()) : "");
    { std::lock_guard<std::mutex> g(err_mu); if (!first_err.empty()) note += " first_err=" + first_err; }
    auto row = [&](const char* op, Recorder& rec) {
        if (rec.size() == 0) return;
        Result r; r.workload = wl + std::string(".") + op;
        r.ops = rec.size(); r.seconds = wall;
        r.p50 = rec.pct(0.50); r.p99 = rec.pct(0.99);
        r.note = note; emit(r);
    };
    row("read", m.read);
    row("insert", m.insert);
    if (m.scan.size()) {
        Result r; r.workload = wl + ".scan"; r.ops = m.scan.size(); r.seconds = wall;
        r.p50 = m.scan.pct(0.50); r.p99 = m.scan.pct(0.99);
        r.note = note + " rows/scans=" + std::to_string(m.scan_rows / m.scan.size());
        emit(r);
    }
    row("rmw", m.rmw);
}

} // namespace

int main(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::cerr << "missing value for " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if      (a == "--keys")       c.keys = (size_t)std::strtoull(next("--keys"), nullptr, 10);
        else if (a == "--valsize")    c.valsize = (size_t)std::strtoull(next("--valsize"), nullptr, 10);
        else if (a == "--threads")    c.threads = (unsigned)std::atoi(next("--threads"));
        else if (a == "--seed")       c.seed = std::strtoull(next("--seed"), nullptr, 0);
        else if (a == "--ops")        c.ops = (size_t)std::strtoull(next("--ops"), nullptr, 10);
        else if (a == "--dir")        c.dir = next("--dir");
        else if (a == "--engine")     c.engine = next("--engine");
        else if (a == "--workloads")  c.workloads = next("--workloads");
        else if (a == "--keep")       c.keep = true;
        else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
    }
    if (c.threads == 0) c.threads = 1;

    std::string err;
    auto e = make_engine(c, &err);
    if (!e) { std::cerr << "engine error: " << err << "\n"; return 2; }
    print_methodology(c, *e);

    std::vector<std::string> sel;
    size_t pos = 0;
    while (pos < c.workloads.size()) {
        size_t comma = c.workloads.find(',', pos);
        if (comma == std::string::npos) comma = c.workloads.size();
        std::string w = c.workloads.substr(pos, comma - pos);
        if (!w.empty()) sel.push_back(w);
        pos = comma + 1;
    }

    int rc = 0;
    for (const auto& w : sel) {
        if      (w == "fillseq")     { auto r = w_fill(*e, c, true);  emit(r); if (r.ops == 0) rc = 1; }
        else if (w == "fillrandom")  { auto r = w_fill(*e, c, false); emit(r); if (r.ops == 0) rc = 1; }
        else if (w == "readrandom")  { auto r = w_readrandom(*e, c);  emit(r); }
        else if (w == "overwrite")   { auto r = w_overwrite(*e, c);   emit(r); }
        else if (w == "ycsb_a")      w_ycsb(*e, c, 'a', 50, 50);
        else if (w == "ycsb_b")      w_ycsb(*e, c, 'b', 95, 5);
        else if (w == "ycsb_c")      w_ycsb(*e, c, 'c', 100, 0);
        else if (w == "ycsb_d")      w_ycsb_def(*e, c, 'd');
        else if (w == "ycsb_e")      w_ycsb_def(*e, c, 'e');
        else if (w == "ycsb_f")      w_ycsb_def(*e, c, 'f');
        else { std::cerr << "unknown workload: " << w << " (set: fillseq,fillrandom,readrandom,overwrite,ycsb_a..ycsb_f)\n"; rc = 2; }
    }
    e->close();
    if (!c.keep) { std::error_code ec; std::filesystem::remove_all(c.dir, ec); }
    return rc;
}
