// chrono_kv_v10_oracle.cpp — snapshot-consistency oracle for read-only transactions.
//
// A transaction reads key "a" twice under concurrent commits. The oracle asserts
// (1) both reads agree [snapshot consistency] and (2) they equal the newest
// committed version <= the transaction's read_ts [correctness].
// -DBROKEN_TXN gives each read its OWN fresh snapshot (no shared read_ts), which
// lets a commit land between the reads and tear the snapshot — the control that
// must fire to prove the instrument is live.
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <string>
#include <optional>
#include <array>
#include <vector>
#include <cstdint>
#include <climits>
#include <algorithm>
#include <iostream>
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
  uint64_t ts; atomic<LogNode*> next;
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
  mutex gc_mu_; condition_variable gc_cv_;
  atomic<bool> dirty_{false}; atomic<bool> running_{false}; thread gc_;
  // oracle
  mutex oracle_mu_;
  array<vector<pair<uint64_t,string>>, MAX_KEYS> oracle_log_;
  atomic<long> violations_{0};

  int find_index(const string& k) const {
    shared_lock lk(nm_); auto it = name2idx_.find(k);
    return it == name2idx_.end() ? -1 : it->second;
  }
  int ensure_index(const string& k) {
    unique_lock lk(nm_); auto it = name2idx_.find(k);
    if (it != name2idx_.end()) return it->second;
    int idx = (int)name2idx_.size();
    if (idx >= (int)MAX_KEYS) throw runtime_error("MAX_KEYS exceeded");
    name2idx_[k] = idx; return idx;
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
      if (published_.compare_exchange_weak(cur, nxt, memory_order_seq_cst, memory_order_seq_cst)) cur = nxt;
    }
  }
  void gc_once() {
    uint64_t m = gc_threshold();
    size_t n; { shared_lock lk(nm_); n = name2idx_.size(); }
    for (size_t i = 0; i < n; ++i) {
      Version* cur = heads_[i].load(memory_order_acquire); bool anchor = false;
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
    gc_cv_.notify_all(); if (gc_.joinable()) gc_.join(); free_all();
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
    int idx = ensure_index(key); uint64_t ts;
    {
      lock_guard<mutex> lk(commit_mu_[idx]);
      ts = clock_.fetch_add(1, memory_order_relaxed);
      WIDEN_YIELD();
      { lock_guard<mutex> olk(oracle_mu_); oracle_log_[idx].push_back({ts, value}); }
      Version* n = new Version(ts, value, heads_[idx].load(memory_order_relaxed));
      heads_[idx].store(n, memory_order_release);
    }
    done_[ts % RING].store(ts, memory_order_release);
    log_push(ts); advance_frontier(); wake_gc();
  }
  int acquire_slot(uint64_t& read_ts) {
    thread_local int slot = -1;
    if (slot < 0) {
      slot = (int)next_slot_.fetch_add(1, memory_order_relaxed);
      if (slot >= (int)MAX_READERS) throw runtime_error("too many readers");
    }
    uint64_t r;
    for (;;) {
      r = published_.load(memory_order_seq_cst); WIDEN_YIELD();
      slots_[slot].store(r + 1, memory_order_seq_cst);
      if (published_.load(memory_order_seq_cst) == r) break;
    }
    read_ts = r; return slot;
  }
  void release_slot(int slot) { slots_[slot].store(0, memory_order_seq_cst); }
  optional<string> read_at(uint64_t read_ts, const string& key) const {
    int idx = find_index(key); optional<string> result;
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
  // oracle: check snapshot consistency + correctness for a two-read transaction
  void check_txn(uint64_t read_ts, const string& key,
                 const optional<string>& r1, const optional<string>& r2) {
    int idx = find_index(key);
    optional<string> expected;
    {
      lock_guard<mutex> olk(oracle_mu_);
      if (idx >= 0) {
        auto& log = oracle_log_[idx];
        for (auto rit = log.rbegin(); rit != log.rend(); ++rit)
          if (rit->first <= read_ts) { expected = rit->second; break; }
      }
    }
    if (r1 != r2) violations_.fetch_add(1);          // snapshot torn
    if (r1 != expected) violations_.fetch_add(1);    // wrong version
  }
  long violations() const { return violations_.load(); }
};

class ReadTransaction {
  ChronoKV& kv_; uint64_t read_ts_; int slot_;
public:
  explicit ReadTransaction(ChronoKV& kv) : kv_(kv) { slot_ = kv_.acquire_slot(read_ts_); }
  ~ReadTransaction() { kv_.release_slot(slot_); }
  uint64_t read_ts() const { return read_ts_; }
  optional<string> read(const string& key) const { return kv_.read_at(read_ts_, key); }
};

int main() {
  ChronoKV kv; kv.start_gc();
  kv.commit("a", "init");

  constexpr int W = 4, R = 4, N = 3000;
  atomic<bool> go{false};
  vector<thread> ts;
  for (int w = 0; w < W; ++w)
    ts.emplace_back([&kv,&go,w]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) kv.commit("a", "w" + to_string(w) + ":" + to_string(i));
    });
  for (int r = 0; r < R; ++r)
    ts.emplace_back([&kv,&go]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) {
#ifdef BROKEN_TXN
        uint64_t r1; int s1 = kv.acquire_slot(r1);
        auto a1 = kv.read_at(r1, "a"); kv.release_slot(s1);
        WIDEN_YIELD();
        uint64_t r2; int s2 = kv.acquire_slot(r2);
        auto a2 = kv.read_at(r2, "a"); kv.release_slot(s2);
        kv.check_txn(r1, "a", a1, a2);
#else
        ReadTransaction txn(kv);
        auto a1 = txn.read("a");
        WIDEN_YIELD();
        auto a2 = txn.read("a");
        kv.check_txn(txn.read_ts(), "a", a1, a2);
#endif
      }
    });
  go.store(true, memory_order_release);
  for (auto& t : ts) t.join();

  long v = kv.violations();
  cout << "oracle violations: " << v << "\n";
#ifdef BROKEN_TXN
  cout << (v > 0 ? "CONTROL FIRED (expected) — instrument sees torn snapshots\n"
                 : "CONTROL DID NOT FIRE — instrument broken; do NOT trust a clean v10\n");
#else
  cout << (v == 0 ? "v10 SNAPSHOT CLEAN (every transaction saw one consistent snapshot)\n"
                  : "v10 SNAPSHOT VIOLATIONS — torn reads detected\n");
#endif
  return 0;
}
