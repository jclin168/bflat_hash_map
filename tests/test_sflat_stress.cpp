// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
#include <bflat/unordered_flat_map.hpp>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort(); } } while (0)
template<class A,class B> bool maps_equal(const A& a,const B& b){
 if(a.size()!=b.size())return false;
 for(const auto& x:a){auto i=b.find(x.first);if(i==b.end()||i->second!=x.second)return false;}
 return true;
}
void test_sequential_keys() {

  bflat::unordered_flat_map<uint64_t, uint64_t> m;
  const uint64_t n = 200000;
  m.reserve(n);
  for (uint64_t i = 0; i < n; ++i) m.emplace(i, i + 1);
  CHECK(m.size() == n);
  for (uint64_t i = 0; i < n; ++i) {
    auto it = m.find(i);
    CHECK(it != m.end() && it->second == i + 1);
  }

  for (uint64_t i = n; i < n + 1000; ++i) CHECK(m.find(i) == m.end());
  std::printf("test_sequential_keys OK\n");
}

void test_self_aliasing() {
  using Map = bflat::unordered_flat_map<std::string, std::string>;
  auto key = [](int i) { return "key_long_enough_to_skip_sso_" + std::to_string(i); };
  auto val = [](int i) { return "val_long_enough_to_skip_sso_" + std::to_string(i); };
  for (int n = 1; n < 600; ++n) {
    for (int op = 0; op < 3; ++op) {

      Map m;
      for (int i = 0; i < n; ++i) m.emplace(key(i), val(i));
      for (int e = 0; e < 40; ++e) {
        const int src = e % n;
        const std::string& v = m.at(key(src));
        if (op == 0) m.try_emplace(key(n + e), v);
        else if (op == 1) m.insert_or_assign(key(n + e), v);
        else m.emplace(key(n + e), v);
        CHECK(m.at(key(n + e)) == val(src));
      }

      Map k;
      for (int i = 0; i < n; ++i) {
        k.emplace(key(i), val(i));
        const auto& kv = *k.find(key(i / 2));
        const size_t before = k.size();
        if (op == 0) k[kv.first];
        else if (op == 1) k.insert(kv);
        else k.try_emplace(kv.first, "x");
        CHECK(k.size() == before);
      }
      for (int i = 0; i < n; ++i) CHECK(k.at(key(i)) == val(i));
    }
  }
  std::printf("test_self_aliasing OK\n");
}

struct KeyCopies {
  static int copies;
  uint64_t v;
  explicit KeyCopies(uint64_t x) : v(x) {}
  KeyCopies(const KeyCopies& o) : v(o.v) { ++copies; }
  KeyCopies(KeyCopies&& o) noexcept : v(o.v) {}
  bool operator==(const KeyCopies& o) const { return v == o.v; }
};
int KeyCopies::copies = 0;
struct KeyCopiesHash {
  size_t operator()(const KeyCopies& k) const { return std::hash<uint64_t>()(k.v); }
};

void test_pair_emplace_copies() {
  using M = bflat::unordered_flat_map<KeyCopies, int, KeyCopiesHash>;
  M m;
  m.reserve(100);
  const std::pair<KeyCopies, int> p(KeyCopies(1), 10);
  const std::pair<const KeyCopies, int> v(KeyCopies(2), 20);
  KeyCopies::copies = 0;
  CHECK(m.emplace(p).second && KeyCopies::copies == 1);
  CHECK(m.insert(v).second && KeyCopies::copies == 2);
  CHECK(m.emplace(std::pair<KeyCopies, int>(KeyCopies(3), 30)).second &&
        KeyCopies::copies == 2);
  CHECK(m.insert(std::pair<KeyCopies, int>(KeyCopies(4), 40)).second &&
        KeyCopies::copies == 2);

  CHECK(!m.emplace(p).second && !m.insert(p).second && !m.emplace(v).second);
  CHECK(KeyCopies::copies == 2);
  CHECK(m.size() == 4 && m.at(KeyCopies(1)) == 10 && m.at(KeyCopies(4)) == 40);

  for (uint64_t i = 5; i < 5000; ++i)
    CHECK(m.emplace(std::pair<KeyCopies, int>(KeyCopies(i), int(i))).second);
  for (uint64_t i = 5; i < 5000; ++i) CHECK(m.at(KeyCopies(i)) == int(i));
  std::printf("test_pair_emplace_copies OK\n");
}

void test_churn_fixed_size() {
  std::mt19937_64 rng(2026);
  for (size_t n : {30u, 900u, 1500u, 1790u, 3000u, 14000u, 28600u, 57000u}) {
    bflat::unordered_flat_map<uint64_t, std::string> m;
    m.reserve(n + n/8 + (n%8 != 0));
    std::unordered_map<uint64_t, std::string> ref;
    std::vector<uint64_t> live;
    while (live.size() < n) {
      const uint64_t k = rng() >> 1;
      if (ref.emplace(k, std::to_string(k)).second) {
        m.emplace(k, std::to_string(k));
        live.push_back(k);
      }
    }
    const size_t buckets = m.bucket_count();
    for (size_t i = 0; i < 20 * n + 20000; ++i) {
      const size_t at = rng() % live.size();
      CHECK(m.erase(live[at]) == 1);
      ref.erase(live[at]);
      uint64_t k;
      do {
        k = rng() >> 1;
      } while (ref.count(k));
      const std::string v = std::string(40, 'x') + std::to_string(k);
      CHECK(m.try_emplace(k, v).second);
      ref.emplace(k, v);
      live[at] = k;
      if (i % 97 == 0) {
        const uint64_t h = live[rng() % live.size()];
        auto it = m.find(h);
        CHECK(it != m.end() && it->second == ref[h]);
        CHECK(m.find(rng() | (1ull << 63)) == m.end());
      }
    }
    CHECK(m.bucket_count() == buckets);
    CHECK(maps_equal(m, ref));
    for (uint64_t k : live) CHECK(m.count(k) == 1);
  }
  std::printf("test_churn_fixed_size OK\n");
}

void test_erase_others_while_iterating() {
  for (size_t n : {10u, 100u, 5000u}) {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    std::unordered_map<uint64_t, uint64_t> alive;
    for (uint64_t i = 0; i < n; ++i) {
      m.emplace(i * 7919, i);
      alive.emplace(i * 7919, i);
    }
    std::unordered_map<uint64_t, int> visited;
    size_t step = 0;
    for (auto it = m.begin(); it != m.end(); ++it, ++step) {
      CHECK(alive.count(it->first) == 1);
      CHECK(++visited[it->first] == 1);
      if (step % 4 == 0) {
        std::vector<uint64_t> drop;
        for (const auto& kv : alive)
          if (kv.first != it->first && !visited.count(kv.first) &&
              kv.second % 3 == step % 3)
            drop.push_back(kv.first);
        for (uint64_t k : drop) {
          CHECK(m.erase(k) == 1);
          alive.erase(k);
        }
      }
    }
    for (const auto& kv : alive) CHECK(visited.count(kv.first) == 1);
    CHECK(m.size() == alive.size());
  }
  std::printf("test_erase_others_while_iterating OK\n");
}
void test_structured_keys() {
 const uint64_t n=16384;
 for(unsigned pattern=0;pattern<7;++pattern){
  auto key=[&](uint64_t i){switch(pattern){
   case 0:return i;case 1:return i*1000;case 2:return UINT64_C(0x7f0000000000)+i*16;
   case 3:return UINT64_C(0x7f0000000000)+i*64;case 4:return i<<8;
   case 5:return i<<32;default:return i<<48;}};
  bflat::unordered_flat_map<uint64_t,uint64_t> m;m.reserve(n+n/8);
  for(uint64_t i=0;i<n;++i)CHECK(m.emplace(key(i),i).second);
  for(uint64_t i=0;i<n;++i)CHECK(m.at(key(i))==i);
  for(uint64_t i=n;i<n+1000;++i)CHECK(!m.contains(key(i)));
  for(uint64_t i=0;i<n;i+=2)CHECK(m.erase(key(i))==1);
  for(uint64_t i=0;i<n;i+=2)CHECK(m.emplace(key(i),i+1).second);
  m.finish_migration();CHECK(m.size()==n&&m.check_invariants());
  for(uint64_t i=0;i<n;++i)CHECK(m.at(key(i))==i+(i%2==0));
 }
}
void test_copy_clear_shrink(){
 using M=bflat::unordered_flat_map<uint64_t,std::string>;
 M a;std::mt19937_64 rng(42);
 for(size_t i=0;i<20000;++i){const auto k=rng();a.emplace(k,std::string(64,'x')+std::to_string(k));}
 a.finish_migration();M b=a;b.clear();b.rehash(0);
 for(const auto& x:a){CHECK(b.emplace(x).second);if((b.size()%7)==0)b.erase(x.first);}
 b.finish_migration();CHECK(b.check_invariants());
 for(const auto& x:b)CHECK(a.at(x.first)==x.second);
 CHECK(a.size()==20000);
}
int main(){test_sequential_keys();test_structured_keys();test_pair_emplace_copies();
 test_self_aliasing();test_churn_fixed_size();test_erase_others_while_iterating();test_copy_clear_shrink();
 std::puts("SFLAT STRESS TESTS PASSED");}
