// chrono_kv_v8.cpp — lock-free reader registration (removes rm_ from the read path).
//
// v7 fused "sample published_ + register read_ts" under a global mutex rm_,
// serializing all readers. v8 replaces that with a per-reader atomic slot and a
// seqlock-style double-check:
//
//     retry:
//       r  = load(published)        (1) sample        [seq_cst]
//       store(slot, r+1)            (2) register      [release]
//       r' = load(published)        (3) double-check  [seq_cst]
//       if r' != r: goto retry      (4) stale -> re-sample
//       walk chain for newest <= r
//       store(slot, 0)              (5) unregister    [release]
//
// GC scans the slots: reg_min = min(slot-1 over nonzero slots); m = min(reg_min,
// published_); anchor at newest <= m; truncate below. (r+1 encoding keeps 0 = "inactive".)
//
// Why it's safe (full argument in the reclamation proof):
//  - Double-check lemma: if a reader commits to r (step 4 passes), published_ was r
//    throughout steps (1)-(3) — it's monotonic, so it can't advance and return. Any GC
//    in that window used m <= published_ = r.
//  - During the active window GC reads slot = r+1 (release/acquire), so reg_min <= r,
//    hence m <= r. Either way m <= r, so the needed version (newest <= r) is the anchor
//    or newer and is never truncated.
//
// Memory ordering (what the TLA+ model abstracts and the proof assumes):
//  - published_: seq_cst on both reader loads and the frontier CAS -> a total order, so
//    the double-check observes any concurrent Publish.            (assumption 5)
//  - slot: release store / acquire load -> GC sees registrations.  (assumption 4)
//  A relaxed build is NOT guaranteed correct; a bounded genmc check is the remaining step.
//
// STATUS: implements ChronoKVLockFree.tla (UseCheck=TRUE clean over 131 states;
// UseCheck=FALSE yields the gap-bug counterexample). Sanitizer/oracle run is next.
// Limitation: not re-entrant — a thread must not call read() recursively (the harness doesn't).
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <string>
#include <optional>
#include <array>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <vector>
#include <stdexcept>
using namespace std;

constexpr size_t MAX_KEYS = 64;
constexpr size_t RING = 1024;
constexpr size_t MAX_READERS = 128;

#ifdef WIDEN
#define WIDEN_YIELD() this_thread::yield()
#else
#define WIDEN_YIELD() ((void)0)
#endif

struct Version {
    atomic<uint64_t> commit_ts;
    string value;
    atomic<Version*> prev;
    Version(uint64_t ts, string v, Version* p) : commit_ts(ts), value(move(v)), prev(p) {}
};

class ChronoKV {
    mutable shared_mutex nm_;
    unordered_map<string,int> name2idx_;
    array<atomic<Version*>, MAX_KEYS> heads_;
    array<mutex, MAX_KEYS> commit_mu_;            // per-key link serialization
    atomic<uint64_t> clock_{1};                   // lock-free reservation counter
    array<atomic<uint64_t>, RING> done_;          // tagged completion ring
    atomic<uint64_t> published_{0};               // reader-visible watermark
    array<atomic<uint64_t>, MAX_READERS> slots_;  // per-reader registration (0=inactive, else snapshot+1)
    atomic<unsigned> next_slot_{0};
    mutex gc_mu_;
    condition_variable gc_cv_;
    atomic<bool> dirty_{false};
    atomic<bool> running_{false};
    thread gc_;

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
    void wake_gc() { dirty_.store(true, memory_order_release); gc_cv_.notify_one(); }

    uint64_t gc_threshold() const {
        uint64_t p = published_.load(memory_order_seq_cst);   // published_ FIRST
        uint64_t rm = UINT64_MAX;
        for (size_t i = 0; i < MAX_READERS; ++i) {
            uint64_t v = slots_[i].load(memory_order_acquire);  // slots SECOND
            if (v != 0) rm = min(rm, v - 1);
        }
        return min(rm, p);
    }

    void advance_frontier() {
        uint64_t cur = published_.load(memory_order_seq_cst);
        for (;;) {
            uint64_t nxt = cur + 1;
            if (done_[nxt % RING].load(memory_order_acquire) != nxt) break;
            if (published_.compare_exchange_weak(cur, nxt, memory_order_seq_cst, memory_order_seq_cst))
                cur = nxt;  // tagged, never cleared; seq_cst so the double-check observes it
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
                if (!anchor && c <= m) { anchor = true; cur->prev.store(nullptr, memory_order_release); cur = nxt; continue; }
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
        for (auto& d : done_) d.store(0, memory_order_relaxed);
        for (auto& s : slots_) s.store(0, memory_order_relaxed);
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
                gc_cv_.wait(lk, [this]{ return !running_.load(memory_order_acquire) || dirty_.load(memory_order_acquire); });
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
            ts = clock_.fetch_add(1, memory_order_relaxed);  // lock-free reservation
            WIDEN_YIELD();
            Version* n = new Version(ts, value, heads_[idx].load(memory_order_relaxed));
            heads_[idx].store(n, memory_order_release);      // per-key link
        }
        done_[ts % RING].store(ts, memory_order_release);  // tag the ring slot
        advance_frontier();
        wake_gc();
    }

    optional<string> read(const string& key) {
        thread_local int slot = -1;
        if (slot < 0) {
            slot = (int)next_slot_.fetch_add(1, memory_order_relaxed);
            if (slot >= (int)MAX_READERS) throw runtime_error("ChronoKV: too many readers");
        }
        int idx = find_index(key);

        // Lock-free registration with seqlock-style double-check.
        uint64_t r;
        for (;;) {
            r = published_.load(memory_order_seq_cst);            // (1) sample
            WIDEN_YIELD();                                        // widen the gap -> stress the double-check
            slots_[slot].store(r + 1, memory_order_release);      // (2) register (r+1 keeps 0 = inactive)
            uint64_t r2 = published_.load(memory_order_seq_cst);  // (3) double-check
            if (r2 == r) break;                                   // (4) consistent snapshot
        }

        // Active: walk the chain for the newest version <= r.
        optional<string> result;
        if (idx >= 0) {
            Version* cur = heads_[idx].load(memory_order_acquire);
            while (cur) {
                uint64_t cts = cur->commit_ts.load(memory_order_acquire);
                if (cts != 0 && cts <= r) { result = cur->value; break; }
                cur = cur->prev.load(memory_order_acquire);
            }
        }

        slots_[slot].store(0, memory_order_release);            // (5) unregister
        return result;
    }
};

int main() {
    ChronoKV kv;
    kv.start_gc();
    kv.commit("a", "1");
    kv.commit("a", "2");
    auto a = kv.read("a");
    cout << "sequential read(a) = " << (a ? *a : string("null")) << "\n";

    constexpr int W = 4, R = 4, N = 8000;
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
    cout << "v8 stress phase done; shutting down\n";
    return 0;
}
