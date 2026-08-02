// chrono_kv_v12_oracle.cpp — post-recovery lost-commit oracle.
// W threads increment a counter via durable transactions; crash; recover; assert
// the recovered counter == committed increments. BROKEN_DURABILITY drops the fsync
// (then simulates the power-loss cache flush), losing every undurable commit.
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
#include <fstream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
using namespace std;
constexpr size_t MAX_KEYS=64, RING=1024, MAX_READERS=128;
#define WIDEN_YIELD() this_thread::yield()

static uint32_t CRCT[256];
static void crc_init(){ for(uint32_t i=0;i<256;++i){ uint32_t c=i; for(int k=0;k<8;++k) c=(c&1)?(0xEDB88320^(c>>1)):(c>>1); CRCT[i]=c; } }
static uint32_t crc32(const uint8_t* d,size_t n){ uint32_t c=0xFFFFFFFF; for(size_t i=0;i<n;++i) c=CRCT[(c^d[i])&0xFF]^(c>>8); return c^0xFFFFFFFF; }
using WriteSet=vector<pair<string,string>>;
static vector<uint8_t> wal_ser(uint64_t ts,const WriteSet& ws){ vector<uint8_t> o;
  auto u64=[&](uint64_t v){for(int i=0;i<8;++i)o.push_back((v>>(8*i))&0xFF);};
  auto u32=[&](uint32_t v){for(int i=0;i<4;++i)o.push_back((v>>(8*i))&0xFF);};
  auto u16=[&](uint16_t v){for(int i=0;i<2;++i)o.push_back((v>>(8*i))&0xFF);};
  u64(ts); u32((uint32_t)ws.size());
  for(auto&[k,v]:ws){u16((uint16_t)k.size());for(char c:k)o.push_back((uint8_t)c);u32((uint32_t)v.size());for(char c:v)o.push_back((uint8_t)c);} return o; }
static vector<uint8_t> wal_frame(const vector<uint8_t>& p){ vector<uint8_t> o; uint32_t len=(uint32_t)p.size(),crc=crc32(p.data(),p.size());
  for(int i=0;i<4;++i)o.push_back((len>>(8*i))&0xFF); for(int i=0;i<4;++i)o.push_back((crc>>(8*i))&0xFF); o.insert(o.end(),p.begin(),p.end()); return o; }
static void write_all(int fd,const uint8_t* d,size_t n){ while(n>0){ssize_t w=write(fd,d,n); if(w<=0)break; d+=w; n-=w;} }
class WAL{ int fd_; public:
  WAL(const string& p){fd_=open(p.c_str(),O_CREAT|O_WRONLY|O_APPEND,0644);} ~WAL(){if(fd_>=0)close(fd_);}
  void append(uint64_t ts,const WriteSet& ws){auto b=wal_frame(wal_ser(ts,ws)); write_all(fd_,b.data(),b.size());} void sync(){fsync(fd_);} };
static vector<pair<uint64_t,WriteSet>> wal_recover(const string& path){ vector<pair<uint64_t,WriteSet>> out; ifstream f(path,ios::binary); if(!f)return out;
  auto ru32=[&](uint32_t& v)->bool{uint8_t b[4]; if(!f.read((char*)b,4))return false; v=0; for(int i=0;i<4;++i)v|=((uint32_t)b[i])<<(8*i); return true;};
  while(true){uint32_t len,crc; if(!ru32(len))break; if(len>(1u<<20))break; if(!ru32(crc))break;
    vector<uint8_t> p(len); if(!f.read((char*)p.data(),len))break; if(crc32(p.data(),len)!=crc)break;
    size_t pos=0; auto pu64=[&](){uint64_t v=0;for(int i=0;i<8;++i)v|=((uint64_t)p[pos++])<<(8*i);return v;};
    auto pu32=[&](){uint32_t v=0;for(int i=0;i<4;++i)v|=((uint32_t)p[pos++])<<(8*i);return v;};
    auto pu16=[&](){uint16_t v=0;for(int i=0;i<2;++i)v|=((uint16_t)p[pos++])<<(8*i);return v;};
    uint64_t ts=pu64(); uint32_t n=pu32(); WriteSet ws;
    for(uint32_t i=0;i<n;++i){uint16_t kl=pu16(); string k((char*)&p[pos],kl); pos+=kl; uint32_t vl=pu32(); string v((char*)&p[pos],vl); pos+=vl; ws.push_back({k,v});}
    out.push_back({ts,ws});} return out; }

struct Version{atomic<uint64_t> commit_ts; string value; atomic<Version*> prev; Version(uint64_t t,string v,Version* p):commit_ts(t),value(move(v)),prev(p){}};
struct LogNode{uint64_t ts; atomic<LogNode*> next; LogNode(uint64_t t,LogNode* n):ts(t),next(n){}};

class ChronoKV{
  mutable shared_mutex nm_; unordered_map<string,int> name2idx_;
  array<atomic<Version*>,MAX_KEYS> heads_; array<mutex,MAX_KEYS> commit_mu_; array<atomic<uint64_t>,MAX_KEYS> last_write_ts_;
  atomic<uint64_t> clock_{1}; array<atomic<uint64_t>,RING> done_; atomic<uint64_t> published_{0};
  array<atomic<uint64_t>,MAX_READERS> slots_; atomic<unsigned> next_slot_{0}; atomic<LogNode*> log_head_{nullptr};
  mutex gc_mu_; condition_variable gc_cv_; atomic<bool> dirty_{false},running_{false}; thread gc_;
  WAL* wal_=nullptr; mutex wal_mu_; bool fsync_enabled_=true;
  int find_index(const string& k) const {shared_lock lk(nm_); auto it=name2idx_.find(k); return it==name2idx_.end()?-1:it->second;}
  int ensure_index(const string& k){unique_lock lk(nm_); auto it=name2idx_.find(k); if(it!=name2idx_.end())return it->second; int idx=(int)name2idx_.size(); if(idx>=(int)MAX_KEYS)throw runtime_error("MAX_KEYS"); name2idx_[k]=idx; return idx;}
  void wake_gc(){dirty_.store(true,memory_order_release); gc_cv_.notify_one();}
  void log_push(uint64_t ts){LogNode* n=new LogNode(ts,nullptr); LogNode* h=log_head_.load(memory_order_relaxed); do{n->next.store(h,memory_order_relaxed);}while(!log_head_.compare_exchange_weak(h,n,memory_order_release,memory_order_relaxed));}
  bool in_log(uint64_t ts) const {LogNode* c=log_head_.load(memory_order_acquire); while(c){if(c->ts==ts)return true; c=c->next.load(memory_order_acquire);} return false;}
  uint64_t gc_threshold() const {uint64_t p=published_.load(memory_order_seq_cst),rm=UINT64_MAX; for(size_t i=0;i<MAX_READERS;++i){uint64_t v=slots_[i].load(memory_order_seq_cst); if(v!=0)rm=min(rm,v-1);} return min(rm,p);}
  void advance_frontier(){uint64_t cur=published_.load(memory_order_seq_cst); for(;;){uint64_t nxt=cur+1; bool complete=(done_[nxt%RING].load(memory_order_acquire)==nxt)||in_log(nxt); if(!complete)break; if(published_.compare_exchange_weak(cur,nxt,memory_order_seq_cst,memory_order_seq_cst))cur=nxt;}}
  void gc_once(){uint64_t m=gc_threshold(); size_t n; {shared_lock lk(nm_); n=name2idx_.size();} for(size_t i=0;i<n;++i){Version* cur=heads_[i].load(memory_order_acquire); bool anchor=false; while(cur){Version* nxt=cur->prev.load(memory_order_acquire); uint64_t c=cur->commit_ts.load(memory_order_acquire); if(c==0){cur=nxt;continue;} if(!anchor&&c<=m){anchor=true;cur->prev.store(nullptr,memory_order_release);cur=nxt;continue;} if(anchor)delete cur; cur=nxt;}}}
  void free_all(){for(size_t i=0;i<MAX_KEYS;++i){Version* c=heads_[i].load(memory_order_relaxed); while(c){Version* n=c->prev.load(memory_order_relaxed); delete c; c=n;} heads_[i].store(nullptr,memory_order_relaxed);} LogNode* ln=log_head_.load(memory_order_relaxed); while(ln){LogNode* n=ln->next.load(memory_order_relaxed); delete ln; ln=n;}}
  void link_version(int idx,uint64_t ts,const string& v){Version* n=new Version(ts,v,heads_[idx].load(memory_order_relaxed)); heads_[idx].store(n,memory_order_release); last_write_ts_[idx].store(ts,memory_order_relaxed);}
public:
  ChronoKV(const string& wp=""){if(!wp.empty())wal_=new WAL(wp); for(auto&h:heads_)h.store(nullptr,memory_order_relaxed); for(auto&d:done_)d.store(0,memory_order_relaxed); for(auto&s:slots_)s.store(0,memory_order_relaxed); for(auto&w:last_write_ts_)w.store(0,memory_order_relaxed);}
  ~ChronoKV(){{lock_guard<mutex> lk(gc_mu_); running_.store(false,memory_order_release);} gc_cv_.notify_all(); if(gc_.joinable())gc_.join(); free_all(); delete wal_;}
  void set_fsync(bool b){fsync_enabled_=b;}
  void start_gc(){running_.store(true,memory_order_release); gc_=thread([this]{unique_lock<mutex> lk(gc_mu_); while(running_.load(memory_order_acquire)){gc_cv_.wait(lk,[this]{return !running_.load(memory_order_acquire)||dirty_.load(memory_order_acquire);}); if(!running_.load(memory_order_acquire))break; dirty_.store(false,memory_order_release); lk.unlock(); gc_once(); lk.lock();}});}
  void recover(const string& wp){uint64_t mx=0; for(auto&[ts,ws]:wal_recover(wp)){for(auto&[k,v]:ws)link_version(ensure_index(k),ts,v); done_[ts%RING].store(ts,memory_order_release); mx=max(mx,ts);} if(mx>0){clock_.store(mx+1,memory_order_relaxed); published_.store(mx,memory_order_seq_cst);}}
  void commit(const string& key,const string& value){int idx=ensure_index(key); lock_guard<mutex> lk(commit_mu_[idx]); uint64_t ts=clock_.fetch_add(1,memory_order_relaxed); WIDEN_YIELD();
    if(wal_){lock_guard<mutex> wlk(wal_mu_); wal_->append(ts,{{key,value}}); if(fsync_enabled_)wal_->sync();}
    link_version(idx,ts,value); done_[ts%RING].store(ts,memory_order_release); log_push(ts); advance_frontier(); wake_gc();}
  bool commit_txn(uint64_t read_ts,const WriteSet& ws){          // FIXED: caller supplies the txn snapshot
    if(ws.empty())return true;
    vector<tuple<int,string,string>> items; for(auto&[k,v]:ws)items.emplace_back(ensure_index(k),k,v);
    sort(items.begin(),items.end(),[](auto&a,auto&b){return get<0>(a)<get<0>(b);});
    for(auto&[idx,k,v]:items)commit_mu_[idx].lock();
    for(auto&[idx,k,v]:items) if(last_write_ts_[idx].load(memory_order_relaxed)>read_ts){for(auto&[i2,k2,v2]:items)commit_mu_[i2].unlock(); return false;}
    uint64_t cts=clock_.fetch_add(1,memory_order_relaxed); WIDEN_YIELD();
    if(wal_){lock_guard<mutex> wlk(wal_mu_); wal_->append(cts,ws); if(fsync_enabled_)wal_->sync();}   // durability point
    for(auto&[idx,k,v]:items)link_version(idx,cts,v);
    for(auto&[idx,k,v]:items)commit_mu_[idx].unlock();
    done_[cts%RING].store(cts,memory_order_release); log_push(cts); advance_frontier(); wake_gc(); return true;}
  int acquire_slot(uint64_t& read_ts){thread_local int slot=-1; if(slot<0){slot=(int)next_slot_.fetch_add(1,memory_order_relaxed); if(slot>=(int)MAX_READERS)throw runtime_error("readers");} uint64_t r; for(;;){r=published_.load(memory_order_seq_cst); WIDEN_YIELD(); slots_[slot].store(r+1,memory_order_seq_cst); if(published_.load(memory_order_seq_cst)==r)break;} read_ts=r; return slot;}
  void release_slot(int slot){slots_[slot].store(0,memory_order_seq_cst);}
  optional<string> read_at(uint64_t read_ts,const string& key) const {int idx=find_index(key); optional<string> result; if(idx>=0){Version* cur=heads_[idx].load(memory_order_acquire); while(cur){uint64_t c=cur->commit_ts.load(memory_order_acquire); if(c!=0&&c<=read_ts){result=cur->value;break;} cur=cur->prev.load(memory_order_acquire);}} return result;}
  optional<string> read(const string& key){uint64_t r; int s=acquire_slot(r); auto res=read_at(r,key); release_slot(s); return res;}
};
class ReadWriteTransaction{ChronoKV& kv_; uint64_t read_ts_; int slot_; map<string,string> ws_; bool fin_=false; public:
  explicit ReadWriteTransaction(ChronoKV& kv):kv_(kv){slot_=kv_.acquire_slot(read_ts_);}
  ~ReadWriteTransaction(){if(!fin_)kv_.release_slot(slot_);}
  optional<string> read(const string& k){auto it=ws_.find(k); if(it!=ws_.end())return it->second; return kv_.read_at(read_ts_,k);}
  void write(const string& k,const string& v){ws_[k]=v;}
  bool commit(){fin_=true; WriteSet ws(ws_.begin(),ws_.end()); bool ok=kv_.commit_txn(read_ts_,ws); kv_.release_slot(slot_); return ok;} };

int main(){
  crc_init(); const string wp="/tmp/v12_oracle.wal"; constexpr int W=4,M=50;
  atomic<long> ack{0};
  { unlink(wp.c_str());
    ChronoKV kv(wp);
#ifdef BROKEN_DURABILITY
    kv.set_fsync(false);                       // broken: commits never made durable
#endif
    kv.start_gc(); kv.commit("c","0");
    atomic<bool> go{false}; vector<thread> ts;
    for(int w=0;w<W;++w) ts.emplace_back([&]{ while(!go.load())this_thread::yield();
      for(int i=0;i<M;++i){ bool ok=false; while(!ok){ ReadWriteTransaction t(kv);
        auto v=t.read("c"); int c=v?stoi(*v):0; t.write("c",to_string(c+1)); ok=t.commit(); }
        ack.fetch_add(1); } std::cerr<<"  [thread done, ack="<<ack.load()<<"]\n"; });
    go.store(true); for(auto&t:ts)t.join();
  } // crash: store destroyed
#ifdef BROKEN_DURABILITY
  truncate(wp.c_str(),0);                      // power loss flushes the never-fsync'd cache
#endif
  ChronoKV kv(wp); kv.recover(wp);
  auto v=kv.read("c"); long recovered=v?stol(*v):-1; long expected=ack.load();
  cout<<"committed increments: "<<expected<<"\nrecovered counter:    "<<recovered<<"\n";
#ifdef BROKEN_DURABILITY
  cout<<(recovered<expected ? "CONTROL FIRED — "+to_string(expected-recovered)+" committed increments lost (never fsync'd)\n" : "CONTROL DID NOT FIRE — expected lost commits\n");
  return recovered<expected?0:1;
#else
  cout<<(recovered==expected ? "POST-RECOVERY CLEAN — every durable commit survived, no lost updates\n" : "LOST COMMITS: "+to_string(expected-recovered)+"\n");
  return recovered==expected?0:1;
#endif
}
