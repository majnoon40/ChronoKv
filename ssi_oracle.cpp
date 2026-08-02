// ssi_oracle.cpp — #4: write-skew under SI vs its prevention under SSI.
// Two doctors, constraint x+y>=1. Both read x=1,y=1; T1 writes x=0, T2 writes y=0.
// SI  (no read-set check): both commit -> x=0,y=0, constraint BROKEN (write-skew).
// SSI (read-set check):    the second committer detects the rw-antidependency
//                          (the other wrote a key I read, after my snapshot) and
//                          aborts -> one doctor stays on call, constraint HOLDS.
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <iostream>
using namespace std;

struct SSIStore{
  unordered_map<string,pair<uint64_t,string>> data_;   // key -> (commit_ts, value)
  unordered_map<string,uint64_t> lwt_;                 // key -> last write ts
  uint64_t clock_=1; bool ssi_;
  SSIStore(bool ssi):ssi_(ssi){}
  void init(const string& k,const string& v){data_[k]={0,v}; lwt_[k]=0;}
  string read(const string& k){return data_[k].second;}
  // commit: write set ws, read set rs, at snapshot read_ts
  bool commit(uint64_t read_ts,const vector<pair<string,string>>& ws,const vector<string>& rs){
    for(auto&[k,v]:ws) if(lwt_[k]>read_ts) return false;        // first-committer-wins (write set)
    if(ssi_) for(auto& k:rs) if(lwt_[k]>read_ts) return false;  // SSI: rw-antidependency (read set)
    uint64_t cts=clock_++;
    for(auto&[k,v]:ws){data_[k]={cts,v}; lwt_[k]=cts;}
    return true;
  }
};

static void run(bool ssi,const char* name){
  SSIStore st(ssi); st.init("x","1"); st.init("y","1");
  uint64_t rts=0;                                   // both snapshot the same state
  st.read("x"); st.read("y");                       // T1 reads x,y
  bool ok1=st.commit(rts,{{"x","0"}},{"x","y"});    // T1 writes x=0
  st.read("x"); st.read("y");                       // T2 reads x,y
  bool ok2=st.commit(rts,{{"y","0"}},{"x","y"});    // T2 writes y=0
  int x=stoi(st.read("x")), y=stoi(st.read("y"));
  bool holds=(x+y>=1);
  cout<<"  "<<name<<": commit1="<<ok1<<" commit2="<<ok2<<" x="<<x<<" y="<<y
      <<" -> "<<(holds?"constraint HOLDS":"WRITE-SKEW — constraint BROKEN")<<"\n";
  // SI must break it; SSI must hold it
  bool expect = ssi ? holds : !holds;
  cout<<"    "<<(expect?"PASS (behaves as predicted)":"FAIL")<<"\n";
}

int main(){
  cout<<"write-skew oracle (#4)\n";
  run(false,"SI ");   // control: write-skew must occur
  run(true ,"SSI");   // correct: write-skew must be prevented
  return 0;
}
