#include <bflat/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdio>
#include <cstdlib>
#include <random>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
struct Collision { size_t operator()(uint64_t x)const{return x%64;} };
template<class Hash> void history() {
 bflat::unordered_flat_map<uint64_t,uint64_t,Hash> a;
 boost::unordered_flat_map<uint64_t,uint64_t,Hash> b;
 a.reserve(500); b.reserve(500);
 std::mt19937_64 rng(168);
 for(size_t i=0;i<30000;++i){
  auto k=rng()%1000;
  if(rng()%2){CHECK(a.emplace(k,k).second==b.emplace(k,k).second);a.finish_migration();}
  else CHECK(a.erase(k)==b.erase(k));
  CHECK(a.size()==b.size());CHECK(a.max_load()>=a.size());
  CHECK(a.check_invariants());
 }
}
int main(){
 for(size_t n:{0,1,14,15,16,28,29,30,52,53,1000,100000,1000000,10000000}){
  bflat::unordered_flat_map<uint64_t,uint64_t> a;boost::unordered_flat_map<uint64_t,uint64_t> b;
  a.reserve(n);b.reserve(n);CHECK(a.max_load()>=n);
  a.clear();CHECK(a.check_invariants());a.emplace(1,2);a.finish_migration();CHECK(a.at(1)==2);
 }
 for(size_t ng=2;ng<=128;++ng)for(size_t home=0;home<ng;++home){
  bflat::detail::prober p(home);bool seen[128]={};size_t step=0;
  do{CHECK(p.pos<ng);seen[p.pos]=true;++step;}while(p.next(ng));CHECK(step<=2*ng);for(size_t i=0;i<ng;++i)CHECK(seen[i]);
 }
 history<std::hash<uint64_t>>();history<Collision>();
 std::puts("TIERED POLICY TESTS PASSED");
}
