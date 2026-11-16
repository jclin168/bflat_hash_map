// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
#include <bflat/unordered_flat_map.hpp>
#include <cstdio>
#include <cstdlib>
#include <random>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
int main(){
 using G=bflat::detail::group15;
 std::mt19937_64 rng(777);
 for(unsigned trial=0;trial<2000;++trial){
  G g{};
  const unsigned spread=trial%2?4:256;
  for(auto& b:g.m)b=static_cast<unsigned char>(rng()%spread);
  for(unsigned c=0;c<256;++c){
   auto mask=g.match(static_cast<uint8_t>(c));CHECK((mask&~G::kAll)==0);
   for(unsigned lane=0;lane<G::N;++lane){
    if(g.m[lane]!=c)continue;
    CHECK(mask!=0&&G::lane(mask)==lane);
    CHECK(G::byte_offset<16>(mask)==lane*16);
    CHECK(G::byte_offset<64>(mask)==lane*64);
    mask&=mask-1;
   }CHECK(mask==0);
  }
  auto available=g.match_available(),occupied=g.match_occupied();
  for(unsigned lane=0;lane<G::N;++lane){
   const auto bit=typename G::Mask(1)<<(lane<<G::kShift);
   CHECK(bool(available&bit)==(g.m[lane]==0));
   CHECK(bool(occupied&bit)==(g.m[lane]>1));
   CHECK(g.is_occupied(lane)==(g.m[lane]>1));
   for(unsigned start=0;start<=G::N;++start)
    CHECK(bool(G::from_lane(start)&bit)==(lane>=start));
  }
 }
 G g{};
 for(unsigned h=0;h<256;++h){
  g.m[G::N]=0;CHECK(g.is_not_overflowed(h));g.mark_overflow(h);
  CHECK(!g.is_not_overflowed(h));CHECK(g.m[G::N]==(1u<<(h%8)));
  for(unsigned lane=0;lane<G::N;++lane){
   const auto rh=bflat::detail::reduced_hash(h+lane);g.set(lane,rh);
   CHECK(g.maybe_caused_overflow(lane)==((rh%8)==(h%8)));
   g.reset(lane);CHECK(!g.is_occupied(lane));
  }
 }
 std::puts("SFLAT GROUP TESTS PASSED");
}
