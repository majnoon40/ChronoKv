// litmus_v8.cpp — deterministic litmus test for the v8 GC-read-order bug.
//
// Forces the torn-read interleaving with explicit thread coordination (no timing):
//
//   R: sample r=1
//   G: read FIRST   (buggy: slot=0, R not registered;  fixed: published=1)
//   R: register slot=2, double-check (published still 1), commit to r=1,
//      walk to the needed version v1, PAUSE holding the pointer
//   C: publish 2  (published_=2)
//   G: read SECOND (buggy: published=2 -> m=2 -> delete v1;  fixed: slot=2 -> m=1 -> keep v1)
//   R: resume, read v1->value  ->  buggy: heap-use-after-free;  fixed: safe
//
// The reader pauses AFTER loading the v1 pointer but BEFORE reading v1->value,
// so the GC frees v1 out from under the live pointer — the exact UAF shape ASan
// reported in the real v8.
//
// Build & run:
//   g++ -std=c++17 -O1 -g -fsanitize=address -pthread -DBUGGY_ORDER litmus_v8.cpp -o litmus_buggy
//   g++ -std=c++17 -O1 -g -fsanitize=address -pthread               litmus_v8.cpp -o litmus_fixed
//   ./litmus_buggy   # expect: AddressSanitizer: heap-use-after-free
//   ./litmus_fixed   # expect: "litmus complete: no UAF"
#include <atomic>
#include <thread>
#include <string>
#include <iostream>
#include <cstdint>
#include <climits>
#include <algorithm>
using namespace std;

struct Version {
    uint64_t commit_ts;
    string value;
    Version* prev;
};

static atomic<uint64_t> published_{0};
static atomic<uint64_t> slot_{0};       // single reader slot (0 = inactive, else snapshot+1)
static Version* head_ = nullptr;
static atomic<int> phase_{0};           // coordination counter

static void wait_phase(int p) {
    while (phase_.load(memory_order_acquire) < p) this_thread::yield();
}

// GC threshold. The ONLY difference between the two builds is the read order here.
static uint64_t gc_threshold() {
    uint64_t rm, p;
    wait_phase(1);                                    // R has sampled r
    #ifdef BUGGY_ORDER
    uint64_t v = slot_.load(memory_order_acquire);    // slots FIRST (R not registered yet -> 0)
    rm = (v != 0) ? v - 1 : UINT64_MAX;
    phase_.store(2, memory_order_release);            // signal: G read first
    wait_phase(4);                                    // committer has published
    p = published_.load(memory_order_seq_cst);        // published_ SECOND (=2)  <- torn read
    #else
    p = published_.load(memory_order_seq_cst);        // published_ FIRST (=1, before the publish)
    phase_.store(2, memory_order_release);            // signal: G read first
    wait_phase(4);                                    // committer has published
    uint64_t v = slot_.load(memory_order_acquire);    // slots SECOND (=2, R now registered)
    rm = (v != 0) ? v - 1 : UINT64_MAX;
    #endif
    return min(rm, p);
}

static void gc_once() {
    uint64_t m = gc_threshold();
    Version* cur = head_;
    bool anchored = false;
    while (cur) {
        Version* nxt = cur->prev;
        if (!anchored && cur->commit_ts <= m) {         // anchor = newest <= m
            anchored = true;
            cur->prev = nullptr;                          // truncate below anchor
            cur = nxt;
            continue;
        }
        if (anchored) delete cur;                       // reclaim below anchor
        cur = nxt;
    }
    phase_.store(5, memory_order_release);            // signal: G done
}

static void reader() {
    uint64_t r = published_.load(memory_order_seq_cst);   // (1) sample r=1
    phase_.store(1, memory_order_release);
    wait_phase(2);                                        // G has read first
    slot_.store(r + 1, memory_order_release);             // (2) register slot=2
    uint64_t r2 = published_.load(memory_order_seq_cst);  // (3) double-check (=1, commit)
    (void)r2;
    // Walk to the needed version (newest <= r).
    Version* cur = head_;                                 // v2 (ts=2 > 1)
    while (cur) {
        if (cur->commit_ts <= r) {                          // cur = v1 (ts=1 <= 1): the needed version
            phase_.store(3, memory_order_release);            // signal: R holds the v1 pointer
            wait_phase(5);                                    // wait for G to finish (delete or keep)
            volatile char c = cur->value[0];                  // READ v1->value -> UAF if v1 freed
            (void)c;
            break;
        }
        cur = cur->prev;                                    // v2.prev = v1 (loaded before G unlinks)
    }
}

static void committer() {
    wait_phase(3);                                        // R holds the v1 pointer
    published_.store(2, memory_order_seq_cst);            // publish version 2
    phase_.store(4, memory_order_release);
}

int main() {
    Version* v1 = new Version{1, "version-1-value", nullptr};
    Version* v2 = new Version{2, "version-2-value", v1};
    head_ = v2;
    published_.store(1, memory_order_seq_cst);            // watermark starts at 1

    thread t_r(reader);
    thread t_c(committer);
    thread t_g(gc_once);
    t_r.join(); t_c.join(); t_g.join();

    cout << "litmus complete: no UAF\n";
    return 0;
}
