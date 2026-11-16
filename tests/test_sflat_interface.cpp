// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin

#include <bflat/unordered_flat_map.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::fflush(stdout);                                               \
      std::abort();                                                      \
    }                                                                    \
  } while (0)

template <typename M>
std::vector<std::pair<typename M::key_type, typename M::mapped_type>> sorted(const M& m) {
  std::vector<std::pair<typename M::key_type, typename M::mapped_type>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end());
  return v;
}

struct SeededHash {
  size_t seed = 0;
  SeededHash() = default;
  explicit SeededHash(size_t s) : seed(s) {}
  size_t operator()(int k) const { return std::hash<int>()(k) ^ seed; }
};
struct ModEqual {
  bool operator()(int a, int b) const { return a % 1000 == b % 1000; }
};
struct ModHash {
  size_t operator()(int k) const { return std::hash<int>()(k % 1000); }
};

struct NoDefault {
  explicit NoDefault(int v) : v(v) {}
  int v;
};

template <template <typename...> class Map>
void test_member_types() {
  using M = Map<int, std::string>;
  static_assert(std::is_same<typename M::key_type, int>::value, "");
  static_assert(std::is_same<typename M::mapped_type, std::string>::value, "");
  static_assert(std::is_same<typename M::value_type,
                             std::pair<const int, std::string>>::value, "");
  static_assert(std::is_same<typename M::size_type, size_t>::value, "");
  static_assert(std::is_same<typename M::difference_type, std::ptrdiff_t>::value, "");
  static_assert(std::is_same<typename M::hasher, std::hash<int>>::value, "");
  static_assert(std::is_same<typename M::key_equal, std::equal_to<int>>::value, "");
  static_assert(std::is_same<typename M::allocator_type,
                             std::allocator<std::pair<const int, std::string>>>::value, "");
  static_assert(std::is_same<typename M::reference, typename M::value_type&>::value, "");
  static_assert(std::is_same<typename M::const_reference,
                             const typename M::value_type&>::value, "");
  static_assert(std::is_same<typename M::pointer, typename M::value_type*>::value, "");
  static_assert(std::is_same<typename M::const_pointer,
                             const typename M::value_type*>::value, "");
  using It = typename M::iterator;
  using CIt = typename M::const_iterator;
  static_assert(std::is_base_of<std::forward_iterator_tag,
                                typename std::iterator_traits<It>::iterator_category>::value, "");
  static_assert(std::is_base_of<std::forward_iterator_tag,
                                typename std::iterator_traits<CIt>::iterator_category>::value, "");
  static_assert(std::is_same<typename std::iterator_traits<It>::value_type,
                             typename M::value_type>::value, "");
  static_assert(std::is_same<decltype(*std::declval<It>()), typename M::value_type&>::value, "");
  static_assert(std::is_same<decltype(*std::declval<CIt>()),
                             const typename M::value_type&>::value, "");
  static_assert(std::is_convertible<It, CIt>::value, "");
  static_assert(std::is_default_constructible<It>::value, "");

}

template <template <typename...> class Map>
void test_constructors() {
  using M = Map<int, int>;
  using A = typename M::allocator_type;
  const std::vector<std::pair<const int, int>> src{{1, 10}, {2, 20}, {3, 30}, {2, 99}};

  M a;
  CHECK(a.empty() && a.size() == 0 && a.begin() == a.end());
  M b(100);
  CHECK(b.empty() && b.bucket_count() >= 100);
  M c(10, std::hash<int>());
  M d(10, std::hash<int>(), std::equal_to<int>());
  M e(10, std::hash<int>(), std::equal_to<int>(), A());
  M f(10, A());
  M g(10, std::hash<int>(), A());
  M h{A()};
  CHECK(c.empty() && d.empty() && e.empty() && f.empty() && g.empty() && h.empty());

  M r1(src.begin(), src.end());
  CHECK(r1.size() == 3 && r1.at(2) == 20);
  M r2(src.begin(), src.end(), 10);
  M r3(src.begin(), src.end(), 10, std::hash<int>());
  M r4(src.begin(), src.end(), 10, std::hash<int>(), std::equal_to<int>(), A());
  M r5(src.begin(), src.end(), 10, A());
  M r6(src.begin(), src.end(), 10, std::hash<int>(), A());
  CHECK(sorted(r2) == sorted(r1) && sorted(r3) == sorted(r1) && sorted(r4) == sorted(r1));
  CHECK(sorted(r5) == sorted(r1) && sorted(r6) == sorted(r1));

  M l1{{1, 10}, {2, 20}, {3, 30}, {2, 99}};
  M l2({{1, 10}, {2, 20}, {3, 30}}, 10);
  M l3({{1, 10}, {2, 20}, {3, 30}}, 10, std::hash<int>());
  M l4({{1, 10}, {2, 20}, {3, 30}}, 10, A());
  M l5({{1, 10}, {2, 20}, {3, 30}}, 10, std::hash<int>(), A());
  CHECK(sorted(l1) == sorted(r1) && sorted(l2) == sorted(r1) && sorted(l3) == sorted(r1));
  CHECK(sorted(l4) == sorted(r1) && sorted(l5) == sorted(r1));

  M cp(l1);
  CHECK(cp == l1);
  M cpa(l1, A());
  CHECK(cpa == l1);
  M mv(std::move(cp));
  CHECK(mv == l1);
  M mva(std::move(cpa), A());
  CHECK(mva == l1);

  Map<int, int, SeededHash> sh(10, SeededHash(12345));
  CHECK(sh.hash_function().seed == 12345);
  Map<int, int, ModHash, ModEqual> mod;
  mod.emplace(5, 1);
  CHECK(!mod.emplace(1005, 2).second);
  CHECK(mod.find(2005) != mod.end() && mod.find(2005)->second == 1);
}

template <template <typename...> class Map>
void test_assignment_and_swap() {
  using M = Map<int, std::string>;
  M a{{1, "a"}, {2, "b"}};
  M b{{3, "c"}};
  b = a;
  CHECK(b == a && b.size() == 2);
  M& self = b;
  b = self;
  CHECK(b == a);
  M c;
  c = std::move(b);
  CHECK(c == a);
  c = {{7, "x"}, {8, "y"}, {9, "z"}};
  CHECK(c.size() == 3 && c.at(8) == "y");

  M x{{1, "one"}};
  M y{{2, "two"}, {3, "three"}};
  x.swap(y);
  CHECK(x.size() == 2 && y.size() == 1 && y.at(1) == "one");
  using std::swap;
  swap(x, y);
  CHECK(x.size() == 1 && x.at(1) == "one");
  static_assert(noexcept(x.swap(y)), "swap of std::allocator containers is noexcept");
}

template <template <typename...> class Map>
void test_iterators() {
  using M = Map<int, int>;
  M m;
  for (int i = 0; i < 1000; ++i) m.emplace(i, i * 2);

  std::vector<int> seen;
  for (auto it = m.begin(); it != m.end(); ++it) seen.push_back(it->first);
  std::sort(seen.begin(), seen.end());
  CHECK(seen.size() == 1000 && seen.front() == 0 && seen.back() == 999);
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
  CHECK(static_cast<size_t>(std::distance(m.cbegin(), m.cend())) == m.size());
  const M& cm = m;
  CHECK(static_cast<size_t>(std::distance(cm.begin(), cm.end())) == m.size());

  auto it = m.begin();
  auto old = it++;
  CHECK(old != it && old == m.begin());
  typename M::const_iterator ci = m.begin();
  CHECK(ci == m.begin() && m.begin() == ci);

  for (auto& kv : m) kv.second += 1;
  CHECK(m.at(10) == 21);

  auto keep = m.find(999);
  size_t visited = 0;
  for (auto e = m.begin(); e != m.end();) {
    ++visited;
    if (e->first % 2 == 0) e = m.erase(e);
    else ++e;
  }
  CHECK(visited == 1000 && m.size() == 500);
  CHECK(keep->first == 999 && keep->second == 1999);

  M empty;
  CHECK(empty.begin() == empty.end() && empty.cbegin() == empty.cend());
}

template <template <typename...> class Map>
void test_capacity() {
  Map<int, int> m;
  CHECK(m.empty() && m.size() == 0 && m.max_size() > 1000000);
  m[1] = 1;
  CHECK(!m.empty() && m.size() == 1);
}

template <template <typename...> class Map>
void test_insert() {
  using M = Map<int, std::string>;
  using V = typename M::value_type;
  M m;

  const V v1(1, "one");
  auto r = m.insert(v1);
  CHECK(r.second && r.first->first == 1 && r.first->second == "one");
  r = m.insert(V(1, "uno"));
  CHECK(!r.second && r.first->second == "one");
  r = m.insert(std::make_pair(2, "two"));
  CHECK(r.second && m.at(2) == "two");

  auto it = m.insert(m.cbegin(), V(3, "three"));
  CHECK(it->first == 3);
  it = m.insert(m.cend(), V(3, "drei"));
  CHECK(it->second == "three");

  std::vector<V> more{{4, "four"}, {5, "five"}, {1, "x"}};
  m.insert(more.begin(), more.end());
  m.insert({{6, "six"}, {7, "seven"}});
  CHECK(m.size() == 7 && m.at(1) == "one" && m.at(7) == "seven");
}

template <template <typename...> class Map>
void test_emplace_family() {
  using M = Map<int, std::string>;
  M m;
  auto r = m.emplace(1, "one");
  CHECK(r.second && r.first->second == "one");
  r = m.emplace(1, "uno");
  CHECK(!r.second && r.first->second == "one");
  r = m.emplace(std::piecewise_construct, std::forward_as_tuple(2),
                std::forward_as_tuple(3, 'x'));
  CHECK(r.second && m.at(2) == "xxx");
  r = m.emplace(std::make_pair(3, std::string("three")));
  CHECK(r.second);
  auto it = m.emplace_hint(m.cbegin(), 4, "four");
  CHECK(it->first == 4 && it->second == "four");
  it = m.emplace_hint(m.cbegin(), 4, "vier");
  CHECK(it->second == "four");

  std::string s = "moved?";
  r = m.try_emplace(1, std::move(s));
  CHECK(!r.second && s == "moved?" && m.at(1) == "one");
  r = m.try_emplace(5, 2, 'y');
  CHECK(r.second && m.at(5) == "yy");
  int k6 = 6;
  r = m.try_emplace(std::move(k6), "six");
  CHECK(r.second && m.at(6) == "six");
  it = m.try_emplace(m.cbegin(), 7, "seven");
  CHECK(it->second == "seven");
  it = m.try_emplace(m.cbegin(), 8, "eight");
  CHECK(it->first == 8);

  r = m.insert_or_assign(1, std::string("ONE"));
  CHECK(!r.second && m.at(1) == "ONE");
  r = m.insert_or_assign(9, "nine");
  CHECK(r.second && m.at(9) == "nine");
  int k10 = 10;
  r = m.insert_or_assign(std::move(k10), "ten");
  CHECK(r.second);
  it = m.insert_or_assign(m.cbegin(), 10, "TEN");
  CHECK(it->second == "TEN");
  it = m.insert_or_assign(m.cbegin(), 11, "eleven");
  CHECK(it->first == 11 && m.size() == 11);

  Map<int, NoDefault> nd;
  nd.emplace(1, NoDefault(5));
  nd.try_emplace(2, 6);
  nd.insert_or_assign(1, NoDefault(7));
  CHECK(nd.at(1).v == 7 && nd.at(2).v == 6);
  Map<int, std::unique_ptr<int>> up;
  up.emplace(1, std::make_unique<int>(1));
  up.try_emplace(2, std::make_unique<int>(2));
  up[3] = std::make_unique<int>(3);
  up.insert_or_assign(1, std::make_unique<int>(10));
  for (int i = 4; i < 300; ++i) up.try_emplace(i, std::make_unique<int>(i));
  CHECK(*up.at(1) == 10 && *up.at(3) == 3 && *up.at(299) == 299);
  Map<int, std::unique_ptr<int>> up2(std::move(up));
  CHECK(up2.size() == 299);
}

template <template <typename...> class Map>
void test_erase_clear() {
  using M = Map<int, int>;
  M m;
  for (int i = 0; i < 100; ++i) m[i] = i;
  CHECK(m.erase(5) == 1 && m.erase(5) == 0 && m.size() == 99);

  auto it = m.find(10);
  auto next = std::next(it);
  auto r = m.erase(it);
  CHECK(r == next && m.count(10) == 0);
  typename M::const_iterator cit = m.find(11);
  m.erase(cit);
  CHECK(m.count(11) == 0);

  auto first = m.begin();
  auto last = std::next(first, 10);
  std::vector<int> gone;
  for (auto i = first; i != last; ++i) gone.push_back(i->first);
  auto after = m.erase(first, last);
  CHECK(after == last);
  for (int k : gone) CHECK(m.count(k) == 0);
  CHECK(m.size() == 87);
  CHECK(m.erase(m.begin(), m.begin()) == m.begin());
  m.erase(m.begin(), m.end());
  CHECK(m.empty());
  for (int i = 0; i < 100; ++i) m[i] = i;
  m.clear();
  CHECK(m.empty() && m.begin() == m.end() && m.find(1) == m.end());
  m[1] = 2;
  CHECK(m.size() == 1);
}

template <template <typename...> class Map>
void test_lookup() {
  using M = Map<std::string, int>;
  M m{{"a", 1}, {"b", 2}};
  const M& cm = m;
  CHECK(m.at("a") == 1 && cm.at("b") == 2);
  m.at("a") = 5;
  CHECK(m.at("a") == 5);
  bool threw = false;
  try {
    (void)m.at("zz");
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    (void)cm.at("zz");
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw);

  CHECK(m["c"] == 0 && m.size() == 3);
  std::string key = "d";
  m[std::move(key)] = 4;
  CHECK(m.at("d") == 4);

  CHECK(m.count("a") == 1 && m.count("zz") == 0);
  CHECK(cm.find("b") != cm.end() && cm.find("zz") == cm.end());
  auto er = m.equal_range("b");
  CHECK(std::distance(er.first, er.second) == 1 && er.first->second == 2);
  auto cer = cm.equal_range("zz");
  CHECK(cer.first == cer.second && cer.first == cm.end());
#if __cplusplus >= 202002L
  CHECK(m.contains("a") && !m.contains("zz"));
#endif
}

template <template <typename...> class Map>
void test_observers() {
  Map<int, int, SeededHash> m(0, SeededHash(42));
  CHECK(m.hash_function().seed == 42);
  CHECK(m.hash_function()(5) == SeededHash(42)(5));
  Map<int, int, ModHash, ModEqual> e;
  CHECK(e.key_eq()(3, 1003) && !e.key_eq()(3, 4));
  Map<int, int> a;
  using A = std::allocator<std::pair<const int, int>>;
  CHECK(a.get_allocator() == A());
}

template <template <typename...> class Map>
void test_comparison() {
  using M = Map<int, int>;
  M a{{1, 1}, {2, 2}, {3, 3}};
  M b{{3, 3}, {2, 2}, {1, 1}};
  CHECK(a == b && !(a != b));
  b[3] = 4;
  CHECK(a != b && !(a == b));
  b.erase(3);
  CHECK(a != b);
  M e1, e2;
  CHECK(e1 == e2);
}

template <template <typename...> class Map>
void run_all(const char* name) {
  test_member_types<Map>();
  test_constructors<Map>();
  test_assignment_and_swap<Map>();
  test_iterators<Map>();
  test_capacity<Map>();
  test_insert<Map>();
  test_emplace_family<Map>();
  test_erase_clear<Map>();
  test_lookup<Map>();
  test_observers<Map>();
  test_comparison<Map>();
  std::printf("%s: all interface tests OK\n", name);
}

template <typename K, typename V, typename H = std::hash<K>,
          typename E = std::equal_to<K>>
using StdMap = std::unordered_map<K, V, H, E>;
template <typename K, typename V, typename H = std::hash<K>,
          typename E = std::equal_to<K>>
using BflatMap = bflat::unordered_flat_map<K, V, H, E>;

int main() {
  run_all<StdMap>("std::unordered_map");
  run_all<BflatMap>("bflat::unordered_flat_map");
  std::printf("ALL MAP INTERFACE TESTS PASSED\n");
  return 0;
}
