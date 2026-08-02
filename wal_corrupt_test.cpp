// wal_corrupt_test.cpp — #5: torn-tail vs mid-file corruption discrimination.
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
using namespace std;

static uint32_t CRCT[256];
static void crc_init(){for(uint32_t i=0;i<256;++i){uint32_t c=i;for(int k=0;k<8;++k)c=(c&1)?(0xEDB88320^(c>>1)):(c>>1);CRCT[i]=c;}}
static uint32_t crc32(const uint8_t*d,size_t n){uint32_t c=0xFFFFFFFF;for(size_t i=0;i<n;++i)c=CRCT[(c^d[i])&0xFF]^(c>>8);return c^0xFFFFFFFF;}
using WriteSet=vector<pair<string,string>>;
static vector<uint8_t> wal_ser(uint64_t ts,const WriteSet&ws){vector<uint8_t>o;
 auto u64=[&](uint64_t v){for(int i=0;i<8;++i)o.push_back((v>>(8*i))&0xFF);};
 auto u32=[&](uint32_t v){for(int i=0;i<4;++i)o.push_back((v>>(8*i))&0xFF);};
 auto u16=[&](uint16_t v){for(int i=0;i<2;++i)o.push_back((v>>(8*i))&0xFF);};
 u64(ts);u32((uint32_t)ws.size());for(auto&[k,v]:ws){u16((uint16_t)k.size());for(char c:k)o.push_back((uint8_t)c);u32((uint32_t)v.size());for(char c:v)o.push_back((uint8_t)c);}return o;}
static vector<uint8_t> wal_frame(const vector<uint8_t>&p){vector<uint8_t>o;uint32_t len=(uint32_t)p.size(),crc=crc32(p.data(),p.size());for(int i=0;i<4;++i)o.push_back((len>>(8*i))&0xFF);for(int i=0;i<4;++i)o.push_back((crc>>(8*i))&0xFF);o.insert(o.end(),p.begin(),p.end());return o;}
static void write_all(int fd,const uint8_t*d,size_t n){while(n>0){ssize_t w=write(fd,d,n);if(w<=0)break;d+=w;n-=w;}}

enum class WalStatus{OK,TORN_TAIL,CORRUPT};
static const char* sname(WalStatus s){return s==WalStatus::OK?"OK":s==WalStatus::TORN_TAIL?"TORN_TAIL":"CORRUPT";}

// recovery that distinguishes a torn tail from mid-file corruption by reading ahead
static pair<WalStatus,vector<pair<uint64_t,WriteSet>>> wal_recover2(const string& path){
  vector<pair<uint64_t,WriteSet>> out;
  ifstream f(path,ios::binary); if(!f)return{WalStatus::OK,out};
  vector<uint8_t> buf((istreambuf_iterator<char>(f)),istreambuf_iterator<char>());
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
    for(uint32_t i=0;i<n;++i){uint16_t kl=pu16();string k((char*)&buf[q],kl);q+=kl;uint32_t vl=pu32();string v((char*)&buf[q],vl);q+=vl;ws.push_back({k,v});}
    p+=8+len;return true;
  };
  while(pos<buf.size()){
    uint64_t ts;WriteSet ws;size_t save=pos;
    if(parse(pos,ts,ws))out.push_back({ts,ws});
    else{
      // bad record at `save`: read ahead — is there a VALID record after it?
      uint32_t len=0;
      if(save+4<=buf.size())for(int i=0;i<4;++i)len|=((uint32_t)buf[save+i])<<(8*i);
      bool valid_after=false;
      if(len<=(1u<<20)&&save+8+len<buf.size()){size_t nxt=save+8+len;uint64_t t2;WriteSet w2;if(parse(nxt,t2,w2))valid_after=true;}
      return{valid_after?WalStatus::CORRUPT:WalStatus::TORN_TAIL,out};  // valid-after => corruption; else torn tail
    }
  }
  return{WalStatus::OK,out};
}

int main(){
  crc_init();
  const string wp="/tmp/wal_corrupt_test.wal";
  int fails=0;
  auto check=[&](const char*name,bool ok){cout<<"  "<<name<<": "<<(ok?"PASS":"FAIL")<<"\n";if(!ok)fails++;};
  auto build=[&](int n){vector<uint8_t> b;for(int i=1;i<=n;++i){auto x=wal_frame(wal_ser(i,{{"k","v"+to_string(i)}}));b.insert(b.end(),x.begin(),x.end());}return b;};
  auto wbuf=[&](const vector<uint8_t>& b){int fd=open(wp.c_str(),O_CREAT|O_WRONLY|O_TRUNC,0644);write_all(fd,b.data(),b.size());close(fd);};

  wbuf(build(3));
  {auto[s,r]=wal_recover2(wp);check("all intact        -> OK,        3 recovered",s==WalStatus::OK&&r.size()==3);}

  {auto b=build(3);b.resize(b.size()-5);wbuf(b);          // chop 5 bytes off the last record
   auto[s,r]=wal_recover2(wp);check("torn tail         -> TORN_TAIL, 2 recovered",s==WalStatus::TORN_TAIL&&r.size()==2);}

  {auto b=build(3);
   auto p1=wal_ser(1,{{"k","v1"}});size_t rec1=8+p1.size(); // record 2 starts here
   b[rec1+8+2]^=0xFF;                                       // flip a payload byte of record 2 -> CRC fails
   wbuf(b);
   auto[s,r]=wal_recover2(wp);check("mid-file corrupt  -> CORRUPT,   1 recovered",s==WalStatus::CORRUPT&&r.size()==1);}

  cout<<(fails==0?"\nALL #5 CRASH-INJECTION SCENARIOS PASS — corruption surfaced, torn tail handled\n":"\nFAILURES: "+to_string(fails)+"\n");
  return fails==0?0:1;
}
