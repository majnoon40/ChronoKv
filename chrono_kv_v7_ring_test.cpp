// chrono_kv_v7_ring_test.cpp — RING-overflow liveness/safety test for v7.
//
// The one residual the stress tests can't reach: in-flight > RING. v7's claim:
// the frontier may STALL (liveness — committed data stays invisible) but must
// NEVER OVER-ADVANCE (safety — published_ never exceeds the contiguous completed
// prefix, so readers never see uncommitted data).
//
// RING=4 (tiny, to force overflow deterministically). commit_stalled() links a
// node without completing it, wedging the frontier; the scenario then overflows
// the ring and checks safety holds throughout and liveness degrades as predicted.
//
// Build & run:
//   g++ -std=c++17 -O0 -g -pthread chrono_kv_v7_ring_test.cpp -o v7_ring_test
//   ./v7_ring_test
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <string>
#include <optional>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <cassert>
using namespace std;

constexpr size_t MAX_KEYS = 64;
constexpr size_t RING = 4;   // deliberately tiny to force overflow

struct Version {
    atomic<uint64_t> commit_ts;
    string value;
    atomic<Version*> prev;
    Version(uint64_t ts, string v, Version* p) : commit_ts(ts), value(move(v)), prev(p) {}
};

// test-only ground truth: which timestamps have completed
static mutex gt_mu;
static vector<bool> completed(1000, false);

class ChronoKV {
    mutable shared_mutex nm_;
    unordered_map<string,int> name2idx_;
    array<atomic<Version*>,MAX_KEYS> heads_;
    array<mutex,MAX_KEYS> commit_mu_;
    atomic<uint64_t> clock_{1};
    array<atomic<uint64_t>,RING> done_;
    atomic<uint64_t> published_{0};
    mutable mutex rm_;
    vector<uint64_t> reg_;

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
        name2idx_[k] = idx;
        return idx;
    }
    void advance_frontier() {
        uint64_t cur = published_.load(memory_order_acquire);
        for (;;) {
            uint64_t nxt = cur + 1;
            if (done_[nxt % RING].load(memory_order_acquire) != nxt) break;
            if (published_.compare_exchange_weak(cur, nxt, memory_order_acq_rel, memory_order_acquire))
                cur = nxt;
        }
    }
    void mark_completed(uint64_t ts) {
        lock_guard<mutex> lk(gt_mu);
        completed[ts] = true;
    }
public:
    ChronoKV() {
        for (auto& h : heads_) h.store(nullptr, memory_order_relaxed);
        for (auto& d : done_) d.store(0, memory_order_relaxed);
    }
    void commit(const string& key, const string& value) {
        int idx = ensure_index(key);
        uint64_t ts;
        { lock_guard<mutex> lk(commit_mu_[idx]);
            ts = clock_.fetch_add(1, memory_order_relaxed);
            Version* n = new Version(ts, value, heads_[idx].load(memory_order_relaxed));
            heads_[idx].store(n, memory_order_release);
        }
        done_[ts % RING].store(ts, memory_order_release);
        mark_completed(ts);
        advance_frontier();
    }
    // TEST-ONLY: reserve + link, but do NOT complete (stalls the frontier at ts-1)
    uint64_t commit_stalled(const string& key, const string& value) {
        int idx = ensure_index(key);
        uint64_t ts;
        { lock_guard<mutex> lk(commit_mu_[idx]);
            ts = clock_.fetch_add(1, memory_order_relaxed);
            Version* n = new Version(ts, value, heads_[idx].load(memory_order_relaxed));
            heads_[idx].store(n, memory_order_release);
        }
        return ts;   // deliberately NO done_[ts].store, NO mark_completed
    }
    // TEST-ONLY: complete a previously stalled reservation
    void complete_stalled(uint64_t ts) {
        done_[ts % RING].store(ts, memory_order_release);
        mark_completed(ts);
        advance_frontier();
    }
    uint64_t get_published() { return published_.load(memory_order_acquire); }
    uint64_t contiguous_prefix() {
        lock_guard<mutex> lk(gt_mu);
        uint64_t f = 0;
        while (f + 1 < completed.size() && completed[f + 1]) ++f;
        return f;
    }
    uint64_t max_completed() {
        lock_guard<mutex> lk(gt_mu);
        uint64_t m = 0;
        for (size_t i = 0; i < completed.size(); ++i) if (completed[i]) m = i;
        return m;
    }
    optional<string> read(const string& key) {
        uint64_t read_ts = published_.load(memory_order_acquire);
        int idx = find_index(key);
        if (idx < 0) return nullopt;
        Version* cur = heads_[idx].load(memory_order_acquire);
        while (cur) {
            uint64_t cts = cur->commit_ts.load(memory_order_acquire);
            if (cts != 0 && cts <= read_ts) return cur->value;
            cur = cur->prev.load(memory_order_acquire);
        }
        return nullopt;
    }
};

int main() {
    ChronoKV kv;
    auto safety_ok = [&]() { return kv.get_published() <= kv.contiguous_prefix(); };

    // Phase 1: normal commits → frontier at 2
    kv.commit("a", "v1");   // ts=1
    kv.commit("a", "v2");   // ts=2
    cout << "Phase 1: published=" << kv.get_published() << " prefix=" << kv.contiguous_prefix() << "\n";
    assert(kv.get_published() == 2 && safety_ok());

    // Phase 2: stall ts=3 (linked, not completed) → frontier wedged at 2
    uint64_t stalled = kv.commit_stalled("a", "stalled");
    cout << "Phase 2 (stall ts=" << stalled << "): published=" << kv.get_published() << " prefix=" << kv.contiguous_prefix() << "\n";
    assert(kv.get_published() == 2 && safety_ok());

    // Phase 3: overflow — complete 5 on "b" (> RING=4) while wedged
    for (int i = 0; i < 5; ++i) kv.commit("b", "b" + to_string(i));  // ts=4..8
    cout << "Phase 3 (overflow): published=" << kv.get_published() << " prefix=" << kv.contiguous_prefix() << " max_completed=" << kv.max_completed() << "\n";
    assert(kv.get_published() == 2 && safety_ok());   // SAFETY holds while wedged

    // Phase 4: release the stall → frontier advances to 3, then wedges PERMANENTLY
    kv.complete_stalled(stalled);
    cout << "Phase 4 (release): published=" << kv.get_published() << " prefix=" << kv.contiguous_prefix() << " max_completed=" << kv.max_completed() << "\n";
    assert(safety_ok());                              // SAFETY still holds
    assert(kv.get_published() == 3);                  // advanced to 3...
    assert(kv.get_published() < kv.max_completed());  // ...but did NOT catch up to 8

    // Reads are still CORRECT for the visible snapshot (safety), even though
    // committed data on "b" (ts4-8) is invisible (liveness degraded).
    auto a = kv.read("a");   // snapshot=3 → "stalled"
    auto b = kv.read("b");   // snapshot=3 → no "b" <= 3 → nullopt
    cout << "read(a)@" << kv.get_published() << " = " << (a ? *a : string("<none>")) << "\n";
    cout << "read(b)@" << kv.get_published() << " = " << (b ? *b : string("<none>")) << "  (ts4-8 committed but invisible)\n";
    assert(a && *a == "stalled");
    assert(!b);

    cout << "\n=== RING-OVERFLOW RESULT ===\n";
    cout << "SAFETY  (published <= contiguous prefix): " << (safety_ok() ? "HOLDS" : "VIOLATED") << "\n";
    cout << "LIVENESS (caught up to max completed):    "
    << (kv.get_published() == kv.max_completed() ? "yes"
    : "DEGRADED — stalled at " + to_string(kv.get_published()) + ", max completed " + to_string(kv.max_completed())) << "\n";
    cout << "Predicted residual confirmed: overflow stalls the frontier (liveness)\n"
    << "but never over-advances it (safety).\n";
    return 0;
}
