// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
#include <bflat/unordered_flat_map.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

struct Key {
  static inline int copies = 0;
  static inline int moves = 0;
  std::string text;
  explicit Key(std::string s) : text(std::move(s)) {}
  Key(const Key& k) : text(k.text) { ++copies; }
  Key(Key&& k) noexcept : text(std::move(k.text)) { ++moves; }
  bool operator==(const Key& k) const { return text == k.text; }
};
struct KeyHash {
  size_t operator()(const Key& k) const { return std::hash<std::string>{}(k.text); }
};
using CountMap = bflat::unordered_flat_map<Key, int, KeyHash>;

void reset_counts() { Key::copies = Key::moves = 0; }

void test_direct_construction() {
  CountMap m;
  m.reserve(16);
  std::pair<Key, int> lvalue{Key(std::string(64, 'a')), 1};
  reset_counts();
  auto a = m.emplace(lvalue);
  CHECK(a.second && a.first->second == 1);
  CHECK(Key::copies == 1 && Key::moves == 0);

  const std::pair<Key, int> const_lvalue{Key(std::string(64, 'b')), 2};
  reset_counts();
  CHECK(m.emplace(const_lvalue).second);
  CHECK(Key::copies == 1 && Key::moves == 0);

  std::pair<Key, int> rvalue{Key(std::string(64, 'c')), 3};
  reset_counts();
  CHECK(m.emplace(std::move(rvalue)).second);
  CHECK(Key::copies == 0 && Key::moves == 1);

  const std::pair<Key, int> const_rvalue{Key(std::string(64, 'd')), 4};
  reset_counts();
  CHECK(m.emplace(std::move(const_rvalue)).second);
  CHECK(Key::copies == 1 && Key::moves == 0);

  CountMap::value_type stored{Key(std::string(64, 'e')), 5};
  reset_counts();
  CHECK(m.emplace(stored).second);
  CHECK(Key::copies == 1 && Key::moves == 0);

  CountMap::value_type stored_rvalue{Key(std::string(64, 'f')), 6};
  reset_counts();
  CHECK(m.emplace(std::move(stored_rvalue)).second);
  CHECK(Key::copies == 1 && Key::moves == 0);

  const CountMap::value_type const_stored{Key(std::string(64, 'g')), 7};
  reset_counts();
  CHECK(m.emplace(const_stored).second);
  CHECK(Key::copies == 1 && Key::moves == 0);
  CHECK(m.size() == 7);
  for (char c = 'a'; c <= 'g'; ++c) {
    auto it = m.find(Key(std::string(64, c)));
    CHECK(it != m.end() && it->second == c - 'a' + 1);
  }
  reset_counts();
  auto duplicate = m.emplace(lvalue);
  CHECK(!duplicate.second && duplicate.first->second == 1 && m.size() == 7);
  CHECK(lvalue.first.text == std::string(64, 'a'));
}

void test_move_only_and_duplicates() {
  bflat::unordered_flat_map<std::string, std::unique_ptr<int>> m;
  m.reserve(10);
  std::pair<std::string, std::unique_ptr<int>> p{"first", std::make_unique<int>(42)};
  CHECK(m.emplace(std::move(p)).second);
  CHECK(!p.second && *m.at("first") == 42);
  decltype(m)::value_type q{"second", std::make_unique<int>(17)};
  CHECK(m.emplace(std::move(q)).second);
  CHECK(!q.second && *m.at("second") == 17);
  std::pair<std::string, std::unique_ptr<int>> duplicate{"first", std::make_unique<int>(99)};
  auto r = m.emplace(std::move(duplicate));
  CHECK(!r.second && *r.first->second == 42 && m.size() == 2);
  auto it = m.find("first");
  auto alias = m.emplace(std::move(*it));
  CHECK(!alias.second && *m.at("first") == 42);
}

struct ConvertedPair {
  operator std::pair<const int, std::string>() const { return {4, "converted"}; }
};

void test_generic_fallbacks() {
  bflat::unordered_flat_map<int, std::string> m;
  m.reserve(20);
  std::pair<short, const char*> converted{1, "one"};
  CHECK(m.emplace(converted).second);
  CHECK(m.emplace(std::piecewise_construct, std::forward_as_tuple(2),
                  std::forward_as_tuple(5, 'x')).second);
  CHECK(m.emplace(3, "three").second);
  CHECK(m.emplace(ConvertedPair{}).second);
  CHECK(m.at(1) == "one" && m.at(2) == "xxxxx");
  CHECK(m.at(3) == "three" && m.at(4) == "converted");

  std::pair<volatile int, const char*> volatile_key{7, "volatile"};
  CHECK(m.emplace(volatile_key).second && m.at(7) == "volatile");
  bflat::unordered_flat_map<int, int> zero;
  CHECK(zero.emplace().second && zero.at(0) == 0);
  int key = 5;
  std::string value = "reference";
  std::pair<int&, std::string&> refs{key, value};
  CHECK(m.emplace(refs).second && m.at(5) == value);
  auto hinted = m.emplace_hint(m.end(), std::make_pair(6, std::string("hint")));
  CHECK(hinted->first == 6 && hinted->second == "hint");
}

void test_growth_alias_and_migration() {
  using M = bflat::unordered_flat_map<int, std::string>;
  M m;
  m.reserve(1);
  const size_t limit = m.max_load();
  for (size_t i = 0; i < limit; ++i)
    m.emplace(static_cast<int>(i), std::string(96, static_cast<char>('a' + i % 20)));
  const size_t capacity = m.bucket_count();
  const std::string expected = m.at(0);

  std::pair<int, const std::string&> alias{1000000, m.at(0)};
  CHECK(m.emplace(alias).second);
  CHECK(m.bucket_count() > capacity && m.at(1000000) == expected);
  m.finish_migration();
  for (int i = 1000001; i < 1010000; ++i) {
    const std::pair<int, std::string> p{i, std::to_string(i)};
    CHECK(m.emplace(p).second);
    if (i % 97 == 0) CHECK(m.at(1000000) == expected);
  }
  m.finish_migration();
  for (int i = 1000001; i < 1010000; ++i) CHECK(m.at(i) == std::to_string(i));
  CHECK(m.at(0) == expected && m.at(1000000) == expected);
}

struct Throwing {
  static inline int alive = 0;
  static inline bool fail = false;
  int value;
  explicit Throwing(int v) : value(v) { ++alive; }
  Throwing(const Throwing& v) : value(v.value) {
    if (fail) throw std::runtime_error("copy");
    ++alive;
  }
  Throwing(Throwing&& v) noexcept : value(v.value) { ++alive; }
  ~Throwing() { --alive; }
};

void test_exceptions() {
  CHECK(Throwing::alive == 0);
  {
    bflat::unordered_flat_map<int, Throwing> m;
    m.reserve(8);
    const std::pair<int, Throwing> p{1, Throwing(42)};
    Throwing::fail = true;
    bool caught = false;
    try { m.emplace(p); } catch (const std::runtime_error&) { caught = true; }
    Throwing::fail = false;
    CHECK(caught && m.empty() && m.find(1) == m.end());
    CHECK(Throwing::alive == 1);
    CHECK(m.emplace(p).second && m.at(1).value == 42);
    CHECK(Throwing::alive == 2);
  }
  CHECK(Throwing::alive == 0);

  {
    bflat::unordered_flat_map<int, Throwing> m;
    const std::pair<int, Throwing> p{2, Throwing(23)};
    Throwing::fail = true;
    bool caught = false;
    try { m.emplace(p); } catch (const std::runtime_error&) { caught = true; }
    Throwing::fail = false;
    CHECK(caught && m.empty() && Throwing::alive == 1);
    CHECK(m.emplace(p).second && m.at(2).value == 23);
  }
  CHECK(Throwing::alive == 0);
}

void test_move_only_keys() {
  using M = bflat::unordered_flat_map<std::unique_ptr<int>, int>;
  M m;
  for (int i = 0; i < 5000; ++i)
    CHECK(m.emplace(std::make_unique<int>(i), i).second);
  m.finish_migration();
  CHECK(m.size() == 5000);
  for (const auto& entry : m) {
    CHECK(*entry.first == entry.second);
    CHECK(m.find(entry.first)->second == entry.second);
  }
}

struct SparseStringHash {
  using is_avalanching = void;
  size_t operator()(const std::string& key) const {
    CHECK(!key.empty());
    return static_cast<size_t>(std::stoull(key));
  }
};

void test_sparse_hash_alias() {
  using M = bflat::unordered_flat_map<std::string, std::string, SparseStringHash>;
  M m;
  const std::string expected(96, 'x');
  CHECK(m.emplace("0", expected).second);
  for (int i = 1; i < 5000; ++i) {
    std::string key = std::to_string(i);
    CHECK(m.try_emplace(std::move(key), m.at("0")).second);
  }
  m.finish_migration();
  CHECK(m.size() == 5000 && m.check_invariants());
  for (int i = 0; i < 1000; ++i) {
    std::string key = std::to_string(i);
    std::string value(96, 'y');
    CHECK(!m.try_emplace(std::move(key), std::move(value)).second);
    CHECK(key == std::to_string(i));
    CHECK(value == std::string(96, 'y'));
  }
  for (int i = 0; i < 5000; ++i) CHECK(m.at(std::to_string(i)) == expected);
}

int main() {
  test_sparse_hash_alias();
  test_move_only_keys();
  test_direct_construction();
  test_move_only_and_duplicates();
  test_generic_fallbacks();
  test_growth_alias_and_migration();
  test_exceptions();
  std::puts("ALL EMPLACE PAIR TESTS PASSED");
}
