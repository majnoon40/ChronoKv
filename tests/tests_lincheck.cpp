// tests/tests_lincheck.cpp (lincheck namespace + run_lincheck_test) — v27 M1 strict-serializability checker — extracted from main.cpp (v29 M2 item 5: the test-suite TU
// split, unlocked by API-1's ODR-safe header). Content is byte-identical to
// the extracted region except: the battery entry point loses `static` (it is
// declared in tests/test_decls.hpp and called from main.cpp), and the whole
// body rides under CHRONOKV_TEST_HOOKS exactly as it did inside main.cpp's
// self-tests section. No engine or test logic changed in the move.
#include "chronokv.hpp"
#include "chronokv.hpp"   // API-1: header must be idempotent (#pragma once)
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>
#include <cstring>

#ifdef CHRONOKV_TEST_HOOKS

// =====================================================================
// v27 M1 (ROADMAP v27 M1: "History -> strict-serializability checker")
//
// lincheck consumes txnrec:: records — a structured history of public-API
// calls — and checks the recorded history against the consistency model
// the engine claims: STRICT SERIALIZABILITY = snapshot isolation over the
// global cts total order + real-time order.
//
// Why no Knossos/Jepsen-style search is needed (the roadmap's argument,
// restated): cts is a TOTAL, monotonic counter — every committed write
// transaction gets a unique cts and every snapshot read is defined as
// "the state as of read_ts". The candidate total order is therefore not
// searched for; it IS cts order, and checking collapses to:
//
//   (a) SNAPSHOT SOUNDNESS — replay committed writes in cts order; every
//       recorded read must equal the reconstructed state as of its
//       snapshot (read-your-writes overlay included), and no read may
//       observe a version whose cts exceeds its snapshot.
//   (b) REAL-TIME ORDER — if a write W was acknowledged before a txn T
//       began (recorded intervals separated by a small slack that absorbs
//       in-library timestamping error), then W.cts < T.cts for committed
//       writers T, and W.cts <= T.snapshot for any snapshot-taking T.
//       Both are theorems about this engine: commit_txn publishes
//       (pub_.complete) BEFORE returning, cts is assigned from a
//       monotonic clock, and a snapshot equals published_ at slot
//       acquisition — so any violation is a genuine defect (or a recorder
//       bug, which the mutation battery below distinguishes).
//   (c) ELLE-STYLE LIST-APPEND analysis, cts-independent: per key, the
//       observed token lists must contain no duplicates, no tokens that
//       no committed append wrote (fabrication / aborted-write leakage),
//       must equal the canonical prefix implied by append cts order
//       (lost updates), and the pairwise precedence graph induced by the
//       observations must be acyclic (fractured reads). Plus a write-side
//       fold check: every committed append must extend the previous
//       committed value by exactly its last token.
//
// A CHECKER THAT HAS NEVER FAILED IS NOT A CHECKER (roadmap acceptance):
// run_lincheck_test fires every violation kind below at hand-built
// synthetic histories AND at deliberate mutations of a real recorded
// engine history, and asserts the clean engine history passes.
//
// v27 M1 completion (0.26.3): the recorder gaps this checker inherited are
// CLOSED — range scans (Database + Transaction; the engine's pre-overlay
// snapshot view, wire-packed k\x1Fv\x1E*), async put/get/erase (interval
// [API entry, shared-state ready]; ack is deferred to the caller's future
// read, so ack_deferred records contribute NO real-time ack edge — the
// sound direction) and Batch commits (Stage* + Commit under one synthetic
// txn id; synchronous, full interval soundness) are all recorded and
// checked: check_scans (phantom/missing/stale vs cts replay) and
// check_set_adds (order-insensitive set algebra) join check_cts_order and
// check_list_append. v29 M2 item 1 adds check_write_skew — the multiversion
// dependency-graph (ww/wr/rw) cycle detector that closes the SSI blind spot
// Audit-2's TXN-1 demonstrated (battery: Section 1d; engine wiring:
// Sections 2/2b; mutation: Section 3b). RangeScanStream remains unrecorded (lazy multi-call
// iteration has no single sound interval; documented). Arm on an EMPTY (or
// quiesced-and-unread) database, else pre-arm state reads as fabricated.
namespace lincheck {

struct ReadEv {
    std::string key;
    bool found = false;          // a value was observed (false = absence observed)
    std::string value;
    uint64_t version_cts = 0;    // 0 = never existed; UINT64_MAX = read-your-writes buffer
};
// v27 M1 (scan modeling): one recorded range scan — the engine's snapshot
// view of [lo,hi] at `snap` (pre read-your-writes overlay for txn scans).
struct ScanEv {
    std::string lo, hi;
    uint64_t snap = 0;           // effective_ts the scan resolved versions at
    std::vector<std::pair<std::string, std::string>> entries;  // observed (k,v)
};
struct WriteEv { std::string key; std::string value; bool deleted = false; };
struct OpSeq { bool is_read; size_t idx; };   // program order within the txn

struct Txn {
    uint64_t id = 0;
    bool is_write = false;
    bool committed = true;       // read-only txns: vacuously committed
    bool has_snapshot = false;
    uint64_t snapshot = 0;       // UINT64_MAX = write-only (standalone put/erase)
    uint64_t commit_cts = 0;     // assigned cts (0 if none/aborted)
    uint64_t begin_ns = 0, end_ns = 0;   // 0 = interval not recorded
    std::vector<ReadEv> reads;
    std::vector<WriteEv> writes;
    std::vector<OpSeq> order;
    std::vector<ScanEv> scans;   // v27 M1: recorded range scans
};

struct Violation { std::string kind; std::string detail; };

inline std::vector<Txn> from_txnrec(const std::vector<txnrec::Record>& recs) {
    std::vector<Txn> out;
    std::map<uint64_t, size_t> by_id;
    uint64_t synth = UINT64_MAX;   // standalone ops get synthetic ids (count down; real ids count up from 1)
    for (const auto& r : recs) {
        size_t pos;
        if (r.txn_id != 0) {
            auto it = by_id.find(r.txn_id);
            if (it == by_id.end()) {
                pos = out.size();
                by_id.emplace(r.txn_id, pos);
                out.emplace_back();
                out.back().id = r.txn_id;
                out.back().begin_ns = r.begin_ns;
            } else pos = it->second;
        } else {
            pos = out.size();
            out.emplace_back();
            out.back().id = synth--;
            out.back().begin_ns = r.begin_ns;
            out.back().end_ns = r.end_ns;
        }
        Txn& t = out[pos];
        switch (r.op) {
        case txnrec::Op::Begin:
            t.has_snapshot = true;
            t.snapshot = r.snap_cts;
            if (t.begin_ns == 0) t.begin_ns = r.begin_ns;
            break;
        case txnrec::Op::Read: {
            if (!t.has_snapshot && r.snap_cts != UINT64_MAX) {
                t.has_snapshot = true;
                t.snapshot = r.snap_cts;
            }
            ReadEv ev;
            ev.key = r.key;
            ev.found = !r.deleted;
            ev.value = r.value;
            ev.version_cts = r.version_cts;
            t.order.push_back({true, t.reads.size()});
            t.reads.push_back(std::move(ev));
            if (r.ack_deferred) t.end_ns = 0;   // v27 M1: async get — see Write case
            break;
        }
        case txnrec::Op::Stage: {
            WriteEv w; w.key = r.key; w.value = r.value; w.deleted = r.deleted;
            t.order.push_back({false, t.writes.size()});
            t.writes.push_back(std::move(w));
            t.is_write = true;
            break;
        }
        case txnrec::Op::Write:
        case txnrec::Op::Delete: {
            WriteEv w; w.key = r.key; w.value = r.value;
            w.deleted = (r.op == txnrec::Op::Delete) || r.deleted;
            t.order.push_back({false, t.writes.size()});
            t.writes.push_back(std::move(w));
            t.is_write = true;
            t.committed = r.committed;
            t.commit_cts = r.committed ? r.commit_cts : 0;
            t.has_snapshot = true;
            t.snapshot = r.snap_cts;      // UINT64_MAX: write-only, no snapshot semantics
            // v27 M1 (async recording): end_ns marks shared-state readiness,
            // NOT the caller's ack (future.get()) — a real-time ack edge
            // derived from it would be unsound. Drop it; begin_ns stays
            // exact (API entry), so begin-side freshness checks still apply.
            if (r.ack_deferred) t.end_ns = 0;
            break;
        }
        case txnrec::Op::Commit:
            t.committed = r.committed;
            t.commit_cts = r.committed ? r.commit_cts : 0;
            t.end_ns = r.ack_deferred ? 0 : r.end_ns;   // v27 M1: see Write case
            if (!t.has_snapshot) { t.has_snapshot = true; t.snapshot = r.snap_cts; }
            break;
        case txnrec::Op::Abort:
            t.committed = false;
            t.commit_cts = 0;
            t.end_ns = r.end_ns;
            break;
        case txnrec::Op::RangeScan: {
            // v27 M1 (scan modeling): unpack lo..hi and the k\x1Fv\x1E wire
            // format (txnrec::pack_scan). A standalone scan becomes its own
            // synthetic txn whose snapshot IS the scan's effective_ts, so
            // check_cts_order's stale-start freshness applies to scans too.
            ScanEv sc;
            auto dd = r.key.find("..");
            if (dd == std::string::npos) break;   // malformed record: skip
            sc.lo = r.key.substr(0, dd);
            sc.hi = r.key.substr(dd + 2);
            sc.snap = r.snap_cts;
            size_t i = 0;
            while (i < r.value.size()) {
                size_t us = r.value.find('\x1F', i);
                size_t re = r.value.find('\x1E', i);
                if (us == std::string::npos || re == std::string::npos || us > re) break;
                sc.entries.emplace_back(r.value.substr(i, us - i),
                                        r.value.substr(us + 1, re - us - 1));
                i = re + 1;
            }
            t.scans.push_back(std::move(sc));
            if (!t.has_snapshot) { t.has_snapshot = true; t.snapshot = r.snap_cts; }
            break;
        }
        }
    }
    return out;
}

inline bool has_kind(const std::vector<Violation>& v, const char* kind) {
    for (const auto& x : v) if (x.kind == kind) return true;
    return false;
}

inline std::string describe(const std::vector<Violation>& v, size_t maxn = 3) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < maxn; ++i) {
        if (i) s += " | ";
        s += v[i].kind + ": " + v[i].detail;
    }
    if (v.size() > maxn) s += " | (+" + std::to_string(v.size() - maxn) + " more)";
    return s;
}

// rt_slack_ns: only enforce a real-time edge when the recorded intervals
// are separated by at least this much. In-library timestamps sit INSIDE
// the true call intervals (by the wrapper entry/exit overhead), so a
// zero-slack comparison could manufacture an edge the true intervals do
// not support. A few microseconds is orders of magnitude above the error
// and orders below the workload's inter-op gaps.
inline std::vector<Violation> check_cts_order(const std::vector<Txn>& txns,
                                              uint64_t rt_slack_ns = 4000) {
    std::vector<Violation> v;
    auto viol = [&](const char* kind, std::string d) {
        v.push_back(Violation{kind, std::move(d)});
    };
    auto sid = [](const Txn& t) { return std::to_string(t.id); };

    // (0) committed writers: unique nonzero cts; cts > own snapshot.
    std::vector<const Txn*> writers;
    std::map<uint64_t, const Txn*> by_cts;
    for (const auto& t : txns) {
        if (!t.is_write || !t.committed) continue;
        if (t.commit_cts == 0) {
            viol("commit-cts-missing", "txn " + sid(t) + " is a committed writer with no assigned cts");
            continue;
        }
        auto ins = by_cts.emplace(t.commit_cts, &t);
        if (!ins.second)
            viol("duplicate-commit-cts",
                 "txns " + sid(*ins.first->second) + " and " + sid(t) +
                 " both claim commit cts " + std::to_string(t.commit_cts) +
                 " — cts must be a TOTAL order");
        if (t.has_snapshot && t.snapshot != UINT64_MAX && t.commit_cts <= t.snapshot)
            viol("commit-not-after-snapshot",
                 "txn " + sid(t) + " snapshot=" + std::to_string(t.snapshot) +
                 " but commit cts=" + std::to_string(t.commit_cts) +
                 " — a txn must not order at-or-before its own snapshot");
        writers.push_back(&t);
    }
    std::sort(writers.begin(), writers.end(),
              [](const Txn* a, const Txn* b) { return a->commit_cts < b->commit_cts; });

    // (a) snapshot soundness: replay committed writes in cts order and
    //     compare every read against the state as of its snapshot.
    {
        const std::optional<std::string> ABSENT;
        std::map<std::string, std::optional<std::string>> state;
        std::vector<const Txn*> readers;
        for (const auto& t : txns)
            if (!t.reads.empty() && t.has_snapshot && t.snapshot != UINT64_MAX)
                readers.push_back(&t);
        std::sort(readers.begin(), readers.end(), [](const Txn* a, const Txn* b) {
            if (a->snapshot != b->snapshot) return a->snapshot < b->snapshot;
            return a->id < b->id;
        });
        size_t wi = 0;
        for (const Txn* t : readers) {
            while (wi < writers.size() && writers[wi]->commit_cts <= t->snapshot) {
                for (const auto& w : writers[wi]->writes)
                    state[w.key] = w.deleted ? ABSENT : std::optional<std::string>(w.value);
                wi++;
            }
            std::map<std::string, std::optional<std::string>> own_store;
            for (const auto& op : t->order) {
                if (!op.is_read) {
                    const auto& w = t->writes[op.idx];
                    own_store[w.key] = w.deleted ? ABSENT : std::optional<std::string>(w.value);
                    continue;
                }
                const auto& r = t->reads[op.idx];
                if (r.version_cts == UINT64_MAX)
                    continue;   // read-your-writes overlay hit: tautological, not a snapshot claim
                auto oit = own_store.find(r.key);
                const std::optional<std::string>& exp =
                    (oit != own_store.end()) ? oit->second
                                             : (state.count(r.key) ? state[r.key] : ABSENT);
                bool exp_found = exp.has_value();
                if (exp_found != r.found || (exp_found && *exp != r.value))
                    viol("snapshot-violation",
                         "txn " + sid(*t) + " (snapshot " + std::to_string(t->snapshot) +
                         ") read key '" + r.key + "': cts-order state says " +
                         (exp_found ? "'" + *exp + "'" : "<absent>") + ", observed " +
                         (r.found ? "'" + r.value + "'" : "<absent>") +
                         " (version cts " + std::to_string(r.version_cts) + ")");
                if (r.version_cts > t->snapshot)
                    viol("future-version-read",
                         "txn " + sid(*t) + " (snapshot " + std::to_string(t->snapshot) +
                         ") observed version cts " + std::to_string(r.version_cts) +
                         " on key '" + r.key + "'");
                if (r.found && r.version_cts == 0)
                    viol("malformed-record",
                         "txn " + sid(*t) + " read key '" + r.key +
                         "' found a value but the recorder saw version cts 0");
            }
        }
    }

    // (b) real-time order (with slack; see rt_slack_ns).
    {
        std::vector<const Txn*> ended;
        for (const Txn* w : writers)
            if (w->begin_ns && w->end_ns) ended.push_back(w);
        std::sort(ended.begin(), ended.end(),
                  [](const Txn* a, const Txn* b) { return a->end_ns < b->end_ns; });
        std::vector<uint64_t> prefmax(ended.size(), 0);
        for (size_t i = 0; i < ended.size(); ++i)
            prefmax[i] = std::max(i ? prefmax[i - 1] : 0, ended[i]->commit_cts);
        auto acked_before = [&](uint64_t begin_ns) -> uint64_t {
            if (!begin_ns || ended.empty()) return 0;
            size_t lo = 0, hi = ended.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (ended[mid]->end_ns + rt_slack_ns < begin_ns) lo = mid + 1;
                else hi = mid;
            }
            return lo ? prefmax[lo - 1] : 0;
        };
        for (const auto& t : txns) {
            if (!t.begin_ns) continue;
            uint64_t m = acked_before(t.begin_ns);
            if (!m) continue;
            if (t.is_write && t.committed && t.commit_cts && m >= t.commit_cts)
                viol("realtime-inversion",
                     "txn " + sid(t) + " (cts " + std::to_string(t.commit_cts) +
                     ") began after a write at cts " + std::to_string(m) +
                     " was acknowledged, yet orders at-or-before it — real-time order violated");
            if (t.has_snapshot && t.snapshot != UINT64_MAX && m > t.snapshot)
                viol("stale-start",
                     "txn " + sid(t) + " (snapshot " + std::to_string(t.snapshot) +
                     ") began after the write at cts " + std::to_string(m) +
                     " was acknowledged, but its snapshot does not include it");
        }
    }
    return v;
}

inline std::vector<std::string> split_tokens(const std::string& s, char sep) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t i = 0;
    while (true) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// Elle-style list-append checker (roadmap v27 M1, item 2). Workload
// convention: every value is an append-only token list joined by `sep`;
// an append transaction reads the current list and writes list+sep+token
// with a GLOBALLY UNIQUE token, so the appended token is the LAST element
// of the written value. Under that convention:
//   * the canonical per-key token order is the appends' commit-cts order;
//   * any observed list must equal the canonical prefix of appends with
//     cts <= the observer's snapshot (catches lost updates and stale
//     fabrications with an exact expected value);
//   * observed lists must not repeat or invent tokens;
//   * the precedence graph induced by the observations must be acyclic —
//     this check is CTS-INDEPENDENT (the Elle part): it would catch an
//     order anomaly even if the cts bookkeeping itself were the bug;
//   * the committed appends must fold: each write extends the previous
//     committed value by exactly its last token (write-side lost update).
inline std::vector<Violation> check_list_append(const std::vector<Txn>& txns, char sep = '+') {
    std::vector<Violation> v;
    auto viol = [&](const char* kind, std::string d) {
        v.push_back(Violation{kind, std::move(d)});
    };
    struct Append { uint64_t cts; std::string token; std::string value; const Txn* txn; };
    std::map<std::string, std::vector<Append>> appends;
    std::map<std::string, std::set<std::string>> known;
    std::map<std::string, std::vector<std::pair<const Txn*, const ReadEv*>>> obs;
    for (const auto& t : txns) {
        if (t.is_write && t.committed && t.commit_cts) {
            for (const auto& w : t.writes) {
                if (w.deleted) continue;
                auto toks = split_tokens(w.value, sep);
                if (toks.empty()) continue;
                appends[w.key].push_back(Append{t.commit_cts, toks.back(), w.value, &t});
                known[w.key].insert(toks.begin(), toks.end());
            }
        }
        for (const auto& r : t.reads) {
            if (r.version_cts == UINT64_MAX) continue;   // read-your-writes: not committed state
            if (!r.found) continue;                       // absence carries no list content
            obs[r.key].emplace_back(&t, &r);
        }
    }
    for (auto& [key, ap] : appends)
        std::sort(ap.begin(), ap.end(),
                  [](const Append& a, const Append& b) { return a.cts < b.cts; });

    static const std::vector<Append> NO_APPENDS;
    for (const auto& [key, ob] : obs) {
        auto ait = appends.find(key);
        const std::vector<Append>& ap = (ait == appends.end()) ? NO_APPENDS : ait->second;
        auto kit = known.find(key);
        for (const auto& [t, r] : ob) {
            auto toks = split_tokens(r->value, sep);
            std::set<std::string> uniq(toks.begin(), toks.end());
            if (uniq.size() != toks.size())
                viol("duplicate-token",
                     "txn " + std::to_string(t->id) + " read key '" + key +
                     "' with a repeated token: '" + r->value + "'");
            if (kit != known.end())
                for (const auto& tk : uniq)
                    if (!kit->second.count(tk))
                        viol("unknown-token",
                             "txn " + std::to_string(t->id) + " observed token '" + tk +
                             "' on key '" + key +
                             "' that no committed append wrote (fabrication or aborted-write leak)");
            if (!t->has_snapshot || t->snapshot == UINT64_MAX) continue;
            std::vector<std::string> expect;
            for (const auto& a : ap)
                if (a.cts <= t->snapshot) expect.push_back(a.token);
            if (toks == expect) continue;
            bool missing = false;
            for (const auto& e : expect)
                if (!uniq.count(e)) { missing = true; break; }
            std::string exp_s, got_s = r->value;
            for (size_t i = 0; i < expect.size(); ++i) { if (i) exp_s += sep; exp_s += expect[i]; }
            if (missing)
                viol("lost-append",
                     "txn " + std::to_string(t->id) + " (snapshot " + std::to_string(t->snapshot) +
                     ") read key '" + key + "' as '" + got_s + "' but appends committed at cts <= snapshot require '" +
                     exp_s + "' — an acknowledged append was lost");
            else
                viol("fractured-read",
                     "txn " + std::to_string(t->id) + " (snapshot " + std::to_string(t->snapshot) +
                     ") read key '" + key + "' as '" + got_s + "' but the appends' cts order requires '" +
                     exp_s + "' — tokens present in the wrong order");
        }
    }

    // Write-side fold: each committed append must extend the previous
    // committed value by exactly its last token.
    for (const auto& [key, ap] : appends) {
        std::string fold;
        for (const auto& a : ap) {
            std::string expect = fold.empty() ? a.token : fold + sep + a.token;
            if (a.value != expect)
                viol("append-fold-mismatch",
                     "committed write to '" + key + "' at cts " + std::to_string(a.cts) +
                     " (txn " + std::to_string(a.txn->id) + ") wrote '" + a.value +
                     "' but folding the prior committed appends requires '" + expect +
                     "' — the writer did not extend the latest committed state (lost update at the write level)");
            fold = expect;
        }
    }

    // Cts-independent precedence-cycle check (Elle G-single flavor):
    // adjacency edges from every observed list; a cycle means two reads
    // disagree on the order of two appends — no total order exists.
    for (const auto& [key, ob] : obs) {
        std::map<std::string, std::set<std::string>> edges;
        for (const auto& [t, r] : ob) {
            auto toks = split_tokens(r->value, sep);
            for (size_t i = 0; i + 1 < toks.size(); ++i)
                edges[toks[i]].insert(toks[i + 1]);
        }
        struct Dfs {
            const std::map<std::string, std::set<std::string>>& e;
            std::map<std::string, int> color;   // 0/absent = unvisited, 1 = on stack, 2 = done
            bool cycle = false;
            std::string witness;
            void go(const std::string& n) {
                if (cycle) return;
                color[n] = 1;
                auto it = e.find(n);
                if (it != e.end())
                    for (const auto& m : it->second) {
                        int c = color.count(m) ? color[m] : 0;
                        if (c == 1) { cycle = true; witness = n + " -> " + m + " -> ... -> " + n; return; }
                        if (c == 0) go(m);
                    }
                color[n] = 2;
            }
        } dfs{edges};
        for (const auto& [n, _] : edges)
            if (!(dfs.color.count(n) ? dfs.color[n] : 0)) dfs.go(n);
        if (dfs.cycle)
            viol("order-cycle",
                 "observations of key '" + key +
                 "' induce a precedence cycle (" + dfs.witness +
                 ") — no total append order is consistent with the reads (fractured reads)");
    }
    return v;
}

// v27 M1 (range-scan modeling): verify every recorded scan against the
// cts replay. A scan over [lo,hi] at effective snapshot S must return
// EXACTLY the keys live in [lo,hi] in the state produced by the committed
// writes with cts <= S, with exactly their values:
//   * extra key           -> scan-phantom        (SSI phantom intrusion)
//   * missing live key    -> scan-missing-key    (lost entry / cursor skip)
//   * wrong value         -> scan-stale-value    (stale or corrupt read)
//   * key outside bounds  -> scan-out-of-bounds  (cursor over-run)
//   * repeated key        -> scan-duplicate-key  (cursor double-visit)
// Real-time freshness for scans (an acked write absent from a later scan's
// snapshot) is enforced by check_cts_order's stale-start rule: a recorded
// scan becomes a synthetic txn whose snapshot IS its effective_ts.
inline std::vector<Violation> check_scans(const std::vector<Txn>& txns) {
    std::vector<Violation> v;
    auto viol = [&](const char* kind, std::string d) {
        v.push_back(Violation{kind, std::move(d)});
    };
    const std::optional<std::string> ABSENT;
    std::vector<const Txn*> writers;
    for (const auto& t : txns)
        if (t.is_write && t.committed && t.commit_cts) writers.push_back(&t);
    std::sort(writers.begin(), writers.end(), [](const Txn* a, const Txn* b) {
        if (a->commit_cts != b->commit_cts) return a->commit_cts < b->commit_cts;
        return a->id < b->id;
    });
    struct ScanRef { const Txn* t; const ScanEv* s; };
    std::vector<ScanRef> scans;
    for (const auto& t : txns)
        for (const auto& sc : t.scans) scans.push_back(ScanRef{&t, &sc});
    if (scans.empty()) return v;
    std::sort(scans.begin(), scans.end(), [](const ScanRef& a, const ScanRef& b) {
        if (a.s->snap != b.s->snap) return a.s->snap < b.s->snap;
        return a.t->id < b.t->id;
    });
    std::map<std::string, std::optional<std::string>> state;
    size_t wi = 0;
    for (const auto& sr : scans) {
        while (wi < writers.size() && writers[wi]->commit_cts <= sr.s->snap) {
            for (const auto& w : writers[wi]->writes)
                state[w.key] = w.deleted ? ABSENT : std::optional<std::string>(w.value);
            wi++;
        }
        std::map<std::string, std::string> expect;
        for (const auto& kv : state)
            if (kv.second && kv.first >= sr.s->lo && kv.first <= sr.s->hi)
                expect[kv.first] = *kv.second;
        std::set<std::string> seen;
        const std::string tag = "txn " + std::to_string(sr.t->id) + " scan [" +
                                sr.s->lo + ".." + sr.s->hi + "] @snap " +
                                std::to_string(sr.s->snap);
        for (const auto& kv : sr.s->entries) {
            if (!seen.insert(kv.first).second) {
                viol("scan-duplicate-key", tag + " returned key '" + kv.first + "' twice");
                continue;
            }
            if (kv.first < sr.s->lo || kv.first > sr.s->hi) {
                viol("scan-out-of-bounds", tag + " returned key '" + kv.first + "' outside bounds");
                continue;
            }
            auto it = expect.find(kv.first);
            if (it == expect.end())
                viol("scan-phantom", tag + " returned key '" + kv.first +
                     "' not live at its snapshot (phantom)");
            else if (it->second != kv.second)
                viol("scan-stale-value", tag + " key '" + kv.first + "': cts-order state says '" +
                     it->second + "', scan observed '" + kv.second + "'");
        }
        for (const auto& kv : expect)
            if (!seen.count(kv.first))
                viol("scan-missing-key", tag + " omitted live key '" + kv.first +
                     "' (value '" + kv.second + "')");
    }
    return v;
}

// v27 M1 (set-checker shape, roadmap item): the list-append checker treats
// token ORDER as canonical; set semantics must not. Workload convention:
// a set key's value is a sep-joined collection of globally unique tokens;
// an add transaction reads the set and writes it back with exactly one new
// token appended (the wire form looks append-like, but the CHECKER's algebra
// is order-insensitive). Checks, per key with the given prefix:
//   * observed value repeats a token            -> set-duplicate
//   * observed token no committed add ever wrote-> set-fabricated
//   * observed set != canonical set at snapshot -> set-snapshot-mismatch
//     (canonical(key,S) = value of the newest committed add with cts <= S;
//      catches lost adds and stale sets without consulting order)
//   * committed add's value != prev ∪ {token}   -> set-write-fold
//     (write-side lost update; token already present -> set-add-duplicate)
//   * add ACKED before a reader began, token    -> set-realtime-loss
//     absent from that reader's observed set       (cts-independent; only
//     sound-interval writes participate — ack_deferred ends are zeroed)
// A PERMUTED token order is explicitly NOT a violation — that is the whole
// point of the set shape, and the battery asserts the silence.
inline std::vector<Violation> check_set_adds(const std::vector<Txn>& txns,
                                             const std::string& prefix,
                                             char sep = '+',
                                             uint64_t rt_slack_ns = 4000) {
    std::vector<Violation> v;
    auto viol = [&](const char* kind, std::string d) {
        v.push_back(Violation{kind, std::move(d)});
    };
    auto has_prefix = [&](const std::string& k) {
        return k.size() >= prefix.size() && k.compare(0, prefix.size(), prefix) == 0;
    };
    struct Add { uint64_t cts; std::string token; std::set<std::string> value;
                 uint64_t end_ns; const Txn* txn; };
    std::map<std::string, std::vector<Add>> adds;
    std::map<std::string, std::set<std::string>> known;
    for (const auto& t : txns) {
        if (!(t.is_write && t.committed && t.commit_cts)) continue;
        for (const auto& w : t.writes) {
            if (w.deleted || !has_prefix(w.key)) continue;
            auto toks = split_tokens(w.value, sep);
            std::set<std::string> sset(toks.begin(), toks.end());
            if (sset.size() != toks.size())
                viol("set-write-duplicate",
                     "committed add txn " + std::to_string(t.id) + " wrote duplicate tokens to '" +
                     w.key + "': '" + w.value + "'");
            known[w.key].insert(toks.begin(), toks.end());
            adds[w.key].push_back(Add{t.commit_cts, toks.empty() ? std::string() : toks.back(),
                                      sset, t.end_ns, &t});
        }
    }
    for (auto& kv : adds) {
        auto& vec = kv.second;
        std::sort(vec.begin(), vec.end(), [](const Add& a, const Add& b) {
            if (a.cts != b.cts) return a.cts < b.cts;
            return a.txn->id < b.txn->id;
        });
        std::set<std::string> acc;
        for (const auto& a : vec) {
            if (!a.token.empty() && acc.count(a.token))
                viol("set-add-duplicate",
                     "add txn " + std::to_string(a.txn->id) + " re-added existing token '" +
                     a.token + "' to '" + kv.first + "'");
            std::set<std::string> expect = acc;
            expect.insert(a.token);
            if (a.value != expect)
                viol("set-write-fold",
                     "add txn " + std::to_string(a.txn->id) + " on '" + kv.first +
                     "': committed value != prior set + its token (write-side lost/garbled add)");
            acc = a.value;   // canonical state follows what ACTUALLY committed
        }
    }
    auto canonical_at = [&](const std::string& key, uint64_t snap) {
        std::set<std::string> out;
        auto it = adds.find(key);
        if (it == adds.end()) return out;
        for (const auto& a : it->second)      // cts-sorted
            if (a.cts <= snap) out = a.value;
            else break;
        return out;
    };
    // readers
    for (const auto& t : txns) {
        for (const auto& r : t.reads) {
            if (!has_prefix(r.key) || r.version_cts == UINT64_MAX) continue;
            auto toks = split_tokens(r.value, sep);
            std::set<std::string> obs;
            bool dup = false;
            for (const auto& tk : toks)
                if (!obs.insert(tk).second) dup = true;
            if (dup)
                viol("set-duplicate",
                     "txn " + std::to_string(t.id) + " read of '" + r.key +
                     "' repeated a token: '" + r.value + "'");
            for (const auto& tk : obs)
                if (!known[r.key].count(tk))
                    viol("set-fabricated",
                         "txn " + std::to_string(t.id) + " read of '" + r.key +
                         "' returned token '" + tk + "' no committed add ever wrote");
            if (t.has_snapshot && t.snapshot != UINT64_MAX && r.found) {
                auto expect = canonical_at(r.key, t.snapshot);
                if (obs != expect) {
                    std::string missing, extra;
                    for (const auto& e : expect) if (!obs.count(e)) missing += (missing.empty() ? "" : ",") + e;
                    for (const auto& o : obs) if (!expect.count(o)) extra += (extra.empty() ? "" : ",") + o;
                    viol("set-snapshot-mismatch",
                         "txn " + std::to_string(t.id) + " (snapshot " + std::to_string(t.snapshot) +
                         ") read '" + r.key + "': canonical set {" +
                         [&]{ std::string j; for (const auto& e : expect) j += (j.empty()?"":",") + e; return j; }() +
                         "}, observed {" + [&]{ std::string j; for (const auto& o : obs) j += (j.empty()?"":",") + o; return j; }() +
                         "}" + (missing.empty() ? "" : " missing=" + missing) +
                         (extra.empty() ? "" : " extra=" + extra));
                }
            }
            if (r.found && t.begin_ns) {
                // cts-independent real-time: adds acked before this read began
                // must be present. Only sound-interval (end_ns != 0) adds count.
                for (const auto& a : (adds.count(r.key) ? adds[r.key] : std::vector<Add>{})) {
                    if (a.end_ns && a.end_ns + rt_slack_ns < t.begin_ns && !obs.count(a.token))
                        viol("set-realtime-loss",
                             "txn " + std::to_string(t.id) + " began after add '" + a.token +
                             "' (txn " + std::to_string(a.txn->id) + ") was acknowledged, but its read of '" +
                             r.key + "' does not contain it");
                }
            }
        }
    }
    return v;
}


// v29 M2 item 1 (roadmap): the SSI anti-dependency ("write-skew") checker
// — lincheck's DEMONSTRATED blind spot. Audit-2's TXN-1 was found by an
// audit round, not by this suite: every pre-existing checker validates
// view consistency (reads match their snapshot) and real-time order, and
// the TXN-1 history satisfies both — it is snapshot-consistent and still
// not serializable. What was missing is the multiversion dependency graph:
//
//   ww  earlier writer of a key -> later writer (version-chain order)
//   wr  writer of an observed version -> the reader that observed it
//   rw  reader/scaner -> every committed writer whose key is the read's key
//       (or falls inside the scan's range) at a cts ABOVE the reader's
//       snapshot: the read missed that write, so the reader must serialize
//       BEFORE the writer (anti-dependency — the edge class SI checkers
//       do not see)
//
// A committed history is serializable only if this graph is acyclic; a
// cycle IS the anomaly (write-skew, lost update, phantom-skew). Scope
// discipline: writes with cts inside (observed_version, snapshot] are
// check_cts_order's snapshot-soundness class and are deliberately NOT
// edges here — each checker owns exactly one violation class. Aborted
// txns contribute nothing (their writes never exist). Write-only (blind)
// txns have no reads: they receive rw edges and chain via ww but cannot
// close a cycle by themselves — mirroring why the engine's entry-lock FUW
// plus phantom-tracker range validation jointly suffice. Reads/scans with
// no recorded snapshot (UINT64_MAX) are skipped: sound under-detection,
// documented. Acceptance per roadmap M2 item 1: the TXN-1 PoC shape
// (battery W2 below) and every synthetic anomaly are flagged; the serial
// and disjoint controls and every clean engine history pass; the pre-fix
// engine's LIVE recorded PoC history is flagged and the post-fix engine's
// is clean (demonstrated in the 0.28.1 confirmation record).
inline std::vector<Violation> check_write_skew(const std::vector<Txn>& txns) {
    std::vector<Violation> v;
    auto viol = [&](std::string d) { v.push_back(Violation{"anti-dependency-cycle", std::move(d)}); };

    std::vector<const Txn*> ct;
    for (const auto& t : txns) if (t.committed) ct.push_back(&t);
    const size_t n = ct.size();
    if (n < 2) return v;

    std::map<std::string, std::vector<std::pair<uint64_t, size_t>>> wbyk;  // key -> (cts, idx), sorted
    for (size_t i = 0; i < n; ++i) {
        if (!ct[i]->commit_cts) continue;
        for (const auto& w : ct[i]->writes)
            wbyk[w.key].emplace_back(ct[i]->commit_cts, i);
    }
    for (auto& kv : wbyk) std::sort(kv.second.begin(), kv.second.end());

    std::vector<std::vector<std::pair<size_t, std::string>>> adj(n);
    std::set<std::pair<size_t, size_t>> seen;
    auto edge = [&](size_t a, size_t b, std::string why) {
        if (a == b || !seen.emplace(a, b).second) return;
        adj[a].push_back({b, std::move(why)});
    };
    auto tid = [&](size_t i) { return "T" + std::to_string(ct[i]->id); };

    // ww: version-chain order per key.
    for (const auto& kv : wbyk) {
        const auto& vec = kv.second;
        for (size_t j = 1; j < vec.size(); ++j)
            edge(vec[j - 1].second, vec[j].second, "ww '" + kv.first + "'");
    }

    // wr + rw (point reads and scans).
    for (size_t i = 0; i < n; ++i) {
        const Txn& t = *ct[i];
        if (!t.has_snapshot || t.snapshot == UINT64_MAX) continue;
        for (const auto& r : t.reads) {
            if (r.version_cts == UINT64_MAX) continue;   // read-your-writes overlay: tautological
            auto wit = wbyk.find(r.key);
            if (wit == wbyk.end()) continue;
            for (const auto& w : wit->second) {
                if (w.first == r.version_cts)
                    edge(w.second, i, "wr '" + r.key + "'@" + std::to_string(w.first));
                else if (w.first > t.snapshot)
                    edge(i, w.second, "rw '" + r.key + "': read@" + std::to_string(r.version_cts) +
                                      " (snap " + std::to_string(t.snapshot) + ") missed write@" +
                                      std::to_string(w.first));
            }
        }
        for (const auto& sc : t.scans) {
            if (sc.snap == UINT64_MAX) continue;
            for (const auto& kv : wbyk) {
                if (kv.first < sc.lo || kv.first > sc.hi) continue;
                for (const auto& w : kv.second)
                    if (w.first > sc.snap)
                        edge(i, w.second, "rw scan[" + sc.lo + "," + sc.hi + "]@snap " +
                                          std::to_string(sc.snap) + " missed '" + kv.first + "'@" +
                                          std::to_string(w.first));
            }
        }
    }

    // Cycle detection: colored DFS, first 5 distinct cycles reported.
    std::vector<int> color(n, 0);
    std::vector<size_t> path;
    int reported = 0;
    auto report_cycle = [&](const std::vector<size_t>& cyc) {
        std::string d = "committed history is not serializable: ";
        for (size_t q = 0; q < cyc.size(); ++q) {
            size_t a = cyc[q], b = cyc[(q + 1) % cyc.size()];
            std::string why;
            for (const auto& e : adj[a]) if (e.first == b) { why = e.second; break; }
            d += tid(a) + " -[" + why + "]-> ";
        }
        d += tid(cyc.front());
        viol(std::move(d));
    };
    std::function<void(size_t)> dfs = [&](size_t u) {
        if (reported >= 5) return;
        color[u] = 1;
        path.push_back(u);
        for (const auto& e : adj[u]) {
            if (reported >= 5) break;
            if (color[e.first] == 1) {
                auto it = std::find(path.begin(), path.end(), e.first);
                report_cycle(std::vector<size_t>(it, path.end()));
                ++reported;
            } else if (color[e.first] == 0) {
                dfs(e.first);
            }
        }
        path.pop_back();
        color[u] = 2;
    };
    for (size_t s = 0; s < n && reported < 5; ++s)
        if (color[s] == 0) dfs(s);
    return v;
}

}   // namespace lincheck

// =====================================================================
// v27 M1 test driver: proves the checker works before trusting it.
//
// Section 1 — SYNTHETIC battery: a hand-built clean history must pass
//             with zero violations, and every injected anomaly (one per
//             violation kind, plus combinations) must be flagged — since
//             0.26.3 including the SCAN kinds (phantom / missing / stale /
//             bounds / duplicate) and the SET kinds (duplicate / fabricated
//             / snapshot-mismatch / write-fold / add-duplicate / realtime-
//             loss, plus the asserted SILENCE on permuted token order).
//             Roadmap acceptance: "The checker must flag a deliberately
//             injected anomaly in a synthetic history. A checker that has
//             never failed is not a checker."
// Section 2 — ENGINE workload: concurrent list-append transactions +
//             readers against a real Database, recorded via txnrec; the
//             checkers must report ZERO violations (with non-vacuity
//             guards: the history must actually contain the workload).
// Section 2b — MIXED-API engine workload (0.26.3): set-add transactions,
//             async put/get with prompt future reads, Batch multi-key
//             commits, standalone + transactional range scans over the
//             churned keyspace; checked by ALL FOUR checkers.
// Section 3 — ENGINE-history mutations: deliberate corruptions of the
//             RECORDED history (dropped token, duplicated token, swapped
//             commit cts across a real-time edge, future version cts)
//             must be flagged — the checker bites engine-shaped data too.
// Section 3b — MUTATIONS of the mixed-API history: a dropped scan entry,
//             an injected scan phantom, a duplicated set token, and a
//             batch-txn cts swap across a real-time edge.
// =====================================================================
int run_lincheck_test() {
    using namespace chronokv;
    int fails = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail = "") {
        std::cout << "   " << name << ":  " << (ok ? "PASS" : "FAIL") << "\n";
        if (!ok) {
            ++fails;
            if (!detail.empty()) std::cout << "      (" << detail << ")\n";
        }
    };

    // ---- Section 0: barrier regression — append-only open on an existing
    // WAL must be able to commit. The publication barrier waits for the
    // contiguous prefix to cover the commit's cts; an append-only open
    // (recover_on_open=false, or direct engine construction over a
    // non-empty wal_dir) never replays cts 1..N, so the prefix must be
    // SEEDED to N at construction or the first commit waits forever on an
    // untracked hole. Found by the full-suite verification of the barrier
    // fix (the v20.1-#7 engine hung pre-fix). Runs on a worker with a hard
    // timeout so a regression FAILS instead of wedging the suite.
    {
        const std::string wd = "/tmp/ckv_lincheck_appendonly";
        std::filesystem::remove_all(wd);
        {
            Options ao;
            ao.wal_dir = wd;
            ao.durability = DurabilityMode::Group;
            auto db = Database::open(ao);
            db.put("x", "1");
            db.close();
        }
        Options o2;
        o2.wal_dir = wd;
        o2.recover_on_open = false;      // append-only: clock seeded, no replay
        o2.durability = DurabilityMode::Group;
        // Heap + deliberate leak on the failure path: the worker below may
        // be stuck INSIDE db2->put forever (that is the regression), so the
        // Database must outlive this scope, and the signaling future must
        // NOT be a std::async future — ~future() of std::async JOINS the
        // task, which would re-create the very hang this test detects.
        auto* db2 = new Database(Database::open(o2));
        auto pr = std::make_shared<std::promise<Status>>();
        auto fut = pr->get_future();
        std::thread([db2, pr] { pr->set_value(db2->put("y", "2")); }).detach();
        bool done = fut.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
        bool ok = done && fut.get() == Status::OK;
        check("lincheck: append-only open commits under the publication barrier", ok,
              done ? "put did not return Status::OK"
                   : "commit HUNG: published prefix not seeded past the existing WAL");
        if (done) {
            db2->close();
            delete db2;
            std::filesystem::remove_all(wd);
        }
        // else: db2 and its directory are intentionally leaked — a hung
        // worker still references both. The suite reports FAIL and moves on.
    }

    // ---- builders for synthetic histories ----
    auto mkw = [](uint64_t id, uint64_t snap, uint64_t cts, uint64_t b, uint64_t e,
                  std::vector<std::pair<std::string, std::string>> ws) {
        lincheck::Txn t;
        t.id = id; t.is_write = true; t.committed = true;
        t.has_snapshot = true; t.snapshot = snap; t.commit_cts = cts;
        t.begin_ns = b; t.end_ns = e;
        for (auto& [k, val] : ws) {
            t.order.push_back({false, t.writes.size()});
            t.writes.push_back(lincheck::WriteEv{k, val, false});
        }
        return t;
    };
    auto mkr = [](uint64_t id, uint64_t snap, uint64_t b, uint64_t e,
                  std::vector<std::tuple<std::string, bool, std::string, uint64_t>> rs) {
        lincheck::Txn t;
        t.id = id; t.is_write = false; t.committed = true;
        t.has_snapshot = true; t.snapshot = snap; t.begin_ns = b; t.end_ns = e;
        for (auto& [k, found, val, vcts] : rs) {
            t.order.push_back({true, t.reads.size()});
            t.reads.push_back(lincheck::ReadEv{k, found, val, vcts});
        }
        return t;
    };
    // Clean baseline (times in ns, microsecond scale so the 4us real-time
    // slack is far below every interval gap):
    //   W1@cts1 a=[t1] | W2@cts2 b=[u1] | W3@cts3 a=[t1,t2]
    //   R4(snap2) a=[t1] b=[u1] | R5(snap1) a=[t1] b=absent | R6(snap3) a=[t1,t2] b=[u1]
    auto base_hist = [&]() {
        std::vector<lincheck::Txn> h;
        h.push_back(mkw(1, 0, 1, 1000000, 2000000, {{"a", "t1"}}));
        h.push_back(mkw(2, 1, 2, 3000000, 4000000, {{"b", "u1"}}));
        h.push_back(mkw(3, 2, 3, 7000000, 8000000, {{"a", "t1+t2"}}));
        h.push_back(mkr(4, 2, 5000000, 6000000,
                        {{"a", true, "t1", 1}, {"b", true, "u1", 2}}));
        h.push_back(mkr(5, 1, 2200000, 2800000,
                        {{"a", true, "t1", 1}, {"b", false, "", 0}}));
        h.push_back(mkr(6, 3, 9000000, 9500000,
                        {{"a", true, "t1+t2", 3}, {"b", true, "u1", 2}}));
        return h;
    };
    auto both = [](const std::vector<lincheck::Txn>& h) {
        auto v = lincheck::check_cts_order(h);
        auto w = lincheck::check_list_append(h);
        v.insert(v.end(), w.begin(), w.end());
        return v;
    };

    // ---- Section 1: clean baseline ----
    {
        auto h = base_hist();
        auto v = both(h);
        check("lincheck: synthetic clean history passes both checkers",
              v.empty(), lincheck::describe(v));
    }

    // ---- Section 1: injected anomalies (each must be flagged) ----
    struct Case { const char* name; const char* expect_kind; const char* absent_kind;
                  std::function<void(std::vector<lincheck::Txn>&)> mutate; };
    std::vector<Case> cases = {
        {"snapshot reads a committed write it should not see", "snapshot-violation", "",
         [](std::vector<lincheck::Txn>& h) {   // R4 misses b=u1 at snap 2
             h[3].reads[1].found = false; h[3].reads[1].value = ""; h[3].reads[1].version_cts = 0;
         }},
        {"read observes a version newer than its snapshot", "future-version-read", "",
         [](std::vector<lincheck::Txn>& h) {   // R5(snap1) sees b=u1@cts2
             h[4].reads[1] = lincheck::ReadEv{"b", true, "u1", 2};
         }},
        {"write acknowledged before another began orders after it", "realtime-inversion", "",
         [](std::vector<lincheck::Txn>& h) {   // W3 runs [0.5us,0.9us], before W1
             h[2].begin_ns = 500000; h[2].end_ns = 900000;
         }},
        {"txn starts after an ack its snapshot does not include", "stale-start", "snapshot-violation",
         [](std::vector<lincheck::Txn>& h) {   // R6 begins at 9us with snap 2 (W3 acked at 8us)
             h[5].snapshot = 2;
             h[5].reads[0] = lincheck::ReadEv{"a", true, "t1", 1};
         }},
        {"two committed writers claim the same cts", "duplicate-commit-cts", "",
         [](std::vector<lincheck::Txn>& h) { h[2].commit_cts = 2; }},
        {"commit cts at-or-before the txn's own snapshot", "commit-not-after-snapshot", "",
         [](std::vector<lincheck::Txn>& h) { h[1].snapshot = 5; }},
        {"observed list repeats a token", "duplicate-token", "",
         [](std::vector<lincheck::Txn>& h) {
             h[5].reads[0] = lincheck::ReadEv{"a", true, "t1+t1", 3};
         }},
        {"acknowledged append missing from a later read", "lost-append", "",
         [](std::vector<lincheck::Txn>& h) {
             h[5].reads[0] = lincheck::ReadEv{"a", true, "t1", 1};
         }},
        {"read returns a token no committed append wrote", "unknown-token", "",
         [](std::vector<lincheck::Txn>& h) {
             h[5].reads[0] = lincheck::ReadEv{"a", true, "t1+tX", 3};
         }},
        {"two reads disagree on append order (cycle)", "order-cycle", "",
         [](std::vector<lincheck::Txn>& h) {
             h[3].reads[0] = lincheck::ReadEv{"a", true, "t1+t2", 1};   // t1 before t2
             h[5].reads[0] = lincheck::ReadEv{"a", true, "t2+t1", 3};   // t2 before t1
         }},
        {"read orders tokens against the cts order", "fractured-read", "",
         [](std::vector<lincheck::Txn>& h) {
             h[5].reads[0] = lincheck::ReadEv{"a", true, "t2+t1", 3};
         }},
        {"committed append does not extend the prior state", "append-fold-mismatch", "",
         [](std::vector<lincheck::Txn>& h) { h[2].writes[0].value = "tX+t2"; }},
        {"committed writer carries no cts", "commit-cts-missing", "",
         [](std::vector<lincheck::Txn>& h) { h[1].commit_cts = 0; }},
        {"aborted write's value leaks into a read", "unknown-token", "order-cycle",
         [&](std::vector<lincheck::Txn>& h) {
             lincheck::Txn w;
             w.id = 7; w.is_write = true; w.committed = false; w.commit_cts = 0;
             w.has_snapshot = true; w.snapshot = 3;
             w.begin_ns = 10000000; w.end_ns = 11000000;
             w.order.push_back({false, 0});
             w.writes.push_back(lincheck::WriteEv{"a", "t1+t2+t7", false});
             h.push_back(std::move(w));
             h.push_back(mkr(8, 3, 12000000, 13000000,
                             {{"a", true, "t1+t2+t7", 3}}));
         }},
    };
    for (auto& c : cases) {
        auto h = base_hist();
        c.mutate(h);
        auto v = both(h);
        bool flagged = lincheck::has_kind(v, c.expect_kind);
        bool clean_absent = true;
        if (c.absent_kind[0]) clean_absent = !lincheck::has_kind(v, c.absent_kind);
        std::string nm = std::string("lincheck: synthetic anomaly flagged — ") + c.name;
        check(nm.c_str(), flagged && clean_absent,
              flagged ? (std::string("unexpected extra kind ") + c.absent_kind)
                      : ("expected kind not flagged; got: " + (v.empty() ? "<none>" : lincheck::describe(v))));
    }

    // ---- Section 1b: synthetic SCAN battery (v27 M1 completion) ----
    // Clean: a=a1@cts1, b=b1@cts2, a=a2@cts3; scans over [a..b] at snap 1/2/3
    // must see exactly the live prefix. Then one injected anomaly per
    // scan violation kind.
    {
        auto scan_hist = [&]() {
            std::vector<lincheck::Txn> h;
            h.push_back(mkw(1, 0, 1, 1000000, 2000000, {{"a", "a1"}}));
            h.push_back(mkw(2, 1, 2, 3000000, 4000000, {{"b", "b1"}}));
            h.push_back(mkw(3, 2, 3, 7000000, 8000000, {{"a", "a2"}}));
            auto mk = [&](uint64_t id, uint64_t snap, uint64_t b, uint64_t e,
                          std::vector<std::pair<std::string, std::string>> ents) {
                lincheck::Txn t;
                t.id = id; t.committed = true;
                t.has_snapshot = true; t.snapshot = snap;
                t.begin_ns = b; t.end_ns = e;
                lincheck::ScanEv sc; sc.lo = "a"; sc.hi = "b"; sc.snap = snap;
                sc.entries = std::move(ents);
                t.scans.push_back(std::move(sc));
                return t;
            };
            h.push_back(mk(4, 1, 2200000, 2400000, {{"a", "a1"}}));
            h.push_back(mk(5, 2, 5000000, 5200000, {{"a", "a1"}, {"b", "b1"}}));
            h.push_back(mk(6, 3, 9000000, 9200000, {{"a", "a2"}, {"b", "b1"}}));
            return h;
        };
        {
            auto v = lincheck::check_scans(scan_hist());
            check("lincheck: synthetic clean scan history passes check_scans",
                  v.empty(), lincheck::describe(v));
        }
        struct ScanCase { const char* name; const char* kind;
                          std::function<void(std::vector<lincheck::Txn>&)> mutate; };
        std::vector<ScanCase> scases = {
            {"scan returns a key not live at its snapshot (phantom)", "scan-phantom",
             [](std::vector<lincheck::Txn>& h) {   // snap-1 scan sees b (committed @2)
                 h[3].scans[0].entries.push_back({"b", "b1"});
             }},
            {"scan omits a live key", "scan-missing-key",
             [](std::vector<lincheck::Txn>& h) {   // snap-2 scan drops b
                 h[4].scans[0].entries.erase(h[4].scans[0].entries.begin() + 1);
             }},
            {"scan observes a stale value", "scan-stale-value",
             [](std::vector<lincheck::Txn>& h) {   // snap-3 scan sees a1, not a2
                 h[5].scans[0].entries[0].second = "a1";
             }},
            {"scan returns a key outside its bounds", "scan-out-of-bounds",
             [](std::vector<lincheck::Txn>& h) {
                 h[5].scans[0].entries.push_back({"z", "zz"});
             }},
            {"scan returns the same key twice", "scan-duplicate-key",
             [](std::vector<lincheck::Txn>& h) {
                 h[5].scans[0].entries.push_back({"a", "a2"});
             }},
        };
        for (auto& c : scases) {
            auto h = scan_hist();
            c.mutate(h);
            auto v = lincheck::check_scans(h);
            bool ok = lincheck::has_kind(v, c.kind);
            std::string nm = std::string("lincheck: synthetic scan anomaly flagged — ") + c.name;
            check(nm.c_str(), ok,
                  ok ? "" : ("expected " + std::string(c.kind) + "; got: " +
                             (v.empty() ? "<none>" : lincheck::describe(v))));
        }
        // Scan freshness rides on check_cts_order's stale-start rule: the
        // snap-2 scan re-timed to BEGIN after W3@cts3 was acknowledged must
        // be flagged even though check_scans alone is satisfied (its entries
        // still match snap 2 — the snapshot itself is stale in real time).
        {
            auto h = scan_hist();
            h[4].begin_ns = 8500000; h[4].end_ns = 8600000;
            auto v = lincheck::check_cts_order(h);
            check("lincheck: scan begun after an ack its snapshot misses is flagged (stale-start)",
                  lincheck::has_kind(v, "stale-start"), lincheck::describe(v));
        }
    }

    // ---- Section 1c: synthetic SET battery (v27 M1 completion) ----
    // Clean: S={x1}@cts1, S={x1,x2}@cts2; readers at snap 1 and 2. The
    // permutation case asserts the DEFINING difference from list-append:
    // reordered tokens are legal under set semantics (and the same history
    // WOULD fail the list-append checker — both directions asserted).
    {
        auto set_hist = [&]() {
            std::vector<lincheck::Txn> h;
            h.push_back(mkw(1, 0, 1, 1000000, 2000000, {{"S", "x1"}}));
            h.push_back(mkw(2, 1, 2, 3000000, 4000000, {{"S", "x1+x2"}}));
            h.push_back(mkr(3, 1, 2500000, 2700000, {{"S", true, "x1", 1}}));
            h.push_back(mkr(4, 2, 5000000, 5200000, {{"S", true, "x1+x2", 2}}));
            return h;
        };
        {
            auto v = lincheck::check_set_adds(set_hist(), "S");
            check("lincheck: synthetic clean set history passes check_set_adds",
                  v.empty(), lincheck::describe(v));
        }
        {
            auto h = set_hist();
            h[3].reads[0].value = "x2+x1";       // permuted, same set
            auto v = lincheck::check_set_adds(h, "S");
            auto w = lincheck::check_list_append(h);
            check("lincheck: permuted tokens pass check_set_adds but fail list-append (set shape is real)",
                  v.empty() && !w.empty(),
                  v.empty() ? lincheck::describe(w, 2) : lincheck::describe(v));
        }
        struct SetCase { const char* name; const char* kind;
                         std::function<void(std::vector<lincheck::Txn>&)> mutate; };
        std::vector<SetCase> xcases = {
            {"read repeats a token", "set-duplicate",
             [](std::vector<lincheck::Txn>& h) { h[3].reads[0].value = "x1+x2+x2"; }},
            {"read returns a token no committed add wrote", "set-fabricated",
             [](std::vector<lincheck::Txn>& h) { h[2].reads[0].value = "x1+x9"; }},
            {"read misses an add at-or-below its snapshot", "set-snapshot-mismatch",
             [](std::vector<lincheck::Txn>& h) { h[3].reads[0].value = "x1"; }},
            {"committed add drops a prior token (write-side lost add)", "set-write-fold",
             [](std::vector<lincheck::Txn>& h) { h[1].writes[0].value = "x2"; }},
            {"committed add writes duplicate tokens", "set-write-duplicate",
             [](std::vector<lincheck::Txn>& h) { h[1].writes[0].value = "x1+x1"; }},
            {"add re-adds an already-present token", "set-add-duplicate",
             [&](std::vector<lincheck::Txn>& h) {
                 h.push_back(mkw(5, 2, 3, 9000000, 9500000, {{"S", "x1+x2+x2"}}));
             }},
            {"read begun after an add's ack misses its token", "set-realtime-loss",
             [&](std::vector<lincheck::Txn>& h) {   // R5 begins 4.5us > ack(x2)=4.0us
                 h.push_back(mkr(5, 2, 4500000, 4700000, {{"S", true, "x1", 1}}));
             }},
        };
        for (auto& c : xcases) {
            auto h = set_hist();
            c.mutate(h);
            auto v = lincheck::check_set_adds(h, "S");
            bool ok = lincheck::has_kind(v, c.kind);
            std::string nm = std::string("lincheck: synthetic set anomaly flagged — ") + c.name;
            check(nm.c_str(), ok,
                  ok ? "" : ("expected " + std::string(c.kind) + "; got: " +
                             (v.empty() ? "<none>" : lincheck::describe(v))));
        }
    }

    // ---- Section 1d: synthetic WRITE-SKEW battery (v29 M2 item 1 — the
    // SSI anti-dependency checker) ----
    // Each anomaly below is a history a CORRECT SSI engine can never
    // commit; check_write_skew must flag each. W2 is shape-verbatim the
    // Audit-2 TXN-1 PoC the PRE-0.28.1 engine actually committed
    // (cross-verification log: T2=Committed updating the scanned key,
    // T1=Committed on its dependent write — value-based write skew),
    // which is the roadmap's acceptance: the checker must flag the
    // pre-fix engine's history and pass the post-fix engine's (C3 below
    // pins the post-fix shape: T1 aborted, committed history acyclic;
    // the live old-engine demonstration rides the confirmation record).
    auto mkx = [](uint64_t id, uint64_t snap, uint64_t cts,
                  std::vector<std::tuple<std::string, bool, std::string, uint64_t>> rs,
                  std::vector<std::tuple<std::string, std::string, uint64_t,
                      std::vector<std::pair<std::string, std::string>>>> scans,
                  std::vector<std::pair<std::string, std::string>> ws) {
        lincheck::Txn t;
        t.id = id; t.is_write = !ws.empty(); t.committed = true;
        t.has_snapshot = true; t.snapshot = snap; t.commit_cts = cts;
        for (auto& [k, found, val, vcts] : rs) {
            t.order.push_back({true, t.reads.size()});
            t.reads.push_back(lincheck::ReadEv{k, found, val, vcts});
        }
        for (auto& [lo, hi, ssnap, entries] : scans) {
            lincheck::ScanEv sc; sc.lo = lo; sc.hi = hi; sc.snap = ssnap; sc.entries = entries;
            t.scans.push_back(std::move(sc));
        }
        for (auto& [k, val] : ws) {
            t.order.push_back({false, t.writes.size()});
            t.writes.push_back(lincheck::WriteEv{k, val, false});
        }
        return t;
    };
    {
        // W1 — classic write-skew (constraint x+y >= 1): both read both
        // keys at snapshot 1, each writes one. Edges: T2's y-read missed
        // T3's y-write (T2->T3); T3's x-read missed T2's x-write (T3->T2).
        std::vector<lincheck::Txn> h;
        h.push_back(mkw(1, 0, 1, 0, 0, {{"x", "1"}, {"y", "1"}}));
        h.push_back(mkx(2, 1, 2, {{"x", true, "1", 1}, {"y", true, "1", 1}}, {}, {{"x", "0"}}));
        h.push_back(mkx(3, 1, 3, {{"x", true, "1", 1}, {"y", true, "1", 1}}, {}, {{"y", "0"}}));
        auto v = lincheck::check_write_skew(h);
        check("lincheck: synthetic write-skew (x/y constraint) flagged",
              lincheck::has_kind(v, "anti-dependency-cycle"), lincheck::describe(v));
    }
    {
        // W2 — the TXN-1 PoC shape: T2 (id 2) read x, then UPDATED the
        // scanned key k and committed FIRST (cts 2); T1 (id 3) scanned
        // [k,k] at snapshot 1 — missing k's new version — then wrote its
        // dependent x and committed (cts 3). Pre-fix engine: both
        // committed (existence-only tracking saw no phantom). Edges:
        // T1's scan missed k@2 (T1->T2); T2's x-read missed T1's x-write
        // @3 > snap 1 (T2->T1) — cycle.
        std::vector<lincheck::Txn> h;
        h.push_back(mkw(1, 0, 1, 0, 0, {{"k", "0"}, {"x", "0"}}));
        h.push_back(mkx(2, 1, 2, {{"x", true, "0", 1}}, {}, {{"k", "1"}}));
        h.push_back(mkx(3, 1, 3, {}, {{"k", "k", 1, {{"k", "0"}}}}, {{"x", "1"}}));
        auto v = lincheck::check_write_skew(h);
        check("lincheck: TXN-1 PoC history (scan missed a concurrent UPDATE) flagged",
              lincheck::has_kind(v, "anti-dependency-cycle"), lincheck::describe(v));
    }
    {
        // W3 — lost update: both read x@1, both write x. ww chains T2->T3;
        // T3's stale x-read missed T2's write (T3->T2) — cycle.
        std::vector<lincheck::Txn> h;
        h.push_back(mkw(1, 0, 1, 0, 0, {{"x", "0"}}));
        h.push_back(mkx(2, 1, 2, {{"x", true, "0", 1}}, {}, {{"x", "1"}}));
        h.push_back(mkx(3, 1, 3, {{"x", true, "0", 1}}, {}, {{"x", "2"}}));
        auto v = lincheck::check_write_skew(h);
        check("lincheck: synthetic lost update flagged",
              lincheck::has_kind(v, "anti-dependency-cycle"), lincheck::describe(v));
    }
    {
        // W4 — phantom write-skew: two empty scans of [k0,k9], each
        // inserting a key invisible to the other (false->true transitions
        // were recorded even pre-TXN-1 — the scan rw edges must catch it).
        std::vector<lincheck::Txn> h;
        h.push_back(mkx(2, 1, 2, {}, {{"k0", "k9", 1, {}}}, {{"k1", "a"}}));
        h.push_back(mkx(3, 1, 3, {}, {{"k0", "k9", 1, {}}}, {{"k5", "b"}}));
        auto v = lincheck::check_write_skew(h);
        check("lincheck: synthetic phantom write-skew (crossed scan inserts) flagged",
              lincheck::has_kind(v, "anti-dependency-cycle"), lincheck::describe(v));
    }
    {
        // C1 — serial control: the same ops, but T3 snapshots AFTER T2's
        // commit and reads T2's x-write: acyclic (this is the serialization
        // the SSI engine forces by aborting one side).
        std::vector<lincheck::Txn> h;
        h.push_back(mkw(1, 0, 1, 0, 0, {{"x", "1"}, {"y", "1"}}));
        h.push_back(mkx(2, 1, 2, {{"x", true, "1", 1}, {"y", true, "1", 1}}, {}, {{"x", "0"}}));
        h.push_back(mkx(3, 2, 3, {{"x", true, "0", 2}, {"y", true, "1", 1}}, {}, {{"y", "0"}}));
        auto v = lincheck::check_write_skew(h);
        check("lincheck: serial reordering of the write-skew passes (control)",
              v.empty(), lincheck::describe(v));
    }
    {
        // C2 — disjoint-key control: concurrent readers/writers on
        // separate keys have no shared dependency surface.
        std::vector<lincheck::Txn> h;
        h.push_back(mkw(1, 0, 1, 0, 0, {{"x", "0"}, {"y", "0"}}));
        h.push_back(mkx(2, 1, 2, {{"x", true, "0", 1}}, {}, {{"x", "9"}}));
        h.push_back(mkx(3, 1, 3, {{"y", true, "0", 1}}, {}, {{"y", "9"}}));
        auto v = lincheck::check_write_skew(h);
        check("lincheck: disjoint-key concurrent writers pass (control)",
              v.empty(), lincheck::describe(v));
    }
    {
        // C3 — the post-fix TXN-1 shape: the engine refuses T1 (Conflict),
        // so the committed history holds only the setup writer and T2;
        // T1 rides along as an ABORTED txn (with its scan) and must be
        // ignored by the checker — exactly what the post-fix engine
        // records for the PoC workload.
        std::vector<lincheck::Txn> h;
        h.push_back(mkw(1, 0, 1, 0, 0, {{"k", "0"}, {"x", "0"}}));
        h.push_back(mkx(2, 1, 2, {{"x", true, "0", 1}}, {}, {{"k", "1"}}));
        lincheck::Txn ab = mkx(3, 1, 0, {}, {{"k", "k", 1, {{"k", "0"}}}}, {{"x", "1"}});
        ab.committed = false; ab.is_write = false;
        h.push_back(ab);
        auto v = lincheck::check_write_skew(h);
        check("lincheck: post-fix TXN-1 history (T1 aborted) passes",
              v.empty(), lincheck::describe(v));
    }

    // ---- Section 2: real engine workload (v27 M2: N-seed scaling) ----
    // CKV_LINCHECK_SEEDS=N sweeps the workload's PRNG base (default: ONE
    // run at the historical 0xC0FFEE11 base, so the always-run suite is
    // unchanged); CKV_LINCHECK_SEED overrides the base. This is the M1
    // leftover "N-seed scaling of the engine workload (feeds M2's CI
    // job)": the CI dst job runs a bounded (PR) / long (nightly) sweep.
    // Every seed's history must independently pass both checkers AND be
    // non-vacuous; Section 3's mutations run against the last seed's
    // history (any non-vacuous history exercises the same detector paths).
    int lc_seeds = 1;
    if (const char* e = getenv("CKV_LINCHECK_SEEDS")) {
        int v = atoi(e);
        if (v > 0) lc_seeds = v;
    }
    uint64_t lc_seed_base = 0xC0FFEE11ULL;
    if (const char* e = getenv("CKV_LINCHECK_SEED")) {
        uint64_t v = strtoull(e, nullptr, 0);
        if (v) lc_seed_base = v;
    }
    std::vector<lincheck::Txn> hist;
    size_t n_readers = 0, n_writers = 0;
    for (int si = 0; si < lc_seeds; ++si) {
        const uint64_t wseed = lc_seed_base + (uint64_t)si * 0x9E3779B97F4A7C15ULL;
        std::string lbl;
        if (lc_seeds > 1) {
            char hb[32];
            snprintf(hb, sizeof hb, "0x%llx", (unsigned long long)wseed);
            lbl = std::string(" [seed ") + std::to_string(si + 1) + "/" +
                  std::to_string(lc_seeds) + " " + hb + "]";
        }
        n_readers = 0;
        n_writers = 0;
        const std::string wd = "/tmp/ckv_lincheck_wal";
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.durability = DurabilityMode::Group;
        o.page_pool_bytes = 32ULL * 1024 * 1024;
        auto db = Database::open(o);
        constexpr int NT = 4, NKEYS = 5, OPS = 140;
        std::atomic<uint64_t> committed_appends{0};
        txnrec::arm();
        std::vector<std::thread> ths;
        for (int t = 0; t < NT; ++t) ths.emplace_back([&, t] {
            uint64_t s = wseed + static_cast<uint64_t>(t) * 7919;
            auto lcg = [&] { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s >> 33; };
            for (int i = 0; i < OPS; ++i) {
                uint64_t r = lcg();
                std::string key = "L" + std::to_string(r % NKEYS);
                if ((r >> 8) % 10 < 6) {
                    // append transaction: read list, extend with a unique
                    // token, commit; retry on SSI conflict. Tokens embed
                    // (thread, op, attempt) so even aborted attempts can
                    // never collide with committed ones — an aborted token
                    // showing up in a read is fabrication, and the checker
                    // reads it exactly that way (unknown-token).
                    for (int att = 0; att < 25; ++att) {
                        std::string tok = "t" + std::to_string(t) + "_" +
                                          std::to_string(i) + "_" + std::to_string(att);
                        auto txn = db.begin();
                        auto cur = txn.get(key);
                        std::string old = cur.value_or("");
                        txn.put(key, old.empty() ? tok : old + "+" + tok);
                        if (txn.commit() == Status::OK) {
                            committed_appends.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                } else {
                    // snapshot read transaction over two keys
                    std::string k2 = "L" + std::to_string((r >> 16) % NKEYS);
                    auto txn = db.begin();
                    (void)txn.get(key);
                    (void)txn.get(k2);
                    (void)txn.commit();
                }
            }
        });
        for (auto& th : ths) th.join();
        // Quiesced full reads — the lost-append detectors of the workload.
        for (int k = 0; k < NKEYS; ++k) (void)db.get("L" + std::to_string(k));
        auto recs = txnrec::disarm();
        db.close();
        std::filesystem::remove_all(wd);
        hist = lincheck::from_txnrec(recs);
        for (const auto& t : hist) {
            if (t.is_write && t.committed) n_writers++;
            if (!t.reads.empty()) n_readers++;
        }
        auto v = both(hist);
        {   // v29 M2 item 1: the anti-dependency checker joins the engine verdict.
            auto ws = lincheck::check_write_skew(hist);
            v.insert(v.end(), ws.begin(), ws.end());
        }
        bool nonvacuous = hist.size() >= 100 && n_writers >= 5 && n_readers >= 10 &&
                          committed_appends.load() >= 5;
        check((std::string("lincheck: engine list-append workload is strictly serializable") +
               lbl).c_str(),
              v.empty() && nonvacuous,
              v.empty() ? ("history too small: txns=" + std::to_string(hist.size()) +
                           " writers=" + std::to_string(n_writers) +
                           " readers=" + std::to_string(n_readers))
                        : lincheck::describe(v, 4));
        std::cout << "      (history" << lbl << ": " << hist.size() << " txns, " << n_writers
                  << " committed writers, " << n_readers << " readers, "
                  << committed_appends.load() << " committed appends)\n";
    }

    // ---- Section 2b: mixed-API engine workload (v27 M1 completion) ----
    // Every formerly-unrecorded API in ONE live history: set-add RMW
    // transactions (S keys, set-checker shape), async put/get with prompt
    // future reads (A keys; ack_deferred => no ack edges, begin-side
    // freshness still checked), synchronous Batch multi-key commits (B/C
    // key pairs, full interval soundness), and range scans — standalone
    // (Database::range_scan over the churned space) and transactional
    // (pre-overlay engine view). Checked by cts-order + scans + sets.
    // (check_list_append deliberately does NOT run here: A/B/C keys are
    // OVERWRITE workloads, and the list-append fold algebra assumes the
    // append-only wire convention — the set checker is the right shape
    // for S, and cts-order replay is the right shape for the rest.)
    std::vector<lincheck::Txn> hist2;
    for (int si = 0; si < lc_seeds; ++si) {
        const uint64_t wseed = (lc_seed_base ^ 0x5EED1234ULL) +
                               (uint64_t)si * 0x9E3779B97F4A7C15ULL;
        std::string lbl;
        if (lc_seeds > 1) {
            char hb[32];
            snprintf(hb, sizeof hb, "0x%llx", (unsigned long long)wseed);
            lbl = std::string(" [seed ") + std::to_string(si + 1) + "/" +
                  std::to_string(lc_seeds) + " " + hb + "]";
        }
        const std::string wd = "/tmp/ckv_lincheck_mix_wal";
        std::filesystem::remove_all(wd);
        Options o;
        o.wal_dir = wd;
        o.durability = DurabilityMode::Group;
        o.page_pool_bytes = 32ULL * 1024 * 1024;
        auto db = Database::open(o);
        std::atomic<uint64_t> set_adds{0}, async_ops{0}, batch_commits{0}, scans_done{0};
        txnrec::arm();
        std::vector<std::thread> ths;
        // Two set-add threads: RMW appends of unique tokens onto S0/S1.
        for (int t = 0; t < 2; ++t) ths.emplace_back([&, t] {
            std::string key = "S" + std::to_string(t);
            for (int i = 0; i < 40; ++i) {
                for (int att = 0; att < 25; ++att) {
                    std::string tok = "x" + std::to_string(t) + "_" +
                                      std::to_string(i) + "_" + std::to_string(att);
                    auto txn = db.begin();
                    auto cur = txn.get(key);
                    std::string old = cur.value_or("");
                    txn.put(key, old.empty() ? tok : old + "+" + tok);
                    if (txn.commit() == Status::OK) {
                        set_adds.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        });
        // Async churn thread: put_async with PROMPT get() (documented
        // soundness contract for ack_deferred intervals) + occasional
        // get_async.
        ths.emplace_back([&] {
            for (int i = 0; i < 60; ++i) {
                std::string k = "A" + std::to_string(i % 3);
                auto f = db.put_async(k, "a" + std::to_string(i));
                if (f.get() == Status::OK)
                    async_ops.fetch_add(1, std::memory_order_relaxed);
                if ((i % 5) == 4) {
                    auto g = db.get_async(k);
                    (void)g.get();
                }
            }
        });
        // Batch thread: atomic multi-key commits (write-only, no conflicts).
        ths.emplace_back([&] {
            for (int i = 0; i < 30; ++i) {
                auto b = db.create_batch();
                b.put("B" + std::to_string(i % 4), "b" + std::to_string(i));
                b.put("C" + std::to_string(i % 4), "c" + std::to_string(i));
                if (b.commit() == Status::OK)
                    batch_commits.fetch_add(1, std::memory_order_relaxed);
            }
        });
        // Standalone scan thread over the churned keyspace.
        ths.emplace_back([&] {
            for (int i = 0; i < 40; ++i) {
                (void)db.range_scan("A0", "C9");
                scans_done.fetch_add(1, std::memory_order_relaxed);
            }
        });
        // Transactional reader: get + scan + staged write + commit. The
        // staged T-key write makes the txn's OWN scan overlay-dirty — the
        // recorder must capture the PRE-overlay engine view (verified by
        // check_scans against the committed-only replay). Range reads over
        // the churn space will often conflict-abort under SSI; aborts are
        // recorded and legal (their scans were still snapshot-consistent).
        ths.emplace_back([&] {
            for (int i = 0; i < 40; ++i) {
                auto txn = db.begin();
                (void)txn.get("A" + std::to_string(i % 3));
                (void)txn.range_scan("A0", "B9");
                txn.put("T" + std::to_string(i % 5), "t" + std::to_string(i));
                (void)txn.commit();
            }
        });
        for (auto& th : ths) th.join();
        // Quiesced final reads — the lost-add / lost-write detectors.
        for (int k = 0; k < 2; ++k) (void)db.get("S" + std::to_string(k));
        (void)db.range_scan("A0", "C9");
        auto recs = txnrec::disarm();
        db.close();
        std::filesystem::remove_all(wd);
        hist2 = lincheck::from_txnrec(recs);

        size_t n_scans = 0, n_setw = 0, n_aw = 0, n_bw = 0;
        for (const auto& t : hist2) {
            n_scans += t.scans.size();
            if (t.is_write && t.committed)
                for (const auto& w : t.writes) {
                    if (w.key.rfind("S", 0) == 0) n_setw++;
                    else if (w.key.rfind("A", 0) == 0) n_aw++;
                    else if (w.key.rfind("B", 0) == 0 || w.key.rfind("C", 0) == 0) n_bw++;
                }
        }
        auto v = lincheck::check_cts_order(hist2);
        {
            auto w = lincheck::check_scans(hist2);
            v.insert(v.end(), w.begin(), w.end());
            auto x = lincheck::check_set_adds(hist2, "S");
            v.insert(v.end(), x.begin(), x.end());
            auto y = lincheck::check_write_skew(hist2);   // v29 M2 item 1
            v.insert(v.end(), y.begin(), y.end());
        }
        // CKV_LINCHECK_DUMP=1: full recorded-history dump to stderr on
        // violation — the triage handle for mixed-workload failures (the
        // per-txn record lines are what diagnosed the v27 scan-attribution
        // artifact: RWT scans recorded as standalone synthetic txns).
        if (!v.empty() && getenv("CKV_LINCHECK_DUMP")) {
            for (const auto& x : v) fprintf(stderr, "VIOL %s: %s\n", x.kind.c_str(), x.detail.c_str());
            for (const auto& t : hist2) {
                fprintf(stderr, "TXN id=%llu w=%d c=%d cts=%llu snap=%llu hs=%d b=%llu e=%llu R=%zu W=%zu S=%zu",
                        (unsigned long long)t.id, (int)t.is_write, (int)t.committed,
                        (unsigned long long)t.commit_cts, (unsigned long long)t.snapshot,
                        (int)t.has_snapshot, (unsigned long long)t.begin_ns, (unsigned long long)t.end_ns,
                        t.reads.size(), t.writes.size(), t.scans.size());
                for (auto& r : t.reads) fprintf(stderr, " [r:%s=%s@%llu]", r.key.c_str(), r.value.c_str(), (unsigned long long)r.version_cts);
                for (auto& w2 : t.writes) fprintf(stderr, " [w:%s=%s]", w2.key.c_str(), w2.value.c_str());
                for (auto& sc : t.scans) fprintf(stderr, " [s:%s..%s@%llu n=%zu]", sc.lo.c_str(), sc.hi.c_str(), (unsigned long long)sc.snap, sc.entries.size());
                fprintf(stderr, "\n");
            }
        }
        bool nonvacuous = hist2.size() >= 100 && n_scans >= 10 && n_setw >= 10 &&
                          n_aw >= 5 && n_bw >= 5 &&
                          set_adds.load() >= 10 && async_ops.load() >= 5 &&
                          batch_commits.load() >= 5 && scans_done.load() >= 10;
        check((std::string("lincheck: mixed-API workload (sets+scans+async+batch) is strictly serializable") +
               lbl).c_str(),
              v.empty() && nonvacuous,
              v.empty() ? ("history under-populated: txns=" + std::to_string(hist2.size()) +
                           " scans=" + std::to_string(n_scans) +
                           " set-writes=" + std::to_string(n_setw) +
                           " async=" + std::to_string(n_aw) +
                           " batch=" + std::to_string(n_bw))
                        : lincheck::describe(v, 4));
        std::cout << "      (mixed history" << lbl << ": " << hist2.size() << " txns, "
                  << n_scans << " scans, " << n_setw << " set-writes, "
                  << n_aw << " async-writes, " << n_bw << " batch-writes)\n";
    }

    // ---- Section 3: mutations of the RECORDED engine history ----
    auto find_final_read = [&](const char* key) -> std::pair<size_t, size_t> {
        // last found read of `key` whose list has >= 2 tokens (a quiesced final read)
        for (size_t i = hist.size(); i-- > 0;) {
            for (size_t j = hist[i].reads.size(); j-- > 0;) {
                const auto& r = hist[i].reads[j];
                if (r.key == key && r.found && r.value.find('+') != std::string::npos &&
                    r.version_cts != UINT64_MAX)
                    return {i, j};
            }
        }
        return {SIZE_MAX, SIZE_MAX};
    };
    {
        auto [ti, ri] = find_final_read("L0");
        bool ok = ti != SIZE_MAX;
        if (ok) {
            auto h = hist;
            auto& r = h[ti].reads[ri];
            r.value = r.value.substr(0, r.value.rfind('+'));   // drop the last committed token
            auto v = lincheck::check_list_append(h);
            auto w = lincheck::check_cts_order(h);
            v.insert(v.end(), w.begin(), w.end());
            ok = lincheck::has_kind(v, "lost-append");
            check("lincheck: engine-history mutation flagged — dropped acknowledged token",
                  ok, lincheck::describe(v, 2));
        } else {
            check("lincheck: engine-history mutation flagged — dropped acknowledged token",
                  false, "no multi-token final read of L0 recorded");
        }
    }
    {
        auto [ti, ri] = find_final_read("L0");
        bool ok = ti != SIZE_MAX;
        if (ok) {
            auto h = hist;
            auto& r = h[ti].reads[ri];
            r.value = r.value.substr(0, r.value.find('+')) + "+" + r.value;  // duplicate first token
            auto v = lincheck::check_list_append(h);
            ok = lincheck::has_kind(v, "duplicate-token");
            check("lincheck: engine-history mutation flagged — duplicated token in a read",
                  ok, lincheck::describe(v, 2));
        } else {
            check("lincheck: engine-history mutation flagged — duplicated token in a read",
                  false, "no multi-token final read of L0 recorded");
        }
    }
    {
        // swap commit cts across a real-time edge: find committed writers
        // A, B with A fully acknowledged before B began; after the swap B
        // orders before an already-acknowledged A -> realtime-inversion.
        std::vector<const lincheck::Txn*> ws;
        for (const auto& t : hist)
            if (t.is_write && t.committed && t.commit_cts && t.begin_ns && t.end_ns)
                ws.push_back(&t);
        std::sort(ws.begin(), ws.end(),
                  [](const lincheck::Txn* a, const lincheck::Txn* b) { return a->end_ns < b->end_ns; });
        size_t ai = SIZE_MAX, bi = SIZE_MAX;
        for (size_t i = 0; i < ws.size() && ai == SIZE_MAX; ++i)
            for (size_t j = ws.size(); j-- > i + 1;)
                if (ws[i]->end_ns + 100000 < ws[j]->begin_ns) { ai = i; bi = j; break; }
        bool ok = ai != SIZE_MAX;
        if (ok) {
            auto h = hist;
            // map ids -> indices in the copy
            auto find_by_id = [&](uint64_t id) -> size_t {
                for (size_t i = 0; i < h.size(); ++i) if (h[i].id == id) return i;
                return SIZE_MAX;
            };
            size_t x = find_by_id(ws[ai]->id), y = find_by_id(ws[bi]->id);
            std::swap(h[x].commit_cts, h[y].commit_cts);
            auto v = lincheck::check_cts_order(h);
            ok = lincheck::has_kind(v, "realtime-inversion");
            check("lincheck: engine-history mutation flagged — cts swap across a real-time edge",
                  ok, lincheck::describe(v, 2));
        } else {
            check("lincheck: engine-history mutation flagged — cts swap across a real-time edge",
                  false, "no disjoint-interval writer pair found in the recorded history");
        }
    }
    {
        // push an observed version cts past the reader's snapshot
        bool done = false, ok = false;
        auto h = hist;
        for (auto& t : h) {
            if (!t.has_snapshot || t.snapshot == UINT64_MAX) continue;
            for (auto& r : t.reads) {
                if (r.found && r.version_cts > 0 && r.version_cts != UINT64_MAX &&
                    r.version_cts <= t.snapshot) {
                    r.version_cts = t.snapshot + 1;
                    done = true;
                    break;
                }
            }
            if (done) break;
        }
        if (done) {
            auto v = lincheck::check_cts_order(h);
            ok = lincheck::has_kind(v, "future-version-read");
        }
        check("lincheck: engine-history mutation flagged — version cts beyond snapshot",
              ok, done ? "" : "no suitable read found in the recorded history");
    }

    // ---- Section 3b: mutations of the RECORDED mixed-API history ----
    // The new checkers must bite engine-shaped data too, not just synthetics.
    {
        // (v) v29 M2 item 1: fabricate the TXN-1 dependency pair on
        // ENGINE-SHAPED txns. In a clean SSI history every committed read
        // sits at-or-below its snapshot (anything above was refused), so a
        // genuine anti-dependency cycle cannot be conjured from one field —
        // it takes the two missed dependencies the engine refuses to let
        // co-commit, reconstructed on real recorded txns: a scanning writer
        // A and another committed writer B with a key inside A's scan
        // range. Drop A's covering scan snapshot below B's write (A's scan
        // now "missed" B -> rw edge A->B), and give B a fabricated pre-A
        // read of a key A wrote (B "missed" A -> rw edge B->A). Either
        // half alone is what the engine rejects at commit; together they
        // close A->B->A.
        //
        // CI HARDENING (run #72, every leg): the first version of this
        // mutation required B to be write-only with a UINT64_MAX snapshot
        // and took the FIRST committed scanning writer as A — on CI's
        // faster runners (real io_uring, -O2, 4 cores) SSI refused every
        // scanning writer on some seeds and the check starved ("no
        // suitable A/B pair"). Two changes, keeping it non-vacuous:
        //   1. exhaustive pair search — ANY committed writer B whose key
        //      falls in ANY of A's scans (reads/snapshot shape of B
        //      unrestricted), over ALL scanning-writer candidates A;
        //   2. a fallback that cannot starve while the workload's
        //      nonvacuity gate holds: two REAL committed write txns; A
        //      gains a fabricated point scan over B's key at snap
        //      cts_B - 1. The scan is synthetic, the txns/keys/cts are
        //      engine data, and the cycle algebra is identical.
        bool done = false, ok = false, via_fallback = false;
        auto h = hist2;
        lincheck::Txn* A = nullptr;
        lincheck::Txn* B = nullptr;
        size_t a_scan_idx = 0;
        std::vector<lincheck::Txn*> cwriters;
        for (auto& t : h)
            if (t.committed && t.is_write && t.commit_cts && !t.writes.empty())
                cwriters.push_back(&t);
        // Primary: a real scanning writer A + any other committed writer B
        // with a key inside one of A's scan ranges.
        for (auto& t : h) {
            if (A) break;
            if (!t.committed || !t.is_write || !t.commit_cts || t.scans.empty() || t.writes.empty())
                continue;
            for (auto& u : h) {
                if (&u == &t || !u.committed || !u.is_write || !u.commit_cts) continue;
                bool pair_found = false;
                for (size_t si = 0; si < t.scans.size() && !pair_found; ++si) {
                    const auto& sc = t.scans[si];
                    for (const auto& w : u.writes)
                        if (sc.lo <= w.key && w.key <= sc.hi) {
                            A = &t; B = &u; a_scan_idx = si; pair_found = true; break;
                        }
                }
                if (pair_found) break;
            }
        }
        // Fallback: two real committed writers; A gains the fabricated scan.
        if (!A && cwriters.size() >= 2) {
            A = cwriters[0]; B = cwriters[1];
            if (A == B) B = cwriters.size() > 2 ? cwriters[2] : nullptr;
            if (A && B && A != B) {
                lincheck::ScanEv sc;
                sc.lo = sc.hi = B->writes.front().key;
                sc.snap = B->commit_cts - 1;
                A->scans.push_back(std::move(sc));
                a_scan_idx = A->scans.size() - 1;
                via_fallback = true;
            } else { A = B = nullptr; }
        }
        if (A && B) {
            A->scans[a_scan_idx].snap = B->commit_cts - 1;      // A's scan misses B's write
            B->has_snapshot = true;
            B->snapshot = A->commit_cts - 1;                    // B "started" before A's write
            B->order.push_back({true, B->reads.size()});
            B->reads.push_back(lincheck::ReadEv{A->writes.front().key, false, "", 0});
            done = true;
        }
        if (done) {
            auto v = lincheck::check_write_skew(h);
            ok = lincheck::has_kind(v, "anti-dependency-cycle");
        }
        check("lincheck: mixed-history mutation flagged — fabricated missed anti-dependency pair",
              ok, done ? (ok ? "" : "anti-dependency-cycle not flagged")
                       : "history has fewer than two committed writers (nonvacuity gate should have failed first)");
        if (done && via_fallback)
            std::cout << "      (mutation used the fabricated-scan fallback: no committed scanning writer in this history)\n";
    }
    {
        // (i) drop a scan entry -> scan-missing-key (every recorded entry was
        // live at the scan's snapshot, so any deletion is a true omission).
        bool done = false, ok = false;
        auto h = hist2;
        for (auto& t : h) {
            for (auto& sc : t.scans)
                if (!done && sc.entries.size() >= 2) {
                    sc.entries.erase(sc.entries.begin());
                    done = true;
                }
            if (done) break;
        }
        if (done) {
            auto v = lincheck::check_scans(h);
            ok = lincheck::has_kind(v, "scan-missing-key");
        }
        check("lincheck: mixed-history mutation flagged — dropped scan entry",
              ok, done ? "scan-missing-key not flagged" : "no scan with >=2 entries recorded");
    }
    {
        // (ii) inject a never-written key inside a scan's bounds -> scan-phantom.
        bool done = false, ok = false;
        auto h = hist2;
        for (auto& t : h) {
            for (auto& sc : t.scans)
                if (!done && sc.lo <= std::string("A0~ghost") && std::string("A0~ghost") <= sc.hi) {
                    sc.entries.push_back({"A0~ghost", "nope"});
                    done = true;
                }
            if (done) break;
        }
        if (done) {
            auto v = lincheck::check_scans(h);
            ok = lincheck::has_kind(v, "scan-phantom");
        }
        check("lincheck: mixed-history mutation flagged — injected scan phantom",
              ok, done ? "scan-phantom not flagged" : "no scan covering 'A0~ghost' recorded");
    }
    {
        // (iii) duplicate a token in an S-key read -> set-duplicate.
        bool done = false, ok = false;
        auto h = hist2;
        for (auto& t : h) {
            for (auto& r : t.reads)
                if (!done && r.found && r.key.rfind("S", 0) == 0 &&
                    r.value.find('+') != std::string::npos) {
                    r.value += "+" + r.value.substr(0, r.value.find('+'));
                    done = true;
                }
            if (done) break;
        }
        if (done) {
            auto v = lincheck::check_set_adds(h, "S");
            ok = lincheck::has_kind(v, "set-duplicate");
        }
        check("lincheck: mixed-history mutation flagged — duplicated set token",
              ok, done ? "set-duplicate not flagged" : "no multi-token S read recorded");
    }
    {
        // (iv) swap a BATCH txn's commit cts across a real-time edge ->
        // realtime-inversion. Batch commits are synchronous, so their
        // intervals carry full ack-edge soundness — this mutation proves
        // the recorded batch intervals actually participate in real-time
        // checking (an unrecorded/zeroed interval would make it inert).
        auto is_batch = [](const lincheck::Txn& t) {
            if (!(t.is_write && t.committed && t.commit_cts)) return false;
            int bc = 0;
            for (const auto& w : t.writes)
                if (w.key.rfind("B", 0) == 0 || w.key.rfind("C", 0) == 0) bc++;
            return bc >= 2;
        };
        std::vector<const lincheck::Txn*> ws;
        for (const auto& t : hist2)
            if (t.is_write && t.committed && t.commit_cts && t.begin_ns && t.end_ns)
                ws.push_back(&t);
        std::sort(ws.begin(), ws.end(),
                  [](const lincheck::Txn* a, const lincheck::Txn* b) { return a->end_ns < b->end_ns; });
        size_t ai = SIZE_MAX, bi = SIZE_MAX;
        for (size_t i = 0; i < ws.size() && ai == SIZE_MAX; ++i)
            for (size_t j = ws.size(); j-- > i + 1;)
                if (ws[i]->end_ns + 100000 < ws[j]->begin_ns &&
                    (is_batch(*ws[i]) || is_batch(*ws[j]))) { ai = i; bi = j; break; }
        bool ok = ai != SIZE_MAX;
        if (ok) {
            auto h = hist2;
            auto find_by_id = [&](uint64_t id) -> size_t {
                for (size_t i = 0; i < h.size(); ++i) if (h[i].id == id) return i;
                return SIZE_MAX;
            };
            size_t x = find_by_id(ws[ai]->id), y = find_by_id(ws[bi]->id);
            std::swap(h[x].commit_cts, h[y].commit_cts);
            auto v = lincheck::check_cts_order(h);
            ok = lincheck::has_kind(v, "realtime-inversion");
            check("lincheck: mixed-history mutation flagged — batch cts swap across a real-time edge",
                  ok, lincheck::describe(v, 2));
        } else {
            check("lincheck: mixed-history mutation flagged — batch cts swap across a real-time edge",
                  false, "no batch txn with a disjoint-interval real-time partner found");
        }
    }

    if (fails == 0) std::cout << "   LINCHECK TEST PASSED\n";
    return fails;
}
#endif // CHRONOKV_TEST_HOOKS (extracted battery TU)
