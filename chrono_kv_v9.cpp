// chrono_kv_v9.cpp — v8 + overflow log (fixes the RING-overflow liveness residual).
//
// v8's frontier wedges permanently if in-flight reservations exceed RING: a later
// completion overwrites an unconsumed ring slot and the earlier completion is lost.
// v9 adds an overflow log — a lock-free Treiber stack holding EVERY completion. The
// ring stays the O(1) fast path; the frontier falls back to the log on a ring miss.
// The log never loses a completion, so the frontier never wedges (liveness restored)
// and still never over-advances (safety preserved). Verified in RingOverflow.tla:
// UseRing=TRUE violates Liveness; the completed-set truth (what the log implements)
// satisfies Safety + Liveness.
//
// Reclamation: append-only here (no compaction) — bounded for the harness, a
// documented leak for production. Compacting entries <= published_ is next.
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

struct LogNode {                       // v9: overflow-log node (Treiber stack)
  uint64_t ts;
  atomic<LogNode*> next;
  LogNode(uint64_t t, LogNode* n) : ts(t), next(n) {}
};

class ChronoKV {
  mutable shared_mutex nm_;
  unordered_map<string,int> name2idx_;
  array<atomic<Version*>, MAX_KEYS> heads_;
  array<mutex, MAX_KEYS> commit_mu_;
  atomic<uint64_t> clock_{1};
  array<atomic<uint64_t>, RING> done_;
  atomic<uint64_t> published_{0};
  array<atomic<uint64_t>, MAX_READERS> slots_;
  atomic<unsigned> next_slot_{0};
  atomic<LogNode*> log_head_{nullptr};   // v9: overflow log
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

  void log_push(uint64_t ts) {           // v9: lock-free Treiber push
    LogNode* n = new LogNode(ts, nullptr);
    LogNode* h = log_head_.load(memory_order_relaxed);
    do {
      n->next.store(h, memory_order_relaxed);
    } while (!log_head_.compare_exchange_weak(h, n, memory_order_release, memory_order_relaxed));
  }

  bool in_log(uint64_t ts) const {       // v9: slow-path scan (only on a ring miss)
    LogNode* cur = log_head_.load(memory_order_acquire);
    while (cur) {
      if (cur->ts == ts) return true;
      cur = cur->next.load(memory_order_acquire);
    }
    return false;
  }

  uint64_t gc_threshold() const {
    uint64_t p = published_.load(memory_order_seq_cst);   // published_ FIRST (assumption 6)
    uint64_t rm = UINT64_MAX;
    for (size_t i = 0; i < MAX_READERS; ++i) {
      uint64_t v = slots_[i].load(memory_order_seq_cst);
      if (v != 0) rm = min(rm, v - 1);
    }
    return min(rm, p);
  }

  void advance_frontier() {
    uint64_t cur = published_.load(memory_order_seq_cst);
    for (;;) {
      uint64_t nxt = cur + 1;
      // v9: ring is the fast path; the log is the overflow-safe fallback.
      bool complete = (done_[nxt % RING].load(memory_order_acquire) == nxt) || in_log(nxt);
      if (!complete) break;
      if (published_.compare_exchange_weak(cur, nxt, memory_order_seq_cst, memory_order_seq_cst))
        cur = nxt;
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
    LogNode* ln = log_head_.load(memory_order_relaxed);   // v9: free the log
    while (ln) { LogNode* nxt = ln->next.load(memory_order_relaxed); delete ln; ln = nxt; }
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
      ts = clock_.fetch_add(1, memory_order_relaxed);
      WIDEN_YIELD();
      Version* n = new Version(ts, value, heads_[idx].load(memory_order_relaxed));
      heads_[idx].store(n, memory_order_release);
    }
    done_[ts % RING].store(ts, memory_order_release);
    log_push(ts);                        // v9: log every completion
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

    uint64_t r;
    for (;;) {
      r = published_.load(memory_order_seq_cst);
      WIDEN_YIELD();
      slots_[slot].store(r + 1, memory_order_seq_cst);
      uint64_t r2 = published_.load(memory_order_seq_cst);
      if (r2 == r) break;
    }

    optional<string> result;
    if (idx >= 0) {
      Version* cur = heads_[idx].load(memory_order_acquire);
      while (cur) {
        uint64_t cts = cur->commit_ts.load(memory_order_acquire);
        if (cts != 0 && cts <= r) { result = cur->value; break; }
        cur = cur->prev.load(memory_order_acquire);
      }
    }

    slots_[slot].store(0, memory_order_seq_cst);
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
  cout << "v9 stress phase done; shutting down\n";
  return 0;
}
