// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
#include <bflat/unordered_flat_map.hpp>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
template <typename T>
struct InputOnly {
  using iterator_category = std::input_iterator_tag;
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = const T*;
  using reference = const T&;
  const T* p;
  reference operator*() const { return *p; }
  InputOnly& operator++() { ++p; return *this; }
  InputOnly operator++(int) { InputOnly t = *this; ++p; return t; }
  bool operator==(const InputOnly& o) const { return p == o.p; }
  bool operator!=(const InputOnly& o) const { return p != o.p; }
};
void test_range_insert() {
  using M = bflat::unordered_flat_map<uint64_t, uint64_t>;
  const uint64_t n = 500000;
  M a;
  for (uint64_t i = 0; i < n; ++i) a[i] = i * 3;
  M b(a.begin(), a.end());
  CHECK(b == a);

  const size_t bc = b.bucket_count();
  b.insert(a.begin(), a.end());
  CHECK(b.bucket_count() == bc && b == a);

  b.insert(b.begin(), b.end());
  CHECK(b.bucket_count() == bc && b == a);

  M r;
  r.reserve(100000);
  const size_t rbc = r.bucket_count();
  std::vector<std::pair<uint64_t, uint64_t>> few = {{1, 1}, {2, 2}, {3, 3}};
  r.insert(few.begin(), few.end());
  CHECK(r.bucket_count() == rbc && r.size() == 3);

  M o{{1, 100}, {5, 500}};
  o.insert(few.begin(), few.end());
  CHECK(o.size() == 4 && o.at(1) == 100 && o.at(2) == 2 && o.at(5) == 500);

  std::vector<std::pair<uint64_t, uint64_t>> v;
  for (uint64_t i = 0; i < 1000; ++i) v.push_back({i % 700, i});
  M in;
  in.insert(InputOnly<std::pair<uint64_t, uint64_t>>{v.data()},
            InputOnly<std::pair<uint64_t, uint64_t>>{v.data() + v.size()});
  CHECK(in.size() == 700);
  bool first_wins = true;
  for (uint64_t k = 0; k < 700; ++k) first_wins = first_wins && in.at(k) == k;
  CHECK(first_wins);
  M ctor(InputOnly<std::pair<uint64_t, uint64_t>>{v.data()},
         InputOnly<std::pair<uint64_t, uint64_t>>{v.data() + v.size()});
  CHECK(ctor == in);
  std::puts("test_range_insert OK");
}
void test_during_migration() {
  using Map = bflat::unordered_flat_map<uint64_t, std::string>;
  auto fill = [](Map& m) {
    uint64_t k = 0;
    size_t bc = 0;
    for (;;) {
      m.emplace(k, std::to_string(k));
      ++k;
      if (m.size() > 5000 && bc && m.bucket_count() != bc) return k;
      bc = m.bucket_count();
    }
  };
  auto all_present = [](const Map& m, uint64_t n) {
    if (m.size() != n) return false;
    for (uint64_t i = 0; i < n; ++i) {
      auto it = m.find(i);
      if (it == m.end() || it->second != std::to_string(i)) return false;
    }
    return true;
  };
  {
    Map m;
    const uint64_t n = fill(m);
    Map c(m);
    CHECK(all_present(c, n));
    CHECK(static_cast<size_t>(std::distance(c.begin(), c.end())) == n);
    Map m2;
    fill(m2);
    Map d;
    d = m2;
    CHECK(all_present(d, n));
  }
  {
    Map m;
    const uint64_t n = fill(m);
    Map c(std::move(m));
    CHECK(all_present(c, n));
    Map m2;
    fill(m2);
    Map d;
    d = std::move(m2);
    CHECK(all_present(d, n));
  }
  {
    Map m;
    const uint64_t n = fill(m);
    m.clear();
    CHECK(m.empty());
    for (uint64_t i = 0; i < n; ++i) CHECK(m.find(i) == m.end());
    CHECK(m.begin() == m.end());
  }
  {
    Map m;
    const uint64_t n = fill(m);
    size_t cnt = 0;
    for (auto it = m.cbegin(); it != m.cend(); ++it) ++cnt;
    CHECK(cnt == n);
  }
  {
    Map m;
    const uint64_t n = fill(m);
    for (uint64_t i = 0; i < 100; ++i)
      CHECK(!m.insert_or_assign(uint64_t(i), std::string("x")).second);
    CHECK(m.size() == n);
    for (uint64_t i = 0; i < 100; ++i) CHECK(m.at(i) == "x");
  }
  std::printf("test_during_migration OK\n");
}
void test_collision_order() {
  using M = bflat::unordered_flat_map<uint64_t, std::string>;
  const uint64_t n = 500000;
  M a;
  for (uint64_t i = 0; i < n; ++i) a[i * 7] = std::to_string(i);
  M b = a;
  b.clear();
  b.rehash(0);
  std::vector<uint64_t> erased;
  uint64_t prev = 0, k = 0;
  for (const auto& kv : a) {
    b[kv.first] = kv.second;
    if (++k % 64 == 0) {
      b.erase(prev);
      erased.push_back(prev);
    }
    prev = kv.first;
  }
  CHECK(b.size() == n - erased.size());
  CHECK(static_cast<size_t>(std::distance(b.begin(), b.end())) == b.size());
  for (uint64_t x : erased) CHECK(b.find(x) == b.end());
  size_t found = 0;
  for (const auto& kv : a) {
    auto it = b.find(kv.first);
    if (it != b.end()) {
      CHECK(it->second == kv.second);
      ++found;
    }
  }
  CHECK(found == b.size());




  struct ConstHash {
    size_t operator()(uint64_t) const { return 42; }
  };
  bflat::unordered_flat_map<uint64_t, uint64_t, ConstHash> c;
  const uint64_t nc = 10000;
  for (uint64_t i = 0; i < nc; ++i) c[i] = i;
  CHECK(c.size() == nc);
  bool all = true;
  for (uint64_t i = 0; i < nc; ++i) all = all && c.find(i) != c.end() && c.at(i) == i;
  CHECK(all);
  std::puts("test_collision_order OK");
}
int main(){test_range_insert();test_during_migration();test_collision_order();std::puts("SFLAT LIFECYCLE TESTS PASSED");}
