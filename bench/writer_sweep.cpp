// bench/writer_sweep.cpp — ChronoKV WAL committer-count sweep (v29 M5
// slice 3, "the measurement"; decision doc 2026-10-20 §8 slice 3).
//
// Roadmap anchor (docs/ROADMAP.md, v29 M5 acceptance): "a measurable win
// at 8+ committers" needs an 8/16/32-committer concurrency sweep —
// recorded as an ARENA-ADJACENT ONE-OFF with a rule-9 methodology header,
// not as a nightly ledger series (the ledger's geometry is pinned at 4
// threads; a concurrency axis would be a ledger-format change). This
// harness drives exactly one workload shape — concurrent commits over a
// pre-filled key space, zipfian-distributed, no reads — so the sweep
// isolates the WAL commit path the writer thread owns. The nightly arena
// remains the cross-engine vehicle; this is the engine-side concurrency
// instrument.
//
// The A/B it exists for: build twice (make sweep; make sweep-legacy) and
// run both legs on the SAME machine — the difference at each committer
// count is the writer-thread handoff's measured cost/benefit against the
// legacy leader path, which is both the regression diagnosis (nightly
// runs #121..#144, arena gate red since the slice-1 landing) and the
// acceptance leg ("no regression at any concurrency, a measurable win at
// 8+").
//
// --probe io (no committers): the raw primitive split on this filesystem —
// N pwrites and N fdatasyncs of a WAL-sized record against a fresh file in
// the wal_dir's directory, p50/p99 each. This is the depth-2 pipelining
// decision's measured basis (decision doc §8 slice 3: "measured and decided
// (ship or record-refuse)"): depth-2 can remove at most the WRITE half of
// the per-drain critical path, so write_p50 / (write_p50 + fdatasync_p50)
// is the mechanism's upper bound. It also anchors the handoff-latency
// story (the fix's value scales with fdatasync's share of the commit loop).
//
// Rule 9 compliance: same header discipline as bench/arena.cpp — kernel,
// CPUs, memory, CPU model, build flags, ChronoKV version, WAL leg
// (writer-thread vs legacy-leader compile path), durability, geometry,
// seed, and the wal_dir, BEFORE the results. The wal_stats diagnostics
// row (drains, records/drain, hot-idle spins/hits, lingers, queue
// high-water) rides every cell so latency numbers are attributable, per
// the M5 slice-2 diagnostics contract.
//
// Output: TSV —
//   label  ops  seconds  ops_per_sec  p50_us  p99_us  rss_mb  note
// followed by a `# diag` comment line with the wal_stats snapshot.
//
// Build:  make sweep            (default -O2; small containers: ARENA_FLAGS="-O1")
//         make sweep-legacy     (CKV_WAL_LEGACY_LEADER — the lifeboat leg)
// Run:    ./build/sweep/sweep --committers 8 --ops 20000 --keys 200000
//             --durability group --dir /tmp/ckv_sweep --label c8
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
#include <unistd.h>
#include <fcntl.h>

#ifndef ARENA_BUILD_INFO
#define ARENA_BUILD_INFO "unknown (set ARENA_BUILD_INFO at build time)"
#endif

namespace {

using chronokv::Database;
using chronokv::Options;
using chronokv::Status;

struct Config {
    unsigned committers = 4;
    size_t   ops        = 20000;      // per committer
    size_t   keys       = 200000;     // pre-filled key space (zipfian over it)
    size_t   valsize    = 100;
    uint64_t seed       = 42;
    std::string dir       = "/tmp/ckv_sweep";
    std::string durability = "group";
    std::string label     = "sweep";
    bool     io_probe  = false;      // --probe io: raw pwrite/fdatasync split
    // Knob overrides (negative = keep the Options default; the point is
    // sensitivity cells for the same binary, never a silent default change).
    long hot_idle_us = -1;
    long linger_us   = -1;
};

std::string key16(uint64_t i) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llu", (unsigned long long)i);
    return std::string(b, 16);
}

std::string make_value(std::mt19937_64& rng, size_t n) {
    std::string v(n, '\0');
    for (size_t i = 0; i < n; ++i)
        v[i] = char('a' + (rng() % 26));
    return v;
}

using Clock = std::chrono::steady_clock;

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

// zipfian(theta=0.99) over [0,n): the arena's precomputed-CDF variant,
// verbatim semantics (same shape argument, same fairness-within-harness
// rule; see bench/arena.cpp's fuller note).
class Zipfian {
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

std::string first_line_containing(const char* path, const char* needle) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return line;
    return "(unknown)";
}

long proc_status_kb(const char* field) {
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

void print_methodology(const Config& c) {
    struct utsname u{};
    uname(&u);
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
#ifdef CKV_WAL_LEGACY_LEADER
    const char* wal_leg = "legacy_leader (CKV_WAL_LEGACY_LEADER)";
#else
    const char* wal_leg = "writer_thread (default)";
#endif
    std::cout << "# chronokv-writer-sweep (v29 M5 slice 3) — methodology (rule 9)\n"
              << "# chronokv_version=" << chronokv::CHRONOKV_VERSION << "\n"
              << "# build=" << ARENA_BUILD_INFO << "\n"
              << "# wal_leg=" << wal_leg << "\n"
              << "# kernel=" << u.sysname << " " << u.release << " (" << u.machine << ")\n"
              << "# cpus=" << cpus << "\n"
              << "# " << first_line_containing("/proc/meminfo", "MemTotal") << "\n"
              << "# " << first_line_containing("/proc/cpuinfo", "model name") << "\n"
              << "# wal_dir=" << c.dir << " (backing fs not classified)\n"
              << "# durability=" << c.durability << " committers=" << c.committers
              << " ops_per_committer=" << c.ops << " keys=" << c.keys
              << " valsize=" << c.valsize << " seed=" << c.seed << "\n"
              << "# knobs: hot_idle_us=" << (c.hot_idle_us < 0 ? "default" : std::to_string(c.hot_idle_us))
              << " linger_us=" << (c.linger_us < 0 ? "default" : std::to_string(c.linger_us))
              << " sqpoll=" << (getenv("CKV_WAL_SQPOLL") && getenv("CKV_WAL_SQPOLL")[0] == '1' ? "1" : "0") << "\n"
              << "# caveat: io_uring rungs not observable from a hooks-off build\n"
              << "# columns: label\tops\tseconds\tops_per_sec\tp50_us\tp99_us\trss_mb\tnote\n";
}

// --probe io: see the file header. Times raw pwrite and fdatasync on a
// fresh file in the wal_dir's directory — no engine, no committers.
int probe_io(const Config& c, unsigned n) {
    print_methodology(c);
    const std::string path = c.dir + "/io_probe";
    std::error_code ec;
    std::filesystem::remove_all(c.dir, ec);
    std::filesystem::create_directories(c.dir, ec);
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) { std::cerr << "probe: open failed: " << strerror(errno) << "\n"; return 2; }
    const size_t rec = c.valsize + 64;   // a WAL frame's rough size class
    std::string buf(rec, 'p');
    std::vector<double> w_us, f_us;
    w_us.reserve(n); f_us.reserve(n);
    off_t off = 0;
    for (unsigned i = 0; i < n; ++i) {
        const auto t0 = Clock::now();
        ssize_t w = ::pwrite(fd, buf.data(), buf.size(), off);
        const auto t1 = Clock::now();
        if (w != (ssize_t)buf.size()) { std::cerr << "probe: short write\n"; ::close(fd); return 2; }
        w_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        const auto t2 = Clock::now();
        if (::fdatasync(fd) != 0) { std::cerr << "probe: fdatasync failed\n"; ::close(fd); return 2; }
        f_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t2).count());
        off += (off_t)buf.size();
    }
    ::close(fd);
    std::filesystem::remove_all(path, ec);
    auto qtile = [](std::vector<double>& v, double q) {
        std::sort(v.begin(), v.end());
        return v[(size_t)(q * (double)(v.size() - 1) + 0.5)];
    };
    const double w50 = qtile(w_us, 0.50), f50 = qtile(f_us, 0.50);
    const double w99 = qtile(w_us, 0.99), f99 = qtile(f_us, 0.99);
    const double bound = (w50 + f50) > 0 ? 100.0 * w50 / (w50 + f50) : 0.0;
    std::cout << "io_probe\t" << n << "\t-\t-\t"
              << (long long)w50 << "\t" << (long long)f50 << "\t-\t"
              << "write_p99_us=" << (long long)w99
              << " fdatasync_p99_us=" << (long long)f99
              << " depth2_upper_bound_pct=" << (long long)(bound + 0.5)
              << " rec_bytes=" << rec << "\n";
    return 0;
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
        if      (a == "--committers") c.committers = (unsigned)std::atoi(next("--committers"));
        else if (a == "--ops")        c.ops = (size_t)std::strtoull(next("--ops"), nullptr, 10);
        else if (a == "--keys")       c.keys = (size_t)std::strtoull(next("--keys"), nullptr, 10);
        else if (a == "--valsize")    c.valsize = (size_t)std::strtoull(next("--valsize"), nullptr, 10);
        else if (a == "--seed")       c.seed = std::strtoull(next("--seed"), nullptr, 0);
        else if (a == "--dir")        c.dir = next("--dir");
        else if (a == "--durability") c.durability = next("--durability");
        else if (a == "--label")      c.label = next("--label");
        else if (a == "--hot-idle-us") c.hot_idle_us = std::atol(next("--hot-idle-us"));
        else if (a == "--linger-us")   c.linger_us = std::atol(next("--linger-us"));
        else if (a == "--probe") { c.io_probe = (std::string(next("--probe")) == "io"); }
        else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
    }
    if (c.committers == 0) c.committers = 1;

    if (c.io_probe) return probe_io(c, 2000);

    // Untimed prep: fresh dir, pre-fill the key space.
    {
        std::error_code ec;
        std::filesystem::remove_all(c.dir, ec);
        std::filesystem::create_directories(c.dir + "/wal", ec);
    }
    Options o;
    o.wal_dir         = c.dir + "/wal";
    o.checkpoint_path = c.dir + "/ckpt";
    o.auto_start_gc   = false;
    o.durability = c.durability == "sync"  ? chronokv::DurabilityMode::Sync
                 : c.durability == "async" ? chronokv::DurabilityMode::Async
                                           : chronokv::DurabilityMode::Group;
    if (c.hot_idle_us >= 0) o.wal_writer_hot_idle_us = (uint32_t)c.hot_idle_us;
    if (c.linger_us >= 0)   o.wal_linger_cap_us = (uint32_t)c.linger_us;

    auto db = Database::open(o);
    {
        std::mt19937_64 rng(c.seed);
        bool ok = true;
        for (size_t i = 0; i < c.keys; ++i)
            if (db.put(key16(i), make_value(rng, c.valsize)) != Status::OK) { ok = false; break; }
        if (!ok) {
            std::cout << c.label << "\t0\t0.000\t0\t-\t-\t0\tABORTED: non-OK put in prep\n";
            return 1;
        }
    }

    // Timed phase: N committers, each `ops` overwrites over the shared
    // zipfian key space — concurrent commits, no reads, one WAL owner.
    // The wal_stats snapshot is taken BEFORE the timed phase so the diag
    // row attributes the measurement window, not the untimed prep.
    const auto st0 = db.wal_stats();
    const Zipfian zipf(c.keys);
    std::vector<Recorder> recs(c.committers);
    std::atomic<uint64_t> ok_puts{0}, failed_puts{0};
    const auto t0 = Clock::now();
    std::vector<std::thread> ths;
    for (unsigned t = 0; t < c.committers; ++t)
        ths.emplace_back([&, t] {
            std::mt19937_64 lrng(c.seed * 1000 + t);
            recs[t].reserve(c.ops);
            for (size_t i = 0; i < c.ops; ++i) {
                const uint64_t k = zipf.next(lrng);
                const auto s0 = Clock::now();
                if (db.put(key16(k), make_value(lrng, c.valsize)) == Status::OK)
                    ok_puts.fetch_add(1, std::memory_order_relaxed);
                else
                    failed_puts.fetch_add(1, std::memory_order_relaxed);
                recs[t].record(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
            }
        });
    for (auto& th : ths) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    Recorder merged;
    for (auto& r : recs) merged.merge(std::move(r));
    const uint64_t total = ok_puts.load() + failed_puts.load();
    const auto st_all = db.wal_stats();
    // Timed-phase deltas only (prep excluded).
    const uint64_t d_drains  = st_all.drains > st0.drains ? st_all.drains - st0.drains : 0;
    const uint64_t d_records = st_all.records > st0.records ? st_all.records - st0.records : 0;
    const uint64_t d_spins   = st_all.hot_spins_taken > st0.hot_spins_taken ? st_all.hot_spins_taken - st0.hot_spins_taken : 0;
    const uint64_t d_hits    = st_all.hot_spin_hits > st0.hot_spin_hits ? st_all.hot_spin_hits - st0.hot_spin_hits : 0;
    const uint64_t d_spin_us = st_all.hot_spin_us_total > st0.hot_spin_us_total ? st_all.hot_spin_us_total - st0.hot_spin_us_total : 0;
    const uint64_t d_fsyncs  = st_all.fsyncs > st0.fsyncs ? st_all.fsyncs - st0.fsyncs : 0;
    const auto st = st_all;
    const long rss = proc_status_kb("VmRSS");
    const double rpd = d_drains ? (double)d_records / (double)d_drains : 0.0;
    const std::string p50s = merged.size() ? std::to_string((long long)merged.pct(0.50)) : "-";
    const std::string p99s = merged.size() ? std::to_string((long long)merged.pct(0.99)) : "-";
    char line[512];
    std::snprintf(line, sizeof line, "%s\t%llu\t%.3f\t%.0f\t%s\t%s\t%.1f\t%s",
        c.label.c_str(), (unsigned long long)total, secs,
        secs > 0 ? (double)total / secs : 0.0,
        p50s.c_str(), p99s.c_str(), rss / 1024.0,
        failed_puts.load() ? "FAILED-PUTS" : "ok");
    std::cout << line << "\n";
    std::cout << "# diag: drains=" << d_drains
              << " records=" << d_records
              << " rec_per_drain=" << (rpd > 0 ? std::to_string(rpd).substr(0, 5) : "0")
              << " spins=" << d_spins
              << " hits=" << d_hits
              << " spin_us=" << d_spin_us
              << " lingers=" << st.lingers_taken
              << " qhw=" << st.queue_high_water
              << " writer_deaths=" << st.writer_deaths
              << " fsyncs=" << d_fsyncs << "\n";
    db.close();
    return failed_puts.load() ? 1 : 0;
}
