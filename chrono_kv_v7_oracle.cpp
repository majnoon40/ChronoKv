// chrono_kv_v7_oracle.cpp — v7 + missed-read oracle (slice 2)
//
// Tests the LOGIC half that TSan/ASan cannot see: does every read return the
// newest version its snapshot entitles it to?
//
// Oracle design (avoids the classic re-sampling false positive documented in
// the v5 header): an independent ground-truth log records every committed
// (ts, value), appended BEFORE the completion flag is set — so any ts a reader
// can observe via published_ is already in the log. Each read snapshots its
// expected answer from that log at the same instant it fixes read_ts, then
// compares against what the chain walk actually returns. A mismatch means a
// needed version was truncated or the chain is corrupt: a real violation.
//
// Build TWO binaries:
//   g++ -std=c++17 -O0 -g -DWIDEN -pthread chrono_kv_v7_oracle.cpp -o v7_oracle
//       -> the CORRECT v7. Expect: oracle violations: 0
//   g++ -std=c++17 -O0 -g -DWIDEN -DBROKEN_GC -pthread chrono_kv_v7_oracle.cpp -o v7_oracle_broken
//       -> POSITIVE CONTROL: GC threshold drops the published_ bound (the v3
//          defect). The oracle MUST fire (violations > 0). If it doesn't, the
//          instrument or the widen hook is wrong — do not trust a clean v7.
//
// STATUS: trace-level. The oracle design is derived, not yet run. Run the
// control FIRST.
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <string>
#include <optional>
#include <list>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <iostream>
using namespace std;

constexpr size_t MAX_KEYS = 64;
constexpr size_t RING     = 1024;   // must exceed max in-flight reservations

#ifdef WIDEN
#define WIDEN_YIELD() this_thread::yield()
#else
#define WIDEN_YIELD() ((void)0)
#endif

struct Version {
    atomic<uint64_t> commit_ts;
    string           value;
    atomic<Version*> prev;
    Version(uint64_t ts, string v, Version* p)
    : commit_ts(ts), value(move(v)), prev(p) {}
};

// ---------- oracle ground truth (test-only, separate from the KV state) ----------
static mutex oracle_mu;
static array<vector<pair<uint64_t,string>>, MAX_KEYS> oracle_log;  // per-key, ts-ascending
static atomic<long> oracle_violations{0};
static mutex viol_mu;
static vector<string> viol_samples;   // first few violations, for inspection

class ChronoKV {
    mutable shared_mutex              nm_;
    unordered_map<string,int>         name2idx_;
    array<atomic<Version*>,MAX_KEYS>  heads_;
    array<mutex,MAX_KEYS>            commit_mu_;   // per-key link serialization
    atomic<uint64_t> clock_{1};                    // lock-free reservation counter
    array<atomic<uint64_t>,RING>     done_;        // done_[ts%RING] = ts (the tag)
    atomic<uint64_t> published_{0};                // contiguous tagged frontier
    mutable mutex    rm_;
    list<uint64_t>   reg_;
    mutex            gc_mu_;
    condition_variable gc_cv_;
    atomic<bool>     dirty_{false};
    atomic<bool>     running_{false};
    thread           gc_;

    int find_index(const string& k) const {
        shared_lock lk(nm_);
        auto it = name2idx_.find(k);
        return it == name2idx_.end() ? -1 : it->second;
    }
    int ensure_index(const string& k) {
        unique_lock lk(nm_);
        auto it = name2idx_.find(k);
        if (it != name2idx_.end()) return it->second;
        int idx = (int)name2idx_.size();
        if (idx >= (int)MAX_KEYS) throw runtime_error("ChronoKV: MAX_KEYS exceeded");
        name2idx_[k] = idx;
        return idx;
    }
    struct ReaderUnregister {
        ChronoKV* self; list<uint64_t>::iterator it;
        ~ReaderUnregister() {
            lock_guard<mutex> lk(self->rm_);
            self->reg_.erase(it);
            self->wake_gc();
        }
    };
    void wake_gc() { dirty_.store(true, memory_order_release); gc_cv_.notify_one(); }

    uint64_t gc_threshold() const {
        lock_guard<mutex> lk(rm_);
        #ifdef BROKEN_GC
        // [POSITIVE CONTROL] reg_min only — the v3 defect. GC ignores published_,
        // so in the link-but-not-yet-published gap it can anchor on a brand-new
        // node and truncate versions a lagging reader still needs. The oracle
        // must catch this.
        if (reg_.empty()) return UINT64_MAX;
        uint64_t m = UINT64_MAX;
        for (uint64_t r : reg_) m = min(m, r);
        return m;
        #else
        // [correct] bounded by BOTH registered readers and the published_ watermark,
        // fused under rm_ (the v5-verified structure).
        uint64_t p = published_.load(memory_order_acquire);
        if (reg_.empty()) return p;
        uint64_t m = p;
        for (uint64_t r : reg_) m = min(m, r);
        return m;
        #endif
    }

    void advance_frontier() {
        uint64_t cur = published_.load(memory_order_acquire);
        for (;;) {
            uint64_t nxt = cur + 1;
            if (done_[nxt % RING].load(memory_order_acquire) != nxt) break;
            if (published_.compare_exchange_weak(cur, nxt,
                memory_order_acq_rel, memory_order_acquire)) {
                cur = nxt;   // tagged, never cleared
                }
        }
    }

    void gc_once() {
        uint64_t m = gc_threshold();
        size_t n; { shared_lock lk(nm_); n = name2idx_.size(); }
        for (size_t i = 0; i < n; ++i) {
            Version* cur = heads_[i].load(memory_order_acquire);
            bool anchor = false;
            while (cur) {
                Version* nxt = cur->prev.load(memory_order_acquire);
                uint64_t c = cur->commit_ts.load(memory_order_acquire);
                if (c == 0) { cur = nxt; continue; }
                if (!anchor && c <= m) {
                    anchor = true;
                    cur->prev.store(nullptr, memory_order_release);
                    cur = nxt; continue;
                }
                if (anchor) delete cur;
                cur = nxt;
            }
        }
    }

    void free_all() {
        for (size_t i = 0; i < MAX_KEYS; ++i) {
            Version* cur = heads_[i].load(memory_order_relaxed);
            while (cur) { Version* nxt = cur->prev.load(memory_order_relaxed); delete cur; cur = nxt; }
            heads_[i].store(nullptr, memory_order_relaxed);
        }
    }

public:
    ChronoKV() {
        for (auto& h : heads_) h.store(nullptr, memory_order_relaxed);
        for (auto& d : done_)  d.store(0, memory_order_relaxed);
    }
    ~ChronoKV() {
        { lock_guard<mutex> lk(gc_mu_); running_.store(false, memory_order_release); }
        gc_cv_.notify_all();
        if (gc_.joinable()) gc_.join();
        free_all();
    }
    void start_gc() {
        running_.store(true, memory_order_release);
        gc_ = thread([this]{
            unique_lock<mutex> lk(gc_mu_);
            while (running_.load(memory_order_acquire)) {
                gc_cv_.wait(lk, [this]{ return !running_.load(memory_order_acquire)
                    || dirty_.load(memory_order_acquire); });
                if (!running_.load(memory_order_acquire)) break;
                dirty_.store(false, memory_order_release);
                lk.unlock(); gc_once(); lk.lock();
            }
        });
    }

    void commit(const string& key, const string& value) {
        int idx = ensure_index(key);
        uint64_t ts;
        {
            lock_guard<mutex> lk(commit_mu_[idx]);
            ts = clock_.fetch_add(1, memory_order_relaxed);
            WIDEN_YIELD();                       // widen the reserved-but-not-linked window
            {
                // ORACLE: record ground truth BEFORE the completion flag is set, inside
                // the per-key lock so each key's log stays ts-ascending. Ordering:
                //   log append  ->  link  ->  done_[ts]  ->  frontier may reach ts
                // so published_ >= ts implies ts is already in the log.
                lock_guard<mutex> olk(oracle_mu);
                oracle_log[idx].push_back({ts, value});
            }
            Version* n = new Version(ts, value, heads_[idx].load(memory_order_relaxed));
            heads_[idx].store(n, memory_order_release);
        }
        done_[ts % RING].store(ts, memory_order_release);
        advance_frontier();
        wake_gc();
    }

    optional<string> read(const string& key) {
        uint64_t read_ts; list<uint64_t>::iterator it;
        {
            lock_guard<mutex> lk(rm_);           // fuse sample + register
            read_ts = published_.load(memory_order_acquire);
            it = reg_.insert(reg_.end(), read_ts);
        }
        ReaderUnregister guard{this, it};
        int idx = find_index(key);

        // ORACLE: snapshot the expected answer at the same instant read_ts is fixed.
        // Every ts <= read_ts is in the log (ordering above), so this is exactly the
        // newest version this snapshot is entitled to.
        optional<string> expected;
        if (idx >= 0) {
            lock_guard<mutex> olk(oracle_mu);
            auto& log = oracle_log[idx];
            for (auto rit = log.rbegin(); rit != log.rend(); ++rit) {
                if (rit->first <= read_ts) { expected = rit->second; break; }
            }
        }

        // The real read path, unchanged.
        optional<string> result;
        if (idx >= 0) {
            Version* cur = heads_[idx].load(memory_order_acquire);
            while (cur) {
                uint64_t c = cur->commit_ts.load(memory_order_acquire);
                if (c != 0 && c <= read_ts) { result = cur->value; break; }
                cur = cur->prev.load(memory_order_acquire);
            }
        }

        if (result != expected) {
            oracle_violations.fetch_add(1);
            lock_guard<mutex> vlk(viol_mu);
            if (viol_samples.size() < 5) {
                viol_samples.push_back(
                    "key=" + key + " read_ts=" + to_string(read_ts) +
                    " expected=" + (expected ? *expected : string("<none>")) +
                    " got=" + (result ? *result : string("<none>")));
            }
        }
        return result;
    }
};

int main() {
    ChronoKV kv;
    kv.start_gc();
    kv.commit("a", "1");
    kv.commit("a", "2");
    cout << "sequential read(a) = " << (kv.read("a") ? *kv.read("a") : string("null")) << "\n";

    constexpr int W = 4, R = 4, N = 3000;   // smaller N keeps the oracle scan fast
    atomic<bool> go{false};
    vector<thread> ts;
    for (int w = 0; w < W; ++w)
        ts.emplace_back([&kv,&go,w]{
            while (!go.load(memory_order_acquire)) this_thread::yield();
            for (int i = 0; i < N; ++i) {
                kv.commit("a", "w" + to_string(w) + ":" + to_string(i));
                kv.commit("b", "w" + to_string(w) + ":" + to_string(i));
            }
        });
    for (int r = 0; r < R; ++r)
        ts.emplace_back([&kv,&go]{
            while (!go.load(memory_order_acquire)) this_thread::yield();
            for (int i = 0; i < N; ++i) { (void)kv.read("a"); (void)kv.read("b"); }
        });
    go.store(true, memory_order_release);
    for (auto& t : ts) t.join();

    long v = oracle_violations.load();
    {
        lock_guard<mutex> vlk(viol_mu);
        for (auto& s : viol_samples) cout << "  VIOLATION: " << s << "\n";
    }
    cout << "oracle violations: " << v << "\n";
    #ifdef BROKEN_GC
    cout << (v > 0 ? "CONTROL FIRED (expected) — instrument is not vacuous\n"
    : "CONTROL DID NOT FIRE — instrument/widen broken; do NOT trust a clean v7\n");
    #else
    cout << (v == 0 ? "v7 LOGIC CLEAN (reads returned their entitled version every time)\n"
    : "v7 LOGIC VIOLATIONS — missed reads detected\n");
    #endif
    return 0;
}
