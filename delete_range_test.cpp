// delete_range_test.cpp — #6: tombstones + ordered range scans, in isolation.
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <optional>
#include <iostream>
using namespace std;

struct Version{uint64_t ts; string value; bool deleted; Version* prev;};

struct Store{
  map<string,Version*> idx_;                 // ORDERED index -> range scans
  uint64_t clock_=1;
  ~Store(){for(auto&[k,h]:idx_){Version*c=h;while(c){Version*n=c->prev;delete c;c=n;}}}
  void put(const string&k,const string&v){uint64_t ts=clock_++;auto it=idx_.find(k);idx_[k]=new Version{ts,v,false,it!=idx_.end()?it->second:nullptr};}
  void del(const string&k){uint64_t ts=clock_++;auto it=idx_.find(k);idx_[k]=new Version{ts,"",true,it!=idx_.end()?it->second:nullptr};}   // tombstone
  optional<string> read_at(uint64_t rts,const string&k){auto it=idx_.find(k);if(it==idx_.end())return nullopt;Version*c=it->second;while(c){if(c->ts<=rts){if(c->deleted)return nullopt;return c->value;}c=c->prev;}return nullopt;}
  vector<pair<string,string>> range(uint64_t rts,const string&lo,const string&hi){
    vector<pair<string,string>> out;
    for(auto it=idx_.lower_bound(lo); it!=idx_.end()&&it->first<hi; ++it){auto v=read_at(rts,it->first); if(v)out.push_back({it->first,*v});}
    return out;}
};

int main(){
  Store st;
  st.put("a","1"); st.put("b","2"); st.put("c","3");
  uint64_t s1=st.clock_-1;        // a=1,b=2,c=3
  st.del("b");                    // tombstone b
  uint64_t s2=st.clock_-1;        // a=1,b=DELETED,c=3
  st.put("d","4");
  uint64_t s3=st.clock_-1;        // a=1,b=DELETED,c=3,d=4

  int fails=0; auto chk=[&](const char*n,bool ok){cout<<"  "<<n<<": "<<(ok?"PASS":"FAIL")<<"\n";if(!ok)fails++;};
  auto r1=st.range(s1,"a","z");
  chk("range[a,z)@s1 -> a,b,c in key order", r1.size()==3&&r1[0].first=="a"&&r1[1].first=="b"&&r1[2].first=="c");
  chk("read b@s2 -> deleted (tombstone)", !st.read_at(s2,"b").has_value());
  chk("read b@s1 -> 2 (pre-delete snapshot sees old value)", st.read_at(s1,"b")&&*st.read_at(s1,"b")=="2");
  auto r2=st.range(s2,"a","z");
  chk("range[a,z)@s2 -> a,c (tombstone skipped)", r2.size()==2&&r2[0].first=="a"&&r2[1].first=="c");
  auto r3=st.range(s3,"b","d");
  chk("range[b,d)@s3 -> c only (b deleted, d out of bounds)", r3.size()==1&&r3[0].first=="c");
  cout<<(fails==0?"\nALL #6 SCENARIOS PASS\n":"\nFAILURES\n");
  return fails?1:0;
}
