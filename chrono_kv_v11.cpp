// chrono_kv_v11.cpp — v10 + read-write transactions (Phase 4b).
//
// ReadWriteTransaction: buffered writes, one commit_ts for the whole txn, commit
// takes the per-key commit_mu_ locks in sorted-index order (deadlock-free),
// validates last_write_ts[key] <= read_ts for every written key (first-committer-
// wins), and only then links the versions. Reads check the buffer first
// (read-your-own-writes), then the chain at read_ts. Reads stay lock-free; only
// the transactional COMMIT path locks, per overlapping key. Reclamation safety is
// inherited (slot held begin->commit). seq_cst slots per the genmc-verified order.
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <map>
#include <string>
#include <optional>
#include <array>
#include <vector>
#include <tuple>
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
  array<atomic<uint64_t>, MAX_KEYS> last_write_ts_;   // v11: per-key last commit ts
  atomic<uint64_t> clock_{1};
  array<atomic<uint64_t>, RING> done_;
  atomic<uint64_t> published_{0};
  array<atomic<uint64_t>, MAX_READERS> slots_;
  atomic<unsigned> next_slot_{0};
  atomic<LogNode*> log_head_{nullptr};
  mutex gc_mu_; condition_variable gc_cv_;
  atomic<bool> dirty_{false}; atomic<bool> running_{false}; thread gc_;

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
    for (auto& w : last_write_ts_) w.store(0, memory_order_relaxed);
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

  // Single-key commit (also updates last_write_ts so txns can detect the conflict).
  void commit(const string& key, const string& value) {
    int idx = ensure_index(key); uint64_t ts;
    {
      lock_guard<mutex> lk(commit_mu_[idx]);
      ts = clock_.fetch_add(1, memory_order_relaxed);
      WIDEN_YIELD();
      Version* n = new Version(ts, value, heads_[idx].load(memory_order_relaxed));
      heads_[idx].store(n, memory_order_release);
      last_write_ts_[idx].store(ts, memory_order_relaxed);
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

  // Single-shot read (own snapshot, own slot).
  optional<string> read(const string& key) {
    uint64_t r; int slot = acquire_slot(r);
    auto res = read_at(r, key);
    release_slot(slot);
    return res;
  }

  // v11: commit a transaction's write set. Returns false on conflict (abort).
  bool commit_txn(uint64_t read_ts, const map<string,string>& write_set) {
    if (write_set.empty()) return true;
    // Resolve keys -> indices, then sort by index for a canonical lock order.
    vector<tuple<int,string,string>> items;
    for (auto& [k, v] : write_set) items.emplace_back(ensure_index(k), k, v);
    sort(items.begin(), items.end(),
         [](auto& a, auto& b){ return get<0>(a) < get<0>(b); });
    for (auto& [idx, k, v] : items) commit_mu_[idx].lock();

    // Validate: first-committer-wins.
    for (auto& [idx, k, v] : items) {
      if (last_write_ts_[idx].load(memory_order_relaxed) > read_ts) {
        for (auto& [i2, k2, v2] : items) commit_mu_[i2].unlock();
        return false;   // conflict — abort, no commit_ts reserved
      }
    }

    uint64_t cts = clock_.fetch_add(1, memory_order_relaxed);
    WIDEN_YIELD();

    // Apply: link every buffered write at the single commit_ts.
    for (auto& [idx, k, v] : items) {
      Version* n = new Version(cts, v, heads_[idx].load(memory_order_relaxed));
      heads_[idx].store(n, memory_order_release);
      last_write_ts_[idx].store(cts, memory_order_relaxed);
    }
    for (auto& [idx, k, v] : items) commit_mu_[idx].unlock();

    done_[cts % RING].store(cts, memory_order_release);
    log_push(cts); advance_frontier(); wake_gc();
    return true;
  }
};

// Read-only transaction (Phase 4a).
class ReadTransaction {
  ChronoKV& kv_; uint64_t read_ts_; int slot_;
public:
  explicit ReadTransaction(ChronoKV& kv) : kv_(kv) { slot_ = kv_.acquire_slot(read_ts_); }
  ~ReadTransaction() { kv_.release_slot(slot_); }
  uint64_t read_ts() const { return read_ts_; }
  optional<string> read(const string& key) const { return kv_.read_at(read_ts_, key); }
};

// Read-write transaction (Phase 4b).
class ReadWriteTransaction {
  ChronoKV& kv_; uint64_t read_ts_; int slot_;
  map<string,string> write_set_;
  bool finished_ = false;
public:
  explicit ReadWriteTransaction(ChronoKV& kv) : kv_(kv) { slot_ = kv_.acquire_slot(read_ts_); }
  ~ReadWriteTransaction() { if (!finished_) { kv_.release_slot(slot_); } }
  ReadWriteTransaction(const ReadWriteTransaction&) = delete;
  ReadWriteTransaction& operator=(const ReadWriteTransaction&) = delete;
  uint64_t read_ts() const { return read_ts_; }
  optional<string> read(const string& key) {
    auto it = write_set_.find(key);
    if (it != write_set_.end()) return it->second;   // read-your-own-writes
    return kv_.read_at(read_ts_, key);
  }
  void write(const string& key, const string& value) { write_set_[key] = value; }
  bool commit() {
    finished_ = true;
    bool ok = kv_.commit_txn(read_ts_, write_set_);
    kv_.release_slot(slot_);
    return ok;
  }
  void abort() { finished_ = true; kv_.release_slot(slot_); }
};

int main() {
  ChronoKV kv; kv.start_gc();
  kv.commit("acct_a", "100");
  kv.commit("acct_b", "100");

  // Transfer 30 from a to b, atomically across both keys.
  {
    bool done = false;
    while (!done) {
      ReadWriteTransaction txn(kv);
      auto a = txn.read("acct_a"); auto b = txn.read("acct_b");
      int va = stoi(*a), vb = stoi(*b);
      txn.write("acct_a", to_string(va - 30));
      txn.write("acct_b", to_string(vb + 30));
      done = txn.commit();   // retries on conflict
    }
  }
  auto a = kv.read("acct_a"); auto b = kv.read("acct_b");
  cout << "after transfer: a=" << *a << " b=" << *b
       << " sum=" << (stoi(*a) + stoi(*b)) << " (must be 200)\n";
  cout << ((stoi(*a) == 70 && stoi(*b) == 130) ? "TXN OK\n" : "TXN BROKEN\n");

  // Stress: concurrent read-write transactions + single-key commits.
  constexpr int W = 4, R = 4, N = 2000;
  atomic<bool> go{false};
  vector<thread> ts;
  for (int w = 0; w < W; ++w)
    ts.emplace_back([&kv,&go,w]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) {
        bool ok = false;
        while (!ok) {
          ReadWriteTransaction txn(kv);
          auto x = txn.read("acct_a"); auto y = txn.read("acct_b");
          int vx = stoi(*x), vy = stoi(*y);
          txn.write("acct_a", to_string(vx + 1));
          txn.write("acct_b", to_string(vy - 1));
          ok = txn.commit();
        }
      }
    });
  for (int r = 0; r < R; ++r)
    ts.emplace_back([&kv,&go]{
      while (!go.load(memory_order_acquire)) this_thread::yield();
      for (int i = 0; i < N; ++i) {
        ReadTransaction txn(kv);
        (void)txn.read("acct_a"); (void)txn.read("acct_b");
      }
    });
  go.store(true, memory_order_release);
  for (auto& t : ts) t.join();
  auto fa = kv.read("acct_a"); auto fb = kv.read("acct_b");
  cout << "final sum=" << (stoi(*fa) + stoi(*fb)) << " (must still be 200)\n";
  cout << "v11 stress phase done; shutting down\n";
  return 0;
}
