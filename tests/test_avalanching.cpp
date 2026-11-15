#include <bflat/unordered_flat_map.hpp>
#include <cassert>
#include <string>
struct VoidHash {using is_avalanching=void;size_t operator()(uint64_t x)const{return x;}};
struct TrueHash {using is_avalanching=std::true_type;};
struct FalseHash {using is_avalanching=std::false_type;};
struct PlainHash {};
static_assert(bflat::hash_is_avalanching<VoidHash>::value,"void marker");
static_assert(bflat::hash_is_avalanching<TrueHash>::value,"true marker");
static_assert(!bflat::hash_is_avalanching<FalseHash>::value,"false marker");
static_assert(!bflat::hash_is_avalanching<PlainHash>::value,"unmarked");
int main(){bflat::unordered_flat_map<uint64_t,uint64_t,VoidHash> m;m.reserve(1125);
for(size_t i=0;i<1000;++i)assert(m.emplace(i,i+1).second);
for(size_t i=0;i<1000;++i)assert(m.at(i)==i+1);
for(size_t i=0;i<1000;++i)assert(m.erase(i)==1);
assert(m.empty() && m.check_invariants());}
