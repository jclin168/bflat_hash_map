// Randomized differential test: bflat::unordered_flat_map vs std::unordered_map.
// Build variants with -DBFLAT_GROW_AT_ONCE_BELOW=0 (every growth is an
// incremental migration), -DBFLAT_MIGRATE_GROUPS=1 (long migrations) and small
// -DBFLAT_GROW_2X_BELOW / -DBFLAT_GROW_1_5X_BELOW (all growth tiers).
// Usage: test_diff [seed] [scale]
#include <bflat/unordered_flat_map.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
      if (++g_fail > 20) std::exit(1);                                        \
    }                                                                         \
  } while (0)

// Hash that collides heavily: 64 distinct values.
template <typename K>
struct BadHash {
  size_t operator()(const K& k) const { return std::hash<K>()(k) % 64; }
};
// Hash that keeps only a few low bits but is marked avalanching, so bflat
// uses it unmixed: home groups and reduced hashes collide too.
template <typename K>
struct WorseHash {
  using is_avalanching = std::true_type;
  size_t operator()(const K& k) const { return (std::hash<K>()(k) % 13) * 0x0101010101010101ull; }
};

template <typename K>
K make_key(uint64_t x);
template <>
uint64_t make_key<uint64_t>(uint64_t x) { return x; }
template <>
std::string make_key<std::string>(uint64_t x) {
  std::string s = "key_" + std::to_string(x);
  if (x % 3 == 0) s += std::string(20, 'x');  // some heap strings
  return s;
}

template <typename Map, typename Ref>
void compare_full(const Map& m, const Ref& ref) {
  CHECK(m.size() == ref.size());
  size_t n = 0;
  for (const auto& kv : m) {
    ++n;
    auto it = ref.find(kv.first);
    CHECK(it != ref.end());
    if (it != ref.end()) CHECK(it->second == kv.second);
  }
  CHECK(n == ref.size());
  for (const auto& kv : ref) {
    auto it = m.find(kv.first);
    CHECK(it != m.end());
    if (it != m.end()) CHECK(it->second == kv.second);
  }
  CHECK(m.check_invariants());
}

template <typename K, typename H>
void run_random(uint64_t seed, size_t ops, uint64_t keyspace, const char* name) {
  using Map = bflat::unordered_flat_map<K, uint64_t, H>;
  std::mt19937_64 rng(seed);
  Map m;
  std::unordered_map<K, uint64_t> ref;
  size_t migrating_ops = 0;
  for (size_t i = 0; i < ops; ++i) {
    if (m.is_migrating()) ++migrating_ops;
    const uint64_t r = rng() % 1000;
    const K k = make_key<K>(rng() % keyspace);
    const uint64_t v = rng();
    if (r < 250) {
      auto a = m.emplace(k, v);
      auto b = ref.emplace(k, v);
      CHECK(a.second == b.second);
      CHECK(a.first->first == k && a.first->second == b.first->second);
    } else if (r < 330) {
      auto a = m.insert(std::make_pair(k, v));
      auto b = ref.insert(std::make_pair(k, v));
      CHECK(a.second == b.second);
      CHECK(a.first->second == b.first->second);
    } else if (r < 380) {
      auto a = m.try_emplace(k, v);
      auto b = ref.try_emplace(k, v);
      CHECK(a.second == b.second);
      CHECK(a.first->second == b.first->second);
    } else if (r < 420) {
      auto a = m.insert_or_assign(k, v);
      auto b = ref.insert_or_assign(k, v);
      CHECK(a.second == b.second);
      CHECK(a.first->second == v);
    } else if (r < 460) {
      m[k] += v;
      ref[k] += v;
      CHECK(m[k] == ref[k]);
    } else if (r < 640) {
      CHECK(m.erase(k) == ref.erase(k));
    } else if (r < 680) {
      auto it = m.find(k);
      auto jt = ref.find(k);
      CHECK((it == m.end()) == (jt == ref.end()));
      if (it != m.end()) {
        m.erase(it);
        ref.erase(jt);
      }
    } else if (r < 900) {
      auto it = m.find(k);
      auto jt = ref.find(k);
      CHECK((it == m.end()) == (jt == ref.end()));
      if (it != m.end() && jt != ref.end()) CHECK(it->second == jt->second);
      CHECK(m.count(k) == ref.count(k));
      CHECK(m.contains(k) == (ref.count(k) == 1));
    } else if (r < 901) {
      if (rng() % 8) continue;
      // iterate + erase_if during whatever state
      const uint64_t mod = 4 + rng() % 7;
      size_t e1 = bflat::erase_if(m, [&](const typename Map::value_type& kv) { return kv.second % mod == 0; });
      size_t e2 = 0;
      for (auto it = ref.begin(); it != ref.end();) {
        if (it->second % mod == 0) { it = ref.erase(it); ++e2; } else ++it;
      }
      CHECK(e1 == e2);
    } else if (r < 903) {
      continue;
    } else if (r < 905) {
      if (rng() % 4) continue;
      m.rehash(rng() % (2 * ref.size() + 100));
    } else if (r < 907) {
      if (rng() % 4) continue;
      m.reserve(rng() % (3 * ref.size() + 100));
    } else if (r < 908) {
      if (rng() % 200 == 0) {
        m.clear();
        ref.clear();
      }
    } else if (r < 912) {
      if (rng() % 16) continue;
      Map c(m);  // copy (also of a migrating table)
      compare_full(c, ref);
      Map d;
      d = c;
      CHECK(d == m);
      Map e(std::move(c));
      CHECK(e == m);
      CHECK(c.empty());
      c.emplace(k, v);  // moved-from map is usable
      CHECK(c.size() == 1);
    } else if (r < 914) {
      if (rng() % 16) continue;
      Map other(m);
      Map tmp;
      tmp.swap(m);
      CHECK(m.empty());
      m = std::move(tmp);
      CHECK(m == other);
    } else if (r < 920) {
      if (rng() % 64) continue;
      compare_full(m, ref);
    } else {
      if (rng() % 32) continue;
      // erase a range of up to 5 elements from begin
      auto it = m.begin();
      size_t cnt = 0;
      while (it != m.end() && cnt < 5) { ++it; ++cnt; }
      std::vector<K> keys;
      for (auto jt = m.begin(); jt != it; ++jt) keys.push_back(jt->first);
      m.erase(m.cbegin(), typename Map::const_iterator(it));
      for (auto& kk : keys) ref.erase(kk);
    }
  }
  compare_full(m, ref);
  std::printf("  %-28s ops=%zu final_size=%zu migrating_ops=%zu\n", name, ops, ref.size(),
              migrating_ops);
}

// Grow from 0 to n with checks at many points, including during migrations.
template <typename K>
void run_growth(size_t n, uint64_t seed, const char* name) {
  using Map = bflat::unordered_flat_map<K, uint64_t>;
  std::mt19937_64 rng(seed);
  Map m;
  size_t next_check = 1, checks_migrating = 0;
  for (size_t i = 0; i < n; ++i) {
    auto r = m.emplace(make_key<K>(i * 2654435761ull), i);
    CHECK(r.second);
    if (i + 1 == next_check || (m.is_migrating() && rng() % 50000 == 0)) {
      if (m.is_migrating()) ++checks_migrating;
      CHECK(m.size() == i + 1);
      for (int t = 0; t < 2000; ++t) {
        const size_t j = rng() % (i + 1);
        auto it = m.find(make_key<K>(j * 2654435761ull));
        CHECK(it != m.end() && it->second == j);
        CHECK(m.find(make_key<K>((i + 1 + rng() % (n + 1)) * 2654435761ull)) == m.end());
      }
      if (i + 1 == next_check) next_check = next_check * 3 / 2 + 1;
    }
  }
  // Iteration during a migration if any is left.
  size_t cnt = 0;
  for (auto& kv : m) { (void)kv; ++cnt; }
  CHECK(cnt == n);
  // Erase the even values, re-insert half of them.
  for (size_t i = 0; i < n; i += 2) CHECK(m.erase(make_key<K>(i * 2654435761ull)) == 1);
  for (size_t i = 0; i < n; i += 4) CHECK(m.emplace(make_key<K>(i * 2654435761ull), i).second);
  size_t expect = 0;
  for (size_t i = 0; i < n; ++i) {
    const bool present = (i % 2 == 1) || (i % 4 == 0);
    expect += present;
    auto it = m.find(make_key<K>(i * 2654435761ull));
    CHECK((it != m.end()) == present);
  }
  CHECK(m.size() == expect);
  CHECK(m.check_invariants());
  std::printf("  %-28s n=%zu cap=%zu checks_during_migration=%zu\n", name, n, m.bucket_count(),
              checks_migrating);
}

int main(int argc, char** argv) {
  const uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  const double scale = argc > 2 ? std::atof(argv[2]) : 1.0;
  auto S = [&](double x) { return static_cast<size_t>(x * scale); };
  std::printf("bflat test_diff seed=%llu scale=%.2f grow_at_once_below=%llu migrate_groups=%d\n",
              (unsigned long long)seed, scale, (unsigned long long)BFLAT_GROW_AT_ONCE_BELOW,
              (int)BFLAT_MIGRATE_GROUPS);
  run_random<uint64_t, std::hash<uint64_t>>(seed, S(2000000), 300, "u64 small keyspace");
  run_random<uint64_t, std::hash<uint64_t>>(seed + 1, S(3000000), 200000, "u64 large keyspace");
  run_random<std::string, std::hash<std::string>>(seed + 2, S(1000000), 50000, "string");
  run_random<uint64_t, BadHash<uint64_t>>(seed + 3, S(300000), 4000, "u64 bad hash (64 values)");
  run_random<std::string, BadHash<std::string>>(seed + 4, S(100000), 2000, "string bad hash");
  run_random<uint64_t, WorseHash<uint64_t>>(seed + 5, S(100000), 1500, "u64 unmixed 13-value hash");
  run_growth<uint64_t>(S(20000000), seed + 6, "u64 growth 0..20M");
  run_growth<std::string>(S(2000000), seed + 7, "string growth 0..2M");
  std::printf("%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
