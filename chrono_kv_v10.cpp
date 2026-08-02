// chrono_kv_v10.cpp — v9 + read-only multi-key transactions (Phase 4a).
//
// ReadTransaction fixes one snapshot (read_ts) at begin, holds a registration
// slot for its whole life, and reads every key at that snapshot. Snapshot
// consistency is immediate (one read_ts); reclamation safety is inherited from
// v9 unchanged (the slot is held longer but the invariant reg_min <= read_ts is
// identical). seq_cst slots per the genmc-verified ordering.
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

struct LogNode {
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
  atomic<LogNode*> log_head_{nullptr};
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

  void log_push(uint64_t ts) {
    LogNode* n = new LogNode(ts, nullptr);
    LogNode* h = log_head_.load(memory_order_relaxed);
    do { n->next.store(h, memory_order_relaxed);
    } while (!log_head_.compare_exchange_weak(h, n, memory_order_release, memory_order_relaxed));
  }
  bool in_log(uint64_t ts) const {
    LogNode* cur = log_head_.load(memory_order_acquire);
    while (cur) { if (cur->ts == ts) return true; cur = cur->next.load(memory_order_acquire); }
    return false;
  }

  uint64_t gc_threshold() const {
    uint64_t p = published_.load(memory_order_seq_cst);
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
    LogNode* ln = log_head_.load(memory_order_relaxed);
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
    log_push(ts);
    advance_frontier();
    wake_gc();
  }

  // Single-shot read (v9 behavior): own snapshot, own slot.
  optional<string> read(const string& key) {
    uint64_t r; int slot = acquire_slot(r);
    auto res = read_at(r, key);
    release_slot(slot);
    return res;
  }

  // --- Phase 4a: transaction primitives ---
  int acquire_slot(uint64_t& read_ts) {
    thread_local int slot = -1;
    if (slot < 0) {
      slot = (int)next_slot_.fetch_add(1, memory_order_relaxed);
      if (slot >= (int)MAX_READERS) throw runtime_error("ChronoKV: too many readers");
    }
    uint64_t r;
    for (;;) {
      r = published_.load(memory_order_seq_cst);
      WIDEN_YIELD();
      slots_[slot].store(r + 1, memory_order_seq_cst);
      uint64_t r2 = published_.load(memory_order_seq_cst);
      if (r2 == r) break;
    }
    read_ts = r;
    return slot;
  }
  void release_slot(int slot) { slots_[slot].store(0, memory_order_seq_cst); }

  optional<string> read_at(uint64_t read_ts, const string& key) const {
    int idx = find_index(key);
    optional<string> result;
    if (idx >= 0) {
      Version* cur = heads_[idx].load(memory_order_acquire);
      while (cur) {
        uint64_t cts = cur->commit_ts.load(memory_order_acquire);
        if (cts != 0 && cts <= read_ts) { result = cur->value; break; }
        cur = cur->prev.load(memory_order_acquire);
      }
    }
    return result;
  }
};

// Read-only multi-key transaction: one snapshot for the whole lifetime.
class ReadTransaction {
  ChronoKV& kv_;
  uint64_t read_ts_;
  int slot_;
public:
  explicit ReadTransaction(ChronoKV& kv) : kv_(kv) { slot_ = kv_.acquire_slot(read_ts_); }
  ~ReadTransaction() { kv_.release_slot(slot_); }
  ReadTransaction(const ReadTransaction&) = delete;
  ReadTransaction& operator=(const ReadTransaction&) = delete;
  uint64_t read_ts() const { return read_ts_; }
  optional<string> read(const string& key) const { return kv_.read_at(read_ts_, key); }
};

int main() {
  ChronoKV kv;
  kv.start_gc();
  kv.commit("a", "1");
  kv.commit("b", "x");
  {
    ReadTransaction txn(kv);
    kv.commit("a", "2");          // lands AFTER the txn snapshot
    auto a1 = txn.read("a");      // must see "1" (snapshot), not "2"
    auto a2 = txn.read("a");      // must equal a1
    auto b  = txn.read("b");
    cout << "txn read a=" << (a1?*a1:"null") << " a=" << (a2?*a2:"null")
         << " b=" << (b?*b:"null") << " (snapshot " << txn.read_ts() << ")\n";
    cout << ((a1 && a2 && *a1 == *a2 && *a1 == "1") ? "SNAPSHOT CONSISTENT\n" : "SNAPSHOT BROKEN\n");
  }
  cout << "post-txn read(a) = " << *kv.read("a") << " (sees the commit)\n";

  constexpr int W = 4, R = 4, N = 4000;
  atomic<bool> go{false};
  vector<thread> ts;
  for (int w = 0; w < W; ++w)
    ts.emplace_back([&kv,&go,w]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) { kv.commit("a", to_string(i)); kv.commit("b", to_string(i)); }
    });
  for (int r = 0; r < R; ++r)
    ts.emplace_back([&kv,&go]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) {
        ReadTransaction txn(kv);
        (void)txn.read("a"); (void)txn.read("b"); (void)txn.read("a");
      }
    });
  go.store(true, memory_order_release);
  for (auto& t : ts) t.join();
  cout << "v10 stress phase done; shutting down\n";
  return 0;
}
