// chrono_kv_v2.cpp — POSITIVE CONTROL · this file is SUPPOSED to trip TSan/ASan.
// Build via build.sh (widen). Under the widened ThreadSanitizer build this MUST
// report a heap-use-after-free / data race. If it comes back silent, the build is
// misconfigured and v7's clean result is meaningless — read this before trusting v7.
//
// Kept history (the defect this control still carries, by design):
//   per-key commit mutex BUT a GLOBAL reservation clock_ that read_ts is drawn
//   from. A reader can sample a read_ts that a concurrent commit on another key
//   bumped before its own head-store, then walk into a node GC reclaims once a
//   newer commit links on this key — a same-key UAF. GC is bounded by reg_min()
//   alone (no published_ watermark), so it can anchor on a just-linked node and
//   truncate a version a lagging reader still needs.
// Carried fixes (so the control isolates the one defect we want TSan to see):
//   C1 .size()  C2 mutable rm_  L1 anchor truncation  L3 fused sample+register
//   L4 dtor order  L5 bounds  L6 shared-lock slot count  L7 RAII unregister  L9 <=
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
#include <chrono>
#include <iostream>
using namespace std;
using namespace std::chrono;
const size_t MAX_KEYS = 64;
#ifdef WIDEN
  #define WIDEN_YIELD() std::this_thread::yield()
#else
  #define WIDEN_YIELD() ((void)0)
#endif
struct Version {
  atomic<uint64_t> commit_ts;
  string           value;
  atomic<Version*> prev;
  Version(uint64_t ts, string v, Version* p)
      : commit_ts(ts), value(std::move(v)), prev(p) {}
};
class ChronoKV {
  mutable shared_mutex              nm_;
  unordered_map<string,int>         name2idx_;
  array<atomic<Version*>,MAX_KEYS>  heads_;
  array<mutex,MAX_KEYS>            commit_mu_;   // per-key only
  atomic<uint64_t> clock_{1};                    // GLOBAL clock (the trap)
  mutable mutex    rm_;
  vector<uint64_t> reg_;
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
  uint64_t reg_min() const {
    lock_guard<mutex> lk(rm_);
    if (reg_.empty()) return UINT64_MAX;
    return *min_element(reg_.begin(), reg_.end());
  }
  struct ReaderUnregister {
    ChronoKV* self; uint64_t ts;
    ~ReaderUnregister() {
      lock_guard<mutex> lk(self->rm_);
      auto it = find(self->reg_.begin(), self->reg_.end(), ts);
      if (it != self->reg_.end()) self->reg_.erase(it);
    }
  };
  void gc_once() {
    WIDEN_YIELD();
    uint64_t m = reg_min();                       // no watermark: reg_min alone
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
        if (anchor) delete cur;                   // <- the UAF TSan should see
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
  ChronoKV()  { for (auto& h : heads_) h.store(nullptr, memory_order_relaxed); }
  ~ChronoKV() {
    running_.store(false, memory_order_release);
    if (gc_.joinable()) gc_.join();
    free_all();
  }
  void start_gc() {
    running_.store(true, memory_order_release);
    gc_ = thread([this]{
      while (running_.load(memory_order_acquire)) { gc_once(); this_thread::yield(); }
    });
  }
  void commit(const string& k, const string& v) {
    int idx = ensure_index(k);
    lock_guard<mutex> lk(commit_mu_[idx]);        // per-key only
    uint64_t ts = clock_.fetch_add(1, memory_order_acq_rel);
    WIDEN_YIELD();
    Version* n = new Version(ts, v, heads_[idx].load(memory_order_relaxed));
    heads_[idx].store(n, memory_order_release);   // no watermark decoupling
  }
  optional<string> read(const string& k) {
    uint64_t r;
    { lock_guard<mutex> lk(rm_);                  // fused, but samples the global clock
      r = clock_.load(memory_order_acquire);
      reg_.push_back(r); }
    ReaderUnregister guard{this, r};
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
  cout << "v2 control done (TSan SHOULD have complained above)\n";
  return 0;
}
