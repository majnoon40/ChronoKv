// chrono_kv_v12.cpp — the full marriage: v11 transactions + WAL durability.
// Write-ahead commit: validate -> reserve cts -> WAL append -> fsync (DURABLE)
// -> link versions -> advance frontier. Recovery replays the intact WAL prefix.
#include <atomic>
#include <mutex>
#include <memory>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <map>
#include <set>
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
#include <cstdio>
using namespace std;

constexpr size_t MAX_KEYS=2048, RING=1024, MAX_READERS=128;
#ifdef WIDEN
  #define WIDEN_YIELD() this_thread::yield()
#else
  #define WIDEN_YIELD() ((void)0)
#endif

// ---------- CRC-32 + WAL framing ----------
static uint32_t CRCT[256];
static void crc_init(){ for(uint32_t i=0;i<256;++i){ uint32_t c=i; for(int k=0;k<8;++k) c=(c&1)?(0xEDB88320^(c>>1)):(c>>1); CRCT[i]=c; } }
static uint32_t crc32(const uint8_t* d,size_t n){ uint32_t c=0xFFFFFFFF; for(size_t i=0;i<n;++i) c=CRCT[(c^d[i])&0xFF]^(c>>8); return c^0xFFFFFFFF; }
using WriteSet=vector<tuple<string,string,bool>>;
static vector<uint8_t> wal_ser(uint64_t ts,const WriteSet& ws){
  vector<uint8_t> o; auto u64=[&](uint64_t v){for(int i=0;i<8;++i)o.push_back((v>>(8*i))&0xFF);};
  auto u32=[&](uint32_t v){for(int i=0;i<4;++i)o.push_back((v>>(8*i))&0xFF);};
  auto u16=[&](uint16_t v){for(int i=0;i<2;++i)o.push_back((v>>(8*i))&0xFF);};
  u64(ts); u32((uint32_t)ws.size());
  for(auto&[k,v,d]:ws){ u16((uint16_t)k.size()); for(char c:k)o.push_back((uint8_t)c); u32((uint32_t)v.size()); for(char c:v)o.push_back((uint8_t)c); o.push_back(d?1:0);} return o; }
static vector<uint8_t> wal_frame(const vector<uint8_t>& p){
  vector<uint8_t> o; uint32_t len=(uint32_t)p.size(),crc=crc32(p.data(),p.size());
  for(int i=0;i<4;++i)o.push_back((len>>(8*i))&0xFF); for(int i=0;i<4;++i)o.push_back((crc>>(8*i))&0xFF);
  o.insert(o.end(),p.begin(),p.end()); return o; }
static void write_all(int fd,const uint8_t* d,size_t n){ while(n>0){ssize_t w=write(fd,d,n); if(w<=0)break; d+=w; n-=w;} }
static void fsync_dir(const string& path){string d=path;size_t s=d.find_last_of('/');d=(s==string::npos)?".":d.substr(0,s);int dfd=open(d.c_str(),O_RDONLY);if(dfd>=0){fsync(dfd);close(dfd);}}
enum class WalStatus{OK,TORN_TAIL,CORRUPT};
static pair<WalStatus,vector<pair<uint64_t,WriteSet>>> wal_recover2(const string& path){
  vector<pair<uint64_t,WriteSet>> out;
  ifstream ff(path,ios::binary); if(!ff)return{WalStatus::OK,out};
  vector<uint8_t> buf((istreambuf_iterator<char>(ff)),istreambuf_iterator<char>());
  size_t pos=0;
  auto parse=[&](size_t& p,uint64_t& ts,WriteSet& ws)->bool{
    if(p+8>buf.size())return false;
    uint32_t len=0,crc=0;
    for(int i=0;i<4;++i)len|=((uint32_t)buf[p+i])<<(8*i);
    for(int i=0;i<4;++i)crc|=((uint32_t)buf[p+4+i])<<(8*i);
    if(len>(1u<<20)||p+8+len>buf.size())return false;
    if(crc32(&buf[p+8],len)!=crc)return false;
    size_t q=p+8;
    auto pu64=[&](){uint64_t v=0;for(int i=0;i<8;++i)v|=((uint64_t)buf[q++])<<(8*i);return v;};
    auto pu32=[&](){uint32_t v=0;for(int i=0;i<4;++i)v|=((uint32_t)buf[q++])<<(8*i);return v;};
    auto pu16=[&](){uint16_t v=0;for(int i=0;i<2;++i)v|=((uint16_t)buf[q++])<<(8*i);return v;};
    ts=pu64();uint32_t n=pu32();ws.clear();
    for(uint32_t i=0;i<n;++i){uint16_t kl=pu16();string k((char*)&buf[q],kl);q+=kl;uint32_t vl=pu32();string v((char*)&buf[q],vl);q+=vl;bool dl=buf[q++]!=0;ws.push_back({k,v,dl});}
    p+=8+len;return true;
  };
  while(pos<buf.size()){
    uint64_t ts;WriteSet ws;size_t save=pos;
    if(parse(pos,ts,ws))out.push_back({ts,ws});
    else{
      uint32_t len=0;
      if(save+4<=buf.size())for(int i=0;i<4;++i)len|=((uint32_t)buf[save+i])<<(8*i);
      bool valid_after=false; // NOTE: if the length field itself is corrupt, a garbage len
      // may push valid_after to false by construction, classifying mid-file corruption as
      // TORN_TAIL. Inherent ambiguity — the CRC protects the payload, not the framing.
      if(len<=(1u<<20)&&save+8+len<buf.size()){size_t nxt=save+8+len;uint64_t t2;WriteSet w2;if(parse(nxt,t2,w2))valid_after=true;}
      return{valid_after?WalStatus::CORRUPT:WalStatus::TORN_TAIL,out};
    }
  }
  return{WalStatus::OK,out};
}
struct Batch{std::vector<std::vector<uint8_t>> records; bool done=false;};
class WAL{
  int fd_;
  std::mutex batch_mu_; std::condition_variable batch_cv_;
  std::shared_ptr<Batch> cur_batch_; bool leader_active_=false;
public:
  WAL(const string& p){ fd_=open(p.c_str(),O_CREAT|O_WRONLY|O_APPEND,0644); fsync_dir(p); }
  ~WAL(){ if(fd_>=0)close(fd_); }
  void append(uint64_t ts,const WriteSet& ws){auto b=wal_frame(wal_ser(ts,ws)); write_all(fd_,b.data(),b.size());}
  void sync(){fsync(fd_);}
  void group_append(uint64_t ts,const WriteSet& ws){
    auto rec=wal_frame(wal_ser(ts,ws));
    std::unique_lock<std::mutex> lk(batch_mu_);
    if(!cur_batch_)cur_batch_=std::make_shared<Batch>();
    auto my_batch=cur_batch_;
    my_batch->records.push_back(rec);
    while(!my_batch->done){
      if(!leader_active_){
        leader_active_=true;
        lk.unlock();
        std::this_thread::yield();
        lk.lock();
        auto batch=cur_batch_; cur_batch_=nullptr;
        lk.unlock();
        for(auto& b:batch->records)write_all(fd_,b.data(),b.size());
        fsync(fd_);
        lk.lock();
        batch->done=true; leader_active_=false;
        batch_cv_.notify_all();
      } else {
        batch_cv_.wait(lk);
      }
    }
  }
};
static vector<pair<uint64_t,WriteSet>> wal_recover(const string& path){
  vector<pair<uint64_t,WriteSet>> out; ifstream f(path,ios::binary); if(!f)return out;
  auto ru32=[&](uint32_t& v)->bool{uint8_t b[4]; if(!f.read((char*)b,4))return false; v=0; for(int i=0;i<4;++i)v|=((uint32_t)b[i])<<(8*i); return true;};
  while(true){ uint32_t len,crc; if(!ru32(len))break; if(len>(1u<<20))break; if(!ru32(crc))break;
    vector<uint8_t> p(len); if(!f.read((char*)p.data(),len))break; if(crc32(p.data(),len)!=crc)break;
    size_t pos=0; auto pu64=[&](){uint64_t v=0;for(int i=0;i<8;++i)v|=((uint64_t)p[pos++])<<(8*i);return v;};
    auto pu32=[&](){uint32_t v=0;for(int i=0;i<4;++i)v|=((uint32_t)p[pos++])<<(8*i);return v;};
    auto pu16=[&](){uint16_t v=0;for(int i=0;i<2;++i)v|=((uint16_t)p[pos++])<<(8*i);return v;};
    uint64_t ts=pu64(); uint32_t n=pu32(); WriteSet ws;
    for(uint32_t i=0;i<n;++i){uint16_t kl=pu16(); string k((char*)&p[pos],kl); pos+=kl; uint32_t vl=pu32(); string v((char*)&p[pos],vl); pos+=vl; bool dl=p[pos++]!=0; ws.push_back({k,v,dl});}
    out.push_back({ts,ws}); }
  return out; }

// ---------- MVCC store ----------
struct Version{ atomic<uint64_t> commit_ts; string value; bool deleted; atomic<Version*> prev; Version(uint64_t t,string v,bool d,Version* p):commit_ts(t),value(move(v)),deleted(d),prev(p){} };
struct LogNode{ uint64_t ts; atomic<LogNode*> next; LogNode(uint64_t t,LogNode* n):ts(t),next(n){} };

class ChronoKV {
  mutable shared_mutex nm_; unordered_map<string,int> name2idx_;
  array<atomic<Version*>,MAX_KEYS> heads_; array<mutex,MAX_KEYS> commit_mu_;
  array<atomic<uint64_t>,MAX_KEYS> last_write_ts_;
  atomic<uint64_t> clock_{1}; array<atomic<uint64_t>,RING> done_; atomic<uint64_t> published_{0};
  array<atomic<uint64_t>,MAX_READERS> slots_; atomic<unsigned> next_slot_{0};
  atomic<LogNode*> log_head_{nullptr};
  mutex gc_mu_; condition_variable gc_cv_; atomic<bool> dirty_{false},running_{false}; thread gc_;
  WAL* wal_=nullptr; mutex wal_mu_;

  int find_index(const string& k) const { shared_lock lk(nm_); auto it=name2idx_.find(k); return it==name2idx_.end()?-1:it->second; }
  int ensure_index(const string& k){ unique_lock lk(nm_); auto it=name2idx_.find(k); if(it!=name2idx_.end())return it->second;
    int idx=(int)name2idx_.size(); if(idx>=(int)MAX_KEYS)throw runtime_error("MAX_KEYS"); name2idx_[k]=idx; ordered_idx_[k]=idx; return idx; }
  void wake_gc(){ dirty_.store(true,memory_order_release); gc_cv_.notify_one(); }
  void log_push(uint64_t ts){ LogNode* n=new LogNode(ts,nullptr); LogNode* h=log_head_.load(memory_order_relaxed);
    do{n->next.store(h,memory_order_relaxed);}while(!log_head_.compare_exchange_weak(h,n,memory_order_release,memory_order_relaxed)); }
  bool in_log(uint64_t ts) const { LogNode* c=log_head_.load(memory_order_acquire); while(c){if(c->ts==ts)return true; c=c->next.load(memory_order_acquire);} return false; }
  uint64_t gc_threshold() const { uint64_t p=published_.load(memory_order_seq_cst),rm=UINT64_MAX;
    for(size_t i=0;i<MAX_READERS;++i){uint64_t v=slots_[i].load(memory_order_seq_cst); if(v!=0)rm=min(rm,v-1);} return min(rm,p); }
  void advance_frontier(){ uint64_t cur=published_.load(memory_order_seq_cst); for(;;){ uint64_t nxt=cur+1;
    bool complete=(done_[nxt%RING].load(memory_order_acquire)==nxt)||in_log(nxt); if(!complete)break;
    if(published_.compare_exchange_weak(cur,nxt,memory_order_seq_cst,memory_order_seq_cst))cur=nxt; } }
  void gc_once(){ uint64_t m=gc_threshold(); size_t n; {shared_lock lk(nm_); n=name2idx_.size();}
    for(size_t i=0;i<n;++i){ Version* cur=heads_[i].load(memory_order_acquire); bool anchor=false;
      while(cur){ Version* nxt=cur->prev.load(memory_order_acquire); uint64_t c=cur->commit_ts.load(memory_order_acquire);
        if(c==0){cur=nxt;continue;} if(!anchor&&c<=m){anchor=true;cur->prev.store(nullptr,memory_order_release);cur=nxt;continue;}
        if(anchor)delete cur; cur=nxt; } } }
  void free_all(){ for(size_t i=0;i<MAX_KEYS;++i){Version* c=heads_[i].load(memory_order_relaxed); while(c){Version* n=c->prev.load(memory_order_relaxed); delete c; c=n;} heads_[i].store(nullptr,memory_order_relaxed);}
    LogNode* ln=log_head_.load(memory_order_relaxed); while(ln){LogNode* n=ln->next.load(memory_order_relaxed); delete ln; ln=n;} }
  void link_version(int idx,uint64_t ts,const string& v,bool deleted=false){ Version* n=new Version(ts,v,deleted,heads_[idx].load(memory_order_relaxed));
    heads_[idx].store(n,memory_order_release); last_write_ts_[idx].store(ts,memory_order_relaxed); }

public:
  ChronoKV(const string& wal_path=""){ if(!wal_path.empty()) wal_=new WAL(wal_path);
    for(auto& h:heads_)h.store(nullptr,memory_order_relaxed); for(auto& d:done_)d.store(0,memory_order_relaxed);
    for(auto& s:slots_)s.store(0,memory_order_relaxed); for(auto& w:last_write_ts_)w.store(0,memory_order_relaxed); }
  ~ChronoKV(){ {lock_guard<mutex> lk(gc_mu_); running_.store(false,memory_order_release);} gc_cv_.notify_all();
    if(gc_.joinable())gc_.join(); free_all(); delete wal_; }
  void start_gc(){ running_.store(true,memory_order_release); gc_=thread([this]{ unique_lock<mutex> lk(gc_mu_);
    while(running_.load(memory_order_acquire)){ gc_cv_.wait(lk,[this]{return !running_.load(memory_order_acquire)||dirty_.load(memory_order_acquire);});
      if(!running_.load(memory_order_acquire))break; dirty_.store(false,memory_order_release); lk.unlock(); gc_once(); lk.lock(); } }); }

  // v12: rebuild in-memory state from the intact WAL prefix
  void recover(const string& wal_path){ auto rec=wal_recover2(wal_path);
    if(rec.first==WalStatus::CORRUPT)throw runtime_error("WAL CORRUPTION DETECTED - refusing silent recovery");
    uint64_t mx=0;
    for(auto&[ts,ws]:rec.second){ for(auto&[k,v,d]:ws) link_version(ensure_index(k),ts,v,d);
      done_[ts%RING].store(ts,memory_order_release); mx=max(mx,ts); }
    if(mx>0){ clock_.store(mx+1,memory_order_relaxed); published_.store(mx,memory_order_seq_cst); } }

  void commit(const string& key,const string& value){ commit_txn(~0ull,{{key,value,false}},{}); }
  void del(const string& key){ commit_txn(~0ull,{{key,"",true}},{}); }

  bool commit_txn(uint64_t read_ts,const WriteSet& ws,const std::set<std::string>& rs){
    if(ws.empty())return true;
    vector<tuple<int,string,string,bool>> witems; for(auto&[k,v,d]:ws)witems.emplace_back(ensure_index(k),k,v,d);
    set<int> all_idx; for(auto&[idx,k,v,d]:witems)all_idx.insert(idx); for(auto&k:rs)all_idx.insert(ensure_index(k));
    for(auto i:all_idx)commit_mu_[i].lock();   // lock read-set UNION write-set, sorted (std::set is ordered)
    // one pass validates BOTH: write-set (first-committer-wins) and read-set (SSI rw-antidependency)
    for(auto i:all_idx) if(last_write_ts_[i].load(memory_order_relaxed)>read_ts){ for(auto j:all_idx)commit_mu_[j].unlock(); return false; }
    uint64_t cts=clock_.fetch_add(1,memory_order_relaxed); WIDEN_YIELD();
    if(wal_){wal_->group_append(cts,ws);}          // <-- DURABILITY POINT (before apply)
    for(auto&[idx,k,v,d]:witems) link_version(idx,cts,v,d);   // link the WRITE set only; read-only keys were locked, not written
    for(auto i:all_idx)commit_mu_[i].unlock();
    done_[cts%RING].store(cts,memory_order_release); log_push(cts); advance_frontier(); wake_gc(); return true; }


  void checkpoint(const string& ckpt_path){
    vector<tuple<string,uint64_t,string,bool>> snap; uint64_t cts=published_.load(memory_order_seq_cst);
    {shared_lock lk(nm_); for(auto&[k,idx]:name2idx_){Version* h=heads_[idx].load(memory_order_acquire); if(h){snap.push_back({k,h->commit_ts.load(memory_order_acquire),h->value,h->deleted});}}}
    vector<uint8_t> pl; auto u64=[&](uint64_t v){for(int i=0;i<8;++i)pl.push_back((v>>(8*i))&0xFF);};
    auto u32=[&](uint32_t v){for(int i=0;i<4;++i)pl.push_back((v>>(8*i))&0xFF);};
    auto u16=[&](uint16_t v){for(int i=0;i<2;++i)pl.push_back((v>>(8*i))&0xFF);};
    u64(cts); u32((uint32_t)snap.size());
    for(auto&[k,hc,v,dl]:snap){u16((uint16_t)k.size());for(char x:k)pl.push_back((uint8_t)x);u64(hc);u32((uint32_t)v.size());for(char x:v)pl.push_back((uint8_t)x);pl.push_back(dl?1:0);}
    uint32_t crc=crc32(pl.data(),pl.size()); vector<uint8_t> buf;
    auto p32=[&](uint32_t v){for(int i=0;i<4;++i)buf.push_back((v>>(8*i))&0xFF);};
    p32(0x434B5054); p32(crc); buf.insert(buf.end(),pl.begin(),pl.end());
    string tmp=ckpt_path+".tmp"; int fd=open(tmp.c_str(),O_CREAT|O_WRONLY|O_TRUNC,0644);
    write_all(fd,buf.data(),buf.size()); fsync(fd); close(fd);
    rename(tmp.c_str(),ckpt_path.c_str()); fsync_dir(ckpt_path);
  }
  void recover_with_checkpoint(const string& wp,const string& ckpt_path){
    uint64_t ckpt_cts=0; ifstream cf(ckpt_path,ios::binary);
    if(cf){vector<uint8_t> buf((istreambuf_iterator<char>(cf)),istreambuf_iterator<char>());
      if(buf.size()>=8){uint32_t magic=0,crc=0; for(int i=0;i<4;++i)magic|=((uint32_t)buf[i])<<(8*i); for(int i=0;i<4;++i)crc|=((uint32_t)buf[4+i])<<(8*i);
        if(magic==0x434B5054&&crc32(&buf[8],buf.size()-8)==crc){size_t q=8;
          auto pu64=[&](){uint64_t v=0;for(int i=0;i<8;++i)v|=((uint64_t)buf[q++])<<(8*i);return v;};
          auto pu32=[&](){uint32_t v=0;for(int i=0;i<4;++i)v|=((uint32_t)buf[q++])<<(8*i);return v;};
          auto pu16=[&](){uint16_t v=0;for(int i=0;i<2;++i)v|=((uint16_t)buf[q++])<<(8*i);return v;};
          ckpt_cts=pu64(); uint32_t nk=pu32();
          for(uint32_t i=0;i<nk;++i){uint16_t kl=pu16();string k((char*)&buf[q],kl);q+=kl;uint64_t hc=pu64();uint32_t vl=pu32();string v((char*)&buf[q],vl);q+=vl;bool dl=buf[q++]!=0;link_version(ensure_index(k),hc,v,dl);}}}}
    auto rec=wal_recover2(wp); if(rec.first==WalStatus::CORRUPT)throw runtime_error("WAL CORRUPTION DETECTED");
    uint64_t mx=ckpt_cts; for(auto&[ts,ws]:rec.second){if(ts<=ckpt_cts)continue; for(auto&[k,v,d]:ws)link_version(ensure_index(k),ts,v,d); done_[ts%RING].store(ts,memory_order_release); mx=max(mx,ts);}
    if(mx>0){clock_.store(mx+1,memory_order_relaxed);published_.store(mx,memory_order_seq_cst);}
  }
  std::map<string,int> ordered_idx_;
  vector<pair<string,string>> range_scan(uint64_t read_ts,const string& lo,const string& hi){ // half-open [lo, hi)

    vector<pair<string,int>> keys; {shared_lock lk(nm_); auto it=ordered_idx_.lower_bound(lo); while(it!=ordered_idx_.end()&&it->first<hi){keys.push_back({it->first,it->second});++it;}}
    vector<pair<string,string>> out; for(auto&[k,idx]:keys){auto v=read_at_idx(read_ts,idx); if(v)out.push_back({k,*v});} return out; }
  int acquire_slot(uint64_t& read_ts){ thread_local int slot=-1; if(slot<0){slot=(int)next_slot_.fetch_add(1,memory_order_relaxed); if(slot>=(int)MAX_READERS)throw runtime_error("readers");}
    uint64_t r; for(;;){ r=published_.load(memory_order_seq_cst); WIDEN_YIELD(); slots_[slot].store(r+1,memory_order_seq_cst); if(published_.load(memory_order_seq_cst)==r)break; } read_ts=r; return slot; }
  void release_slot(int slot){ slots_[slot].store(0,memory_order_seq_cst); }
  optional<string> read_at_idx(uint64_t read_ts,int idx) const { if(idx<0)return nullopt; Version* cur=heads_[idx].load(memory_order_acquire); while(cur){uint64_t c=cur->commit_ts.load(memory_order_acquire); if(c!=0&&c<=read_ts){ if(cur->deleted)return nullopt; return cur->value;} cur=cur->prev.load(memory_order_acquire);} return nullopt; }
  optional<string> read_at(uint64_t read_ts,const string& key) const { return read_at_idx(read_ts,find_index(key)); }
  optional<string> read(const string& key){ uint64_t r; int s=acquire_slot(r); auto res=read_at(r,key); release_slot(s); return res; }
};

class ReadWriteTransaction{ ChronoKV& kv_; uint64_t read_ts_; int slot_; map<string,pair<string,bool>> ws_; std::set<std::string> rs_; bool fin_=false; public:
  explicit ReadWriteTransaction(ChronoKV& kv):kv_(kv){ slot_=kv_.acquire_slot(read_ts_); }
  ~ReadWriteTransaction(){ if(!fin_)kv_.release_slot(slot_); }
  optional<string> read(const string& k){ auto it=ws_.find(k); if(it!=ws_.end())return it->second.second?nullopt:optional<string>(it->second.first); rs_.insert(k); return kv_.read_at(read_ts_,k); }
  void write(const string& k,const string& v){ ws_[k]={v,false}; }
  void del(const string& k){ ws_[k]={"",true}; }
  bool commit(){ fin_=true; WriteSet ws; for(auto&[k,vd]:ws_)ws.push_back({k,vd.first,vd.second}); bool ok=kv_.commit_txn(read_ts_,ws,rs_); kv_.release_slot(slot_); return ok; } };

int main(){
  crc_init(); const string wp="v12.wal"; int fails=0;
  auto report=[&](const char* n,bool ok){ cout<<"  "<<n<<": "<<(ok?"PASS":"FAIL")<<"\n"; if(!ok)fails++; };
  cout<<"v12 — durable transactional store\n";

  // 1. durable multi-key transaction survives crash + recovery
  { unlink(wp.c_str());
    { ChronoKV kv(wp); kv.start_gc(); kv.commit("a","1");
      bool ok=false; while(!ok){ ReadWriteTransaction t(kv); auto a=t.read("a"); auto b=kv.read("b");
        t.write("a", to_string(stoi(*a)+10)); t.write("b","100"); ok=t.commit(); } }   // crash on scope exit
    ChronoKV kv(wp); kv.recover(wp); kv.start_gc();
    report("durable txn recovered (a=11,b=100)", kv.read("a")&&*kv.read("a")=="11"&&kv.read("b")&&*kv.read("b")=="100"); }

  // 2. concurrent durable commits, crash, recover, none lost
  { unlink(wp.c_str()); constexpr int W=4,M=300;
    { ChronoKV kv(wp); kv.start_gc(); atomic<bool> go{false}; vector<thread> ts;
      for(int w=0;w<W;++w) ts.emplace_back([&,w]{ while(!go.load())this_thread::yield();
        for(int i=0;i<M;++i) kv.commit("k"+to_string(w)+"_"+to_string(i),"v"); });
      go.store(true); for(auto&t:ts)t.join(); }   // crash
    ChronoKV kv(wp); kv.recover(wp);
    int present=0; for(int w=0;w<W;++w)for(int i=0;i<M;++i) if(kv.read("k"+to_string(w)+"_"+to_string(i)))present++;
    report("concurrent commits all recovered", present==W*M); cout<<"    ("<<present<<"/"<<W*M<<")\n"; }

  { const string wp2="/tmp/v12_ckpt.wal", cp2="/tmp/v12.ckpt";
    unlink(wp2.c_str()); unlink(cp2.c_str());
    { ChronoKV kv(wp2); kv.start_gc(); kv.commit("a","1"); kv.commit("b","2");
      kv.checkpoint(cp2); kv.commit("a","3"); }                 // ckpt{a=1,b=2}, WAL tail a=3, crash
    ChronoKV kv(wp2); kv.recover_with_checkpoint(wp2,cp2);
    auto a=kv.read("a"), b=kv.read("b");
    report("checkpoint+WAL recovery (a=3 from WAL, b=2 from ckpt)", a&&*a=="3"&&b&&*b=="2"); }

  // 4. write-skew (two doctors, x+y>=1): SSI must abort the second committer
  { const string wp3="/tmp/v12_skew.wal"; unlink(wp3.c_str());
    ChronoKV kv(wp3); kv.start_gc(); kv.commit("x","1"); kv.commit("y","1");
    ReadWriteTransaction t1(kv); ReadWriteTransaction t2(kv);   // both snapshot BEFORE either commits
    (void)t1.read("x"); (void)t1.read("y"); (void)t2.read("x"); (void)t2.read("y");
    t1.write("x","0"); t2.write("y","0");                       // disjoint writes
    bool ok1=t1.commit(); bool ok2=t2.commit();                 // SSI aborts t2 (t1 wrote x, which t2 read)
    auto xf=kv.read("x"), yf=kv.read("y"); int x=xf?stoi(*xf):-1, y=yf?stoi(*yf):-1;
    report("write-skew prevented (ok1=1,ok2=0,x+y>=1)", ok1&&!ok2&&(x+y>=1)); }

  // 5. deletes + range scans: delete survives crash; range scan in key order, tombstone skipped
  { const string wp4="/tmp/v12_del.wal", cp4="/tmp/v12_del.ckpt"; unlink(wp4.c_str()); unlink(cp4.c_str());
    { ChronoKV kv(wp4); kv.start_gc(); kv.commit("a","1"); kv.commit("b","2"); kv.commit("c","3");
      kv.del("b"); kv.checkpoint(cp4); }                       // delete b, checkpoint, crash
    ChronoKV kv(wp4); kv.recover_with_checkpoint(wp4,cp4);
    auto b=kv.read("b"); bool del_ok=!b.has_value();           // b still deleted after recovery
    uint64_t r; int s=kv.acquire_slot(r); auto scan=kv.range_scan(r,"a","z"); kv.release_slot(s);
    bool scan_ok=scan.size()==2&&scan[0].first=="a"&&scan[1].first=="c";  // a,c in order; b skipped
    report("delete survives crash + ordered range scan", del_ok&&scan_ok); }

  // 6. concurrent SSI write-skew: both transactions start at the SAME snapshot
  //    (same read_ts), both read both keys, then race to commit disjoint writes.
  //    Under plain SI, both would commit -> x+y=0 (constraint broken).
  //    Under SSI, the rw-antidependency aborts one -> x+y>=1.
  { const string wp5="/tmp/v12_skew_conc.wal"; unlink(wp5.c_str());
    ChronoKV kv(wp5); kv.start_gc(); kv.commit("x","1"); kv.commit("y","1");
    // published_=2 now. Both transactions below will capture read_ts=2.
    ReadWriteTransaction t1(kv), t2(kv);
    (void)t1.read("x"); (void)t1.read("y");
    (void)t2.read("x"); (void)t2.read("y");
    // Both transactions have read_ts=2, both have seen x=1,y=1.
    // Now race them to commit concurrently — one must abort.
    atomic<int> results{0};  // bits: t1_committed (1), t2_committed (2)
    thread r1([&]{ t1.write("x","0"); if(t1.commit()) results.fetch_or(1); });
    thread r2([&]{ t2.write("y","0"); if(t2.commit()) results.fetch_or(2); });
    r1.join(); r2.join();
    int r = results.load();
    bool exactly_one_committed = (r == 1 || r == 2);  // not 0 (both aborted), not 3 (both committed)
    auto xf=kv.read("x"), yf=kv.read("y");
    int x=xf?stoi(*xf):-1, y=yf?stoi(*yf):-1;
    report("concurrent SSI write-skew (exactly one commits, x+y>=1)", exactly_one_committed && (x+y>=1));
    cout<<"    (results="<<r<<" x="<<x<<" y="<<y<<")\n";
  }
  cout<<(fails==0?"\nV12 FULL MARRIAGE VERIFIED\n":"\nFAILURES: "+to_string(fails)+"\n");
  return fails==0?0:1;
}
