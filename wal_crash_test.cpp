// wal_crash_test.cpp — WAL implementation + crash-injection instrument.
// Implements the WAL_DESIGN.md record format ([len:u32][crc:u32][payload]),
// fsync, and recovery (replay the intact prefix), then injects crash states and
// asserts the durability invariant: recovered == intact prefix, no more, no less.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
using namespace std;

// ---- CRC-32 (reflected, poly 0xEDB88320) ----
static uint32_t T[256];
static void crc_init(){ for(uint32_t i=0;i<256;++i){ uint32_t c=i; for(int k=0;k<8;++k) c=(c&1)?(0xEDB88320^(c>>1)):(c>>1); T[i]=c; } }
static uint32_t crc32(const uint8_t* d, size_t n){ uint32_t c=0xFFFFFFFF; for(size_t i=0;i<n;++i) c=T[(c^d[i])&0xFF]^(c>>8); return c^0xFFFFFFFF; }

struct Record { uint64_t ts; string key, value; };

static vector<uint8_t> serialize(const Record& r){
  vector<uint8_t> o;
  auto u64=[&](uint64_t v){ for(int i=0;i<8;++i) o.push_back((v>>(8*i))&0xFF); };
  auto u32=[&](uint32_t v){ for(int i=0;i<4;++i) o.push_back((v>>(8*i))&0xFF); };
  auto str=[&](const string& s){ u32((uint32_t)s.size()); for(char c:s) o.push_back((uint8_t)c); };
  u64(r.ts); str(r.key); str(r.value); return o;
}
static vector<uint8_t> frame(const vector<uint8_t>& p){
  vector<uint8_t> o; uint32_t len=(uint32_t)p.size(), crc=crc32(p.data(),p.size());
  for(int i=0;i<4;++i) o.push_back((len>>(8*i))&0xFF);
  for(int i=0;i<4;++i) o.push_back((crc>>(8*i))&0xFF);
  o.insert(o.end(), p.begin(), p.end()); return o;
}
static void write_all(int fd, const uint8_t* d, size_t n){ while(n>0){ ssize_t w=write(fd,d,n); if(w<=0) break; d+=w; n-=w; } }

class WAL {
  int fd_;
public:
  WAL(const string& path){ fd_=open(path.c_str(), O_CREAT|O_WRONLY|O_APPEND, 0644); }
  ~WAL(){ if(fd_>=0) close(fd_); }
  void append(const Record& r){ auto b=frame(serialize(r)); write_all(fd_, b.data(), b.size()); }
  void append_raw(const vector<uint8_t>& b){ write_all(fd_, b.data(), b.size()); }  // inject torn/corrupt
  void sync(){ fsync(fd_); }
};

// recovery: replay the intact prefix; ignore_checksum = the broken control
static vector<Record> recover(const string& path, bool ignore_checksum){
  vector<Record> out; ifstream f(path, ios::binary); if(!f) return out;
  auto ru32=[&](uint32_t& v)->bool{ uint8_t b[4]; if(!f.read((char*)b,4)) return false; v=0; for(int i=0;i<4;++i) v|=((uint32_t)b[i])<<(8*i); return true; };
  while(true){
    uint32_t len,crc; if(!ru32(len)) break; if(len>(1u<<20)) break; if(!ru32(crc)) break;
    vector<uint8_t> p(len); if(!f.read((char*)p.data(),len)) break;          // torn: short payload
    if(!ignore_checksum && crc32(p.data(),len)!=crc) break;                  // corrupt/torn: bad crc
    size_t pos=0; Record r;
    r.ts=0; for(int i=0;i<8;++i) r.ts|=((uint64_t)p[pos++])<<(8*i);
    auto rdstr=[&](string& s){ uint32_t n=0; for(int i=0;i<4;++i) n|=((uint32_t)p[pos++])<<(8*i); s.assign((char*)&p[pos],n); pos+=n; };
    rdstr(r.key); rdstr(r.value); out.push_back(r);
  }
  return out;
}

int main(){
  crc_init(); const string path="test.wal"; int fails=0;
  auto report=[&](const char* name, bool ok, size_t n){
    cout << "  " << name << ": recovered " << n << " record(s) — " << (ok?"PASS":"FAIL") << "\n"; if(!ok) fails++; };

  cout << "crash-injection instrument\n";

  // 1. NO LESS — three durable (fsync'd) records; crash after the last fsync, before any "apply".
  { unlink(path.c_str()); WAL w(path);
    w.append({1,"a","v1"}); w.sync(); w.append({2,"a","v2"}); w.sync(); w.append({3,"a","v3"}); w.sync();
    auto r=recover(path,false);
    report("no-less (3 intact)", r.size()==3 && r[0].ts==1 && r[2].ts==3, r.size()); }

  // 2. NO MORE — crash mid-write of record 3 (torn tail); only 1,2 may be recovered.
  { unlink(path.c_str()); WAL w(path);
    w.append({1,"a","v1"}); w.sync(); w.append({2,"a","v2"}); w.sync();
    auto b=frame(serialize({3,"a","v3"})); b.resize(b.size()/2); w.append_raw(b);  // torn, no fsync
    auto r=recover(path,false);
    report("no-more (torn tail)", r.size()==2 && r[1].ts==2, r.size()); }

  // 3. NO MORE — a bit-flipped CRC on record 2; recovery must stop at record 1.
  { unlink(path.c_str()); WAL w(path);
    w.append({1,"a","v1"}); w.sync();
    auto b=frame(serialize({2,"a","v2"})); b[5]^=0xFF; w.append_raw(b); w.sync();  // corrupt crc
    auto r=recover(path,false);
    report("no-more (bad crc)", r.size()==1 && r[0].ts==1, r.size()); }

  // 4. REAL CRASH — fork a child that writes+fsyncs then _exit()s (crash before any apply).
  { unlink(path.c_str());
    pid_t pid=fork();
    if(pid==0){ WAL w(path); w.append({7,"k","durable"}); w.sync(); _exit(0); }  // crash post-fsync
    int st; waitpid(pid,&st,0);
    auto r=recover(path,false);
    report("real fork+kill after fsync", r.size()==1 && r[0].ts==7 && r[0].value=="durable", r.size()); }

  cout << (fails==0 ? "\nALL CRASH-INJECTION SCENARIOS PASS — visible == durable, no more, no less\n"
                    : "\nFAILURES: " + to_string(fails) + "\n");
  return fails==0?0:1;
}
