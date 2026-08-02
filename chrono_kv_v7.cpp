// chrono_kv_v7.cpp — v7 candidate · TRACE-LEVEL · NOT COMPILED BY THE AUTHOR.
// Build via build.sh (asan / tsan / widen). The widened TSan build is the test:
// this file MUST come back clean WHILE chrono_kv_v2.cpp trips TSan (the control).
//
// v7 reconstruction of the commit path: lock-free fetch_add reservations, per-key
// link serialization (chain order == ts order without a global lock), and a TAGGED
// contiguous completion frontier that closes v6's boolean-ring ABA. done_[slot]
// stores the timestamp that completed into it; advance_frontier() walks while
// done_[nxt]==nxt and NEVER clears a slot, so the watermark can over-advance past
// an unlinked commit in no execution. The read side is v5's proven fusion, kept.
//
// Open items carried, not hidden:
//   L12 / RING — bounded in-flight contract. If outstanding reservations exceed
//       RING, a later completion overwrites an unconsumed slot and the frontier
//       stalls permanently (a freshness failure, never a wrong value). Safety is
//       unconditional on RING; liveness is the contract.
//   Reclamation theorem — still a proof obligation (informal sketch + the v2
//       pre/post-fusion UAF history + v5 ASan-clean as anchors; not mechanized).
//   rm_ read-side contention — carried; a lock-free registration is declined as
//       unverified (it would re-open the L3 gap).
//
// Fixes carried: C1 C2 L1 L3 L4 L5 L6 L7 L9 L10. New vs v5: L2 per-key + L8/L11
// tagged lock-free frontier. condvar GC + intrusive-list reg_ are the efficiency
// changes (no new concurrency invariant).
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <string>
#include <optional>
#include <list>
#include <array>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace std;
constexpr size_t MAX_KEYS = 64;
constexpr size_t RING     = 1024;   // [L12] must exceed max in-flight reservations
static_assert(RING >= 64, "RING must exceed the in-flight window");
#ifdef WIDEN
  #define WIDEN_YIELD() std::this_thread::yield()
#else
  #define WIDEN_YIELD() ((void)0)
#endif
struct Version {
  atomic<uint64_t> commit_ts;       // stamped in ctor, before link
  string           value;
  atomic<Version*> prev;
  Version(uint64_t ts, string v, Version* p)
      : commit_ts(ts), value(std::move(v)), prev(p) {}
};
class ChronoKV {
  mutable shared_mutex              nm_;
  unordered_map<string,int>         name2idx_;
  array<atomic<Version*>,MAX_KEYS>  heads_;
  array<mutex,MAX_KEYS>            commit_mu_;   // [L2] per-key link serialization
  atomic<uint64_t> clock_{1};                    // lock-free reservation counter
  array<atomic<uint64_t>,RING>     done_;        // [L11] done_[ts%RING] = ts (the tag)
  atomic<uint64_t> published_{0};                // [L8] contiguous tagged frontier
  mutable mutex    rm_;                          // read-side fusion (kept from v5)
  list<uint64_t>   reg_;                         // [L6-fix] intrusive list, O(1) erase
  mutex            gc_mu_;                       // [L4-fix] event-driven GC
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
    if (idx >= (int)MAX_KEYS) throw runtime_error("ChronoKV: MAX_KEYS exceeded"); // [L5]
    name2idx_[k] = idx;
    return idx;
  }
  struct ReaderUnregister {                       // [L7] + O(1) erase via iterator
    ChronoKV* self; list<uint64_t>::iterator it;
    ~ReaderUnregister() {
      lock_guard<mutex> lk(self->rm_);
      self->reg_.erase(it);
      self->wake_gc();
    }
  };
  void wake_gc() { dirty_.store(true, memory_order_release); gc_cv_.notify_one(); }
  uint64_t gc_threshold() const {                 // [L10] one snapshot under rm_ (kept)
    lock_guard<mutex> lk(rm_);
    uint64_t p = published_.load(memory_order_acquire);
    if (reg_.empty()) return p;
    uint64_t m = p;
    for (uint64_t r : reg_) m = min(m, r);
    return m;
  }
  void advance_frontier() {                       // [L11] tagged, NO clear
    uint64_t cur = published_.load(memory_order_acquire);
    for (;;) {
      uint64_t nxt = cur + 1;
      uint64_t tag = done_[nxt % RING].load(memory_order_acquire);
      if (tag != nxt) break;                      // exact match: no ABA, no over-advance
      if (published_.compare_exchange_weak(cur, nxt,
              memory_order_acq_rel, memory_order_acquire)) {
        cur = nxt;                                // slot keeps its tag until next occupant
      }
    }
  }
  void gc_once() {
    WIDEN_YIELD();
    uint64_t m = gc_threshold();
    size_t n; { shared_lock lk(nm_); n = name2idx_.size(); }   // [L6][C1]
    for (size_t i = 0; i < n; ++i) {
      Version* cur = heads_[i].load(memory_order_acquire);
      bool anchor = false;
      while (cur) {
        Version* nxt = cur->prev.load(memory_order_acquire);
        uint64_t c = cur->commit_ts.load(memory_order_acquire);
        if (c == 0) { cur = nxt; continue; }
        if (!anchor && c <= m) {                  // [L9] <=
          anchor = true;
          cur->prev.store(nullptr, memory_order_release);  // [L1]
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
    { lock_guard<mutex> lk(gc_mu_); running_.store(false, memory_order_release); } // [L4]
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
        lk.unlock(); gc_once(); lk.lock();        // [L4-fix] no busy spin
      }
    });
  }
  void commit(const string& k, const string& v) {
    int idx = ensure_index(k);
    uint64_t ts;
    { lock_guard<mutex> lk(commit_mu_[idx]);      // [L2] per-key: link order == ts order
      ts = clock_.fetch_add(1, memory_order_relaxed);
      WIDEN_YIELD();
      Version* n = new Version(ts, v, heads_[idx].load(memory_order_relaxed));
      heads_[idx].store(n, memory_order_release); }
    done_[ts % RING].store(ts, memory_order_release);   // [L11] the TAG, not a boolean
    advance_frontier();
    wake_gc();
  }
  optional<string> read(const string& k) {
    uint64_t r; list<uint64_t>::iterator it;
    { lock_guard<mutex> lk(rm_);                   // [L3] fusion kept verbatim from v5
      r = published_.load(memory_order_acquire);
      it = reg_.insert(reg_.end(), r); }
    ReaderUnregister guard{this, it};
    wake_gc();
    int idx = find_index(k);
    if (idx < 0) return nullopt;
    Version* cur = heads_[idx].load(memory_order_acquire);
    while (cur) {
      uint64_t c = cur->commit_ts.load(memory_order_acquire);
      if (c != 0 && c <= r) return cur->value;
      cur = cur->prev.load(memory_order_acquire);
    }
    return nullopt;
  }
};
int main() {
  ChronoKV kv;
  kv.start_gc();
  kv.commit("a", "1");
  kv.commit("a", "2");
  auto sanity = kv.read("a");
  cout << "sequential read(a) = " << (sanity ? *sanity : string("null")) << "\n";
  constexpr int W = 4, R = 4, N = 8000;
  atomic<bool> go{false};
  vector<thread> ts;
  for (int w = 0; w < W; ++w)
    ts.emplace_back([&kv, &go, w]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) {
        kv.commit("a", "w" + to_string(w) + ":" + to_string(i));
        kv.commit("b", "w" + to_string(w) + ":" + to_string(i));
      }
    });
  for (int r = 0; r < R; ++r)
    ts.emplace_back([&kv, &go]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) { (void)kv.read("a"); (void)kv.read("b"); }
    });
  go.store(true, memory_order_release);
  for (auto& t : ts) t.join();
  cout << "v7 stress phase done; shutting down\n";
  return 0;
}
