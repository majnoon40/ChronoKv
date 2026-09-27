// bench/arena.cpp — ChronoKV benchmark arena (v29 M1, SKELETON).
//
// Roadmap anchor (docs/ROADMAP.md, v29 M1 step 1): a db_bench-style micro
// suite over the PUBLIC consumer API (this file builds hooks-OFF — the
// product's real face; no test hooks, no engine internals). The skeleton
// ships the engine-side workloads and the methodology block; the vendored
// baselines (step 3), YCSB-style mixes (step 2), and the CI ledger + noise
// protocol (step 4) land on top of this harness.
//
// Rule 9 compliance (measured claims only): every run prints a methodology
// header — kernel, CPUs, memory, CPU model, build flags, ChronoKV version,
// durability mode, key/value geometry, seed, and the wal_dir — BEFORE the
// results table, so no number can circulate without its hardware note.
// Two skeleton caveats are printed with every run and must survive into the
// ledger: (a) the io_uring backend is not observable from a hooks-off build
// (backend attribution lands with the CI integration); (b) the wal_dir's
// backing filesystem is not classified here.
//
// Each workload is SELF-CONTAINED: an untimed prep phase (noted in the
// output) followed by the timed phase — so any subset runs standalone.
//
// Output: TSV — workload  ops  seconds  ops_per_sec  p50_us  p99_us  rss_mb  note
// Percentiles are per-op where recorded (reads/overwrites/scans); fills
// report aggregate throughput with p50/p99 as "-" (per-op recording on the
// write path arrives with the CI ledger; the skeleton stays lean).
//
// Build:  make arena            (default -O2; small containers: make arena ARENA_FLAGS="-O1")
// Run:    ./build/arena/arena --keys 1000000 --valsize 100 --threads 4
//             --durability group --dir /tmp/ckv_arena
//             --workloads fillseq,fillrandom,readrandom,overwrite,rangescan,
//                         deletechurn,ckptload,coldrecovery,memory
#include "chronokv.hpp"

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
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef ARENA_BUILD_INFO
#define ARENA_BUILD_INFO "unknown (set ARENA_BUILD_INFO at build time)"
#endif

namespace {

using chronokv::Database;
using chronokv::Options;
using chronokv::Status;

struct Config {
    size_t   keys       = 1000000;
    size_t   valsize    = 100;
    unsigned threads    = 4;
    uint64_t seed       = 42;
    std::string dir       = "/tmp/ckv_arena";
    std::string durability = "group";
    std::string workloads =
        "fillseq,fillrandom,readrandom,overwrite,rangescan,deletechurn,"
        "ckptload,coldrecovery,memory";
    bool keep = false;
};

// ---------- small utilities ----------

std::string key16(uint64_t i) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llu", (unsigned long long)i);
    return std::string(b, 16);   // the soak spec's 16-byte keys
}

std::string make_value(std::mt19937_64& rng, size_t n) {
    std::string v(n, '\0');
    for (size_t i = 0; i < n; ++i)
        v[i] = char('a' + (rng() % 26));
    return v;
}

using Clock = std::chrono::steady_clock;
double elapsed_s(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

long proc_status_kb(const char* field) {   // "VmRSS" / "VmHWM"
    std::ifstream f("/proc/self/status");
    std::string line;
    const size_t flen = std::strlen(field);
    while (std::getline(f, line)) {
        if (line.compare(0, flen, field) == 0 && line.size() > flen && line[flen] == ':') {
            return std::strtol(line.c_str() + flen + 1, nullptr, 10);
        }
    }
    return -1;
}

std::string first_line_containing(const char* path, const char* needle) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return line;
    return "(unknown)";
}

// Per-op latency recorder (microseconds); one per thread, merged at the end.
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
        size_t idx = (size_t)(q * (double)(us_.size() - 1) + 0.5);
        return us_[idx];
    }
};

struct Result {
    std::string workload;
    uint64_t    ops = 0;
    double      seconds = 0.0;
    double      p50 = -1.0, p99 = -1.0;
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

Options base_options(const Config& c, const std::string& sub) {
    Options o;
    o.wal_dir         = c.dir + "/" + sub + "/wal";
    o.checkpoint_path = c.dir + "/" + sub + "/ckpt";
    o.auto_start_gc   = false;         // benchmarks measure the engine, not GC timing
    o.durability = c.durability == "sync"  ? chronokv::DurabilityMode::Sync
                 : c.durability == "async" ? chronokv::DurabilityMode::Async
                                           : chronokv::DurabilityMode::Group;
    return o;
}

void reset_dir(const std::string& p) {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p + "/wal", ec);
}

// Untimed prep: fill `n` keys; returns false on any non-OK put.
bool prep_fill(Database& db, const Config& c, size_t n, std::mt19937_64& rng,
               bool shuffle) {
    std::vector<uint64_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    if (shuffle)
        for (size_t i = n; i-- > 1;) {
            size_t j = (size_t)(rng() % (i + 1));
            std::swap(order[i], order[j]);
        }
    for (size_t i = 0; i < n; ++i)
        if (db.put(key16(order[i]), make_value(rng, c.valsize)) != Status::OK)
            return false;
    return true;
}

// ---------- workloads ----------

Result w_fill(const Config& c, bool sequential) {
    Result r; r.workload = sequential ? "fillseq" : "fillrandom";
    reset_dir(c.dir + "/" + r.workload);
    auto db = Database::open(base_options(c, r.workload));
    std::mt19937_64 rng(c.seed);
    const auto t0 = Clock::now();
    bool ok = prep_fill(db, c, c.keys, rng, /*shuffle=*/!sequential);
    r.seconds = elapsed_s(t0);
    r.ops = ok ? c.keys : 0;
    r.note = ok ? "single-writer; new-key path (nm_) isolation workload"
                : "ABORTED: non-OK put";
    db.close();
    return r;
}

Result w_readrandom(const Config& c) {
    Result r; r.workload = "readrandom";
    reset_dir(c.dir + "/readrandom");
    auto db = Database::open(base_options(c, "readrandom"));
    std::mt19937_64 rng(c.seed);
    if (!prep_fill(db, c, c.keys, rng, false)) { r.note = "prep failed"; return r; }
    db.checkpoint();     // reads served from a checkpointed state (recovery not re-measured per op)
    db.close();
    auto db2 = Database::open(base_options(c, "readrandom"));

    std::vector<Recorder> recs(c.threads);
    std::atomic<uint64_t> hits{0}, misses{0};
    const size_t per_thread = std::max<size_t>(1000, c.keys / 4 / c.threads);
    const auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < c.threads; ++t) ts.emplace_back([&, t] {
        std::mt19937_64 lrng(c.seed * 1000 + t);
        recs[t].reserve(per_thread);
        for (size_t i = 0; i < per_thread; ++i) {
            const uint64_t k = lrng() % c.keys;
            const auto s0 = Clock::now();
            auto v = db2.get(key16(k));
            recs[t].record(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
            if (v) hits.fetch_add(1, std::memory_order_relaxed);
            else   misses.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (auto& th : ts) th.join();
    r.seconds = elapsed_s(t0);
    Recorder merged;
    for (auto& rc : recs) merged.merge(std::move(rc));
    r.ops = merged.size();
    r.p50 = merged.pct(0.50);
    r.p99 = merged.pct(0.99);
    r.note = "uniform random; threads=" + std::to_string(c.threads) +
             " misses=" + std::to_string(misses.load()) +
             " (zipfian + true YCSB mixes land with M1 step 2)";
    db2.close();
    return r;
}

Result w_overwrite(const Config& c) {
    Result r; r.workload = "overwrite";
    reset_dir(c.dir + "/overwrite");
    auto db = Database::open(base_options(c, "overwrite"));
    std::mt19937_64 rng(c.seed);
    if (!prep_fill(db, c, c.keys, rng, false)) { r.note = "prep failed"; return r; }
    const size_t n = std::max<size_t>(1000, c.keys / 4);
    std::vector<Recorder> recs(c.threads);
    std::atomic<bool> failed{false};
    const auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < c.threads; ++t) ts.emplace_back([&, t] {
        std::mt19937_64 lrng(c.seed * 2000 + t);
        const size_t per = n / c.threads;
        recs[t].reserve(per);
        for (size_t i = 0; i < per; ++i) {
            const uint64_t k = lrng() % c.keys;
            const auto s0 = Clock::now();
            if (db.put(key16(k), make_value(lrng, c.valsize)) != Status::OK)
                failed.store(true);
            recs[t].record(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
        }
    });
    for (auto& th : ts) th.join();
    r.seconds = elapsed_s(t0);
    Recorder merged;
    for (auto& rc : recs) merged.merge(std::move(rc));
    r.ops = merged.size();
    r.p50 = merged.pct(0.50);
    r.p99 = merged.pct(0.99);
    r.note = std::string("existing-key update path (no nm_); threads=") +
             std::to_string(c.threads) + (failed.load() ? " FAILED-PUTS" : "");
    db.close();
    return r;
}

Result w_rangescan(const Config& c, size_t len, const char* name) {
    Result r; r.workload = name;
    reset_dir(c.dir + "/" + name);
    auto db = Database::open(base_options(c, name));
    std::mt19937_64 rng(c.seed);
    if (!prep_fill(db, c, c.keys, rng, false)) { r.note = "prep failed"; return r; }
    const size_t n = std::max<size_t>(100, c.keys / len / 4);
    Recorder rec; rec.reserve(n);
    size_t yielded = 0;
    const auto t0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        const uint64_t lo = rng() % c.keys;
        const uint64_t hi = std::min<uint64_t>(c.keys - 1, lo + len);
        const auto s0 = Clock::now();
        auto rows = db.range_scan(key16(lo), key16(hi));
        rec.record(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
        yielded += rows.size();
    }
    r.seconds = elapsed_s(t0);
    r.ops = n;
    r.p50 = rec.pct(0.50);
    r.p99 = rec.pct(0.99);
    r.note = "scan_len=" + std::to_string(len) + " rows/scans=" +
             std::to_string(n ? yielded / n : 0);
    db.close();
    return r;
}

Result w_deletechurn(const Config& c) {
    Result r; r.workload = "deletechurn";
    reset_dir(c.dir + "/deletechurn");
    auto db = Database::open(base_options(c, "deletechurn"));
    std::mt19937_64 rng(c.seed);
    if (!prep_fill(db, c, c.keys, rng, false)) { r.note = "prep failed"; return r; }
    const long rss0 = proc_status_kb("VmRSS");
    // 3 churn cycles: delete a 10% slice, reinsert the same keys with new
    // values. Pre-M3 (no page reclamation) this is the workload that
    // ratchets the pool — the arena's "before" column for P1.
    const size_t slice = c.keys / 10;
    uint64_t ops = 0;
    const auto t0 = Clock::now();
    for (int cycle = 0; cycle < 3; ++cycle) {
        const size_t base = (size_t)(rng() % std::max<size_t>(1, c.keys - slice));
        for (size_t i = 0; i < slice; ++i) {
            if (db.erase(key16(base + i)) != Status::OK) { r.note = "erase failed"; db.close(); return r; }
            ++ops;
        }
        for (size_t i = 0; i < slice; ++i) {
            if (db.put(key16(base + i), make_value(rng, c.valsize)) != Status::OK) {
                r.note = "reinsert failed"; db.close(); return r;
            }
            ++ops;
        }
    }
    r.seconds = elapsed_s(t0);
    r.ops = ops;
    const long rss1 = proc_status_kb("VmRSS");
    r.note = "3x delete+reinsert of a 10% slice; rss_delta_mb=" +
             std::to_string((rss1 - rss0) / 1024) +
             " (monotonic pre-M3; P1's before-column)";
    db.close();
    return r;
}

Result w_ckptload(const Config& c) {
    Result r; r.workload = "ckptload";
    reset_dir(c.dir + "/ckptload");
    auto db = Database::open(base_options(c, "ckptload"));
    std::mt19937_64 rng(c.seed);
    const size_t n = std::max<size_t>(10000, c.keys / 10);
    if (!prep_fill(db, c, n, rng, false)) { r.note = "prep failed"; return r; }
    // Writers hammer NEW keys (past the prep range) while we checkpoint.
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> wops{0};
    std::vector<std::thread> ws;
    for (unsigned t = 0; t < c.threads; ++t) ws.emplace_back([&, t] {
        std::mt19937_64 lrng(c.seed * 3000 + t);
        uint64_t k = n + 1 + t * 1000000;
        while (!stop.load(std::memory_order_relaxed)) {
            if (db.put(key16(k++), make_value(lrng, c.valsize)) != Status::OK) break;
            wops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uint64_t w0 = wops.load();
    const auto t0 = Clock::now();
    bool ok = true;
    try { db.checkpoint(); } catch (const std::exception& e) { ok = false; r.note = e.what(); }
    r.seconds = elapsed_s(t0);
    stop.store(true);
    for (auto& th : ws) th.join();
    r.ops = 1;   // the checkpoint itself
    r.note = (ok ? "checkpoint wall-time under load" : std::string("checkpoint THREW: ") + r.note) +
             "; writer_ops_during=" + std::to_string(wops.load() - w0);
    db.close();
    return r;
}

Result w_coldrecovery(const Config& c) {
    Result r; r.workload = "coldrecovery";
    reset_dir(c.dir + "/coldrecovery");
    {   // untimed: build the database to recover from
        auto db = Database::open(base_options(c, "coldrecovery"));
        std::mt19937_64 rng(c.seed);
        if (!prep_fill(db, c, c.keys, rng, false)) { r.note = "prep failed"; return r; }
        db.close();   // NO checkpoint: recovery replays the full WAL window
    }
    const auto t0 = Clock::now();
    auto db = Database::open(base_options(c, "coldrecovery"));
    r.seconds = elapsed_s(t0);
    r.ops = c.keys;
    const bool sane = db.get(key16(c.keys / 2)).has_value();
    r.note = "WAL-only recovery (no checkpoint); records/s is the v30-M1 "
             "bulk-load-prerequisite anchor";
    if (!sane) r.note += " SANITY-FAIL";
    db.close();
    return r;
}

Result w_memory(const Config& c) {
    Result r; r.workload = "memory";
    reset_dir(c.dir + "/memory");
    auto db = Database::open(base_options(c, "memory"));
    std::mt19937_64 rng(c.seed);
    if (!prep_fill(db, c, c.keys, rng, false)) { r.note = "prep failed"; return r; }
    r.ops = c.keys;
    r.seconds = 0;
    const long rss = proc_status_kb("VmRSS");
    const long hwm = proc_status_kb("VmHWM");
    // RSS-level, per the roadmap's B2 discipline: this counts EVERYTHING
    // (tree pool + version heap + reader slack), not pool-only.
    r.note = "rss_mb=" + std::to_string(rss / 1024) +
             " hwm_mb=" + std::to_string(hwm / 1024) +
             " bytes_per_key=" + std::to_string(c.keys ? (size_t)rss * 1024 / c.keys : 0) +
             " (RSS-level: tree pool + version heap + slack)";
    db.close();
    return r;
}

void print_methodology(const Config& c) {
    struct utsname u{};
    uname(&u);
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    std::cout << "# chronokv-arena (v29 M1 skeleton) — methodology (rule 9)\n"
              << "# chronokv_version=" << chronokv::CHRONOKV_VERSION << "\n"
              << "# build=" << ARENA_BUILD_INFO << "\n"
              << "# kernel=" << u.sysname << " " << u.release << " (" << u.machine << ")\n"
              << "# cpus=" << cpus << "\n"
              << "# " << first_line_containing("/proc/meminfo", "MemTotal") << "\n"
              << "# " << first_line_containing("/proc/cpuinfo", "model name") << "\n"
              << "# wal_dir=" << c.dir << " (backing fs not classified in the skeleton)\n"
              << "# durability=" << c.durability << " threads=" << c.threads
              << " keys=" << c.keys << " valsize=" << c.valsize << " seed=" << c.seed << "\n"
              << "# caveat: io_uring backend not observable from a hooks-off build"
                 " (attribution lands with M1 step 4)\n"
              << "# columns: workload\tops\tseconds\tops_per_sec\tp50_us\tp99_us\trss_mb\tnote\n";
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
        else if (a == "--dir")        c.dir = next("--dir");
        else if (a == "--durability") c.durability = next("--durability");
        else if (a == "--workloads")  c.workloads = next("--workloads");
        else if (a == "--keep")       c.keep = true;
        else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
    }
    if (c.threads == 0) c.threads = 1;

    print_methodology(c);

    std::vector<std::string> sel;
    {
        size_t pos = 0;
        while (pos < c.workloads.size()) {
            size_t comma = c.workloads.find(',', pos);
            if (comma == std::string::npos) comma = c.workloads.size();
            std::string w = c.workloads.substr(pos, comma - pos);
            if (!w.empty()) sel.push_back(w);
            pos = comma + 1;
        }
    }

    int rc = 0;
    for (const auto& w : sel) {
        Result r;
        if      (w == "fillseq")      r = w_fill(c, true);
        else if (w == "fillrandom")   r = w_fill(c, false);
        else if (w == "readrandom")   r = w_readrandom(c);
        else if (w == "overwrite")    r = w_overwrite(c);
        else if (w == "rangescan")  { emit(w_rangescan(c, 100, "rangescan100"));
                                      r = w_rangescan(c, 10000, "rangescan10k"); }
        else if (w == "deletechurn")  r = w_deletechurn(c);
        else if (w == "ckptload")     r = w_ckptload(c);
        else if (w == "coldrecovery") r = w_coldrecovery(c);
        else if (w == "memory")       r = w_memory(c);
        else { std::cerr << "unknown workload: " << w << "\n"; rc = 2; continue; }
        emit(r);
        if (r.note.find("ABORTED") != std::string::npos ||
            r.note.find("SANITY-FAIL") != std::string::npos ||
            r.note.find("FAILED") != std::string::npos ||
            r.note.find("THREW") != std::string::npos)
            rc = 1;
    }

    if (!c.keep) { std::error_code ec; std::filesystem::remove_all(c.dir, ec); }
    return rc;
}
