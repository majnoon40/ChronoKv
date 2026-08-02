// bench_phase2.cpp — A/B: ordered (v9) vs full-scan (Phase 2 option b).
//   g++ -std=c++17 -O2 -pthread bench_phase2.cpp -o bench_ordered
//   g++ -std=c++17 -O2 -pthread -DFULLSCAN bench_phase2.cpp -o bench_fullscan
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <chrono>
#include <iostream>
using namespace std;

struct Version { uint64_t commit_ts; string value; Version* prev; };

class OneKey {
  atomic<Version*> head_{nullptr};
  mutex commit_mu_;                       // ordered mode only
public:
  ~OneKey(){ Version* c=head_.load(); while(c){ Version* n=c->prev; delete c; c=n; } }

  void commit(uint64_t ts, const string& value) {
    Version* n = new Version{ts, value, nullptr};
#ifdef FULLSCAN
    Version* h = head_.load(memory_order_relaxed);          // lock-free CAS, no lock
    do { n->prev = h; }
    while (!head_.compare_exchange_weak(h, n, memory_order_release, memory_order_relaxed));
#else
    lock_guard<mutex> lk(commit_mu_);                       // per-key lock, ordered link
    n->prev = head_.load(memory_order_relaxed);
    head_.store(n, memory_order_release);
#endif
  }

  string read(uint64_t r) {
    Version* c = head_.load(memory_order_acquire);
#ifdef FULLSCAN
    uint64_t best = 0; string result;                       // scan the WHOLE chain
    while (c) { if (c->commit_ts <= r && c->commit_ts > best) { best = c->commit_ts; result = c->value; } c = c->prev; }
    return result;
#else
    while (c) { if (c->commit_ts <= r) return c->value; c = c->prev; }  // early stop
    return "";
#endif
  }
};

static double now_ms(){ return chrono::duration<double,milli>(chrono::steady_clock::now().time_since_epoch()).count(); }

int main() {
#ifdef FULLSCAN
  cout << "[variant: FULL-SCAN — lock-free CAS commit + full-scan read]\n";
#else
  cout << "[variant: ORDERED — per-key-lock commit + early-stop read]\n";
#endif
  // (1) commit throughput, hot key, 8 threads
  {
    const int W = 8, N = 100000;
    OneKey store; atomic<uint64_t> clock{1}; atomic<bool> go{false};
    vector<thread> ts;
    for (int w=0; w<W; ++w) ts.emplace_back([&]{ while(!go.load()) this_thread::yield();
      for (int i=0;i<N;++i) store.commit(clock.fetch_add(1), "v"); });
    double t0 = now_ms(); go.store(true);
    for (auto& t: ts) t.join();
    double dt = now_ms() - t0;
    cout << "  COMMIT hot-key  (" << W << " threads x " << N << "): "
         << (double)W*N/dt*1000.0/1e6 << " M commits/sec\n";
  }
  // (2) read throughput, long chain, 8 threads, recent snapshots
  {
    const int M = 2000, R = 8, NR = 100000;
    OneKey store; for (uint64_t i=1;i<=M;++i) store.commit(i, "value-payload");
    atomic<bool> go{false}; vector<thread> ts;
    for (int r=0;r<R;++r) ts.emplace_back([&,r]{ while(!go.load()) this_thread::yield();
      uint64_t seed = (uint64_t)r+1;
      for (int i=0;i<NR;++i){ seed = seed*6364136223846793005ULL+1;
        uint64_t rr = M - (seed>>33) % (M/5);   // recent snapshots: r in [M-M/5, M]
        store.read(rr); } });
    double t0 = now_ms(); go.store(true);
    for (auto& t: ts) t.join();
    double dt = now_ms() - t0;
    cout << "  READ long-chain (chain=" << M << ", " << R << " threads x " << NR << "): "
         << (double)R*NR/dt*1000.0/1e6 << " M reads/sec\n";
  }
  return 0;
}
