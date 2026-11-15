// API and edge cases of bflat::unordered_flat_map.
#include <bflat/unordered_flat_map.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                               \
    }                                                                         \
  } while (0)

static long g_live_bytes = 0;
template <typename T>
struct CountingAlloc {
  using value_type = T;
  CountingAlloc() = default;
  template <typename U>
  CountingAlloc(const CountingAlloc<U>&) {}
  T* allocate(size_t n) {
    g_live_bytes += static_cast<long>(n * sizeof(T));
    return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(alignof(T) < 16 ? 16 : alignof(T))));
  }
  void deallocate(T* p, size_t n) {
    g_live_bytes -= static_cast<long>(n * sizeof(T));
    ::operator delete(p, std::align_val_t(alignof(T) < 16 ? 16 : alignof(T)));
  }
  template <typename U>
  bool operator==(const CountingAlloc<U>&) const { return true; }
  template <typename U>
  bool operator!=(const CountingAlloc<U>&) const { return false; }
};

struct SvHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); }
};
struct SvEq {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const { return a == b; }
};

static double now_seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct IdentityHash {
  using is_avalanching = void;
  size_t operator()(uint64_t x) const noexcept { return static_cast<size_t>(x); }
};

// Time of inserting a's iteration order into an empty map, over the time of
// inserting the same keys in a random order.
template <typename H>
double iteration_order_ratio(const std::vector<uint64_t>& keys) {
  bflat::unordered_flat_map<uint64_t, uint64_t, H> a;
  for (uint64_t k : keys) a.emplace(k, k);
  std::vector<uint64_t> shuffled(keys);
  std::mt19937_64 rng(1);
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  double t0 = now_seconds();
  bflat::unordered_flat_map<uint64_t, uint64_t, H> b;
  for (auto& kv : a) b[kv.first] = kv.second;
  double t1 = now_seconds();
  bflat::unordered_flat_map<uint64_t, uint64_t, H> c;
  for (uint64_t k : shuffled) c[k] = k;
  double t2 = now_seconds();
  CHECK(a == b && b == c && b.check_invariants());
  // Each key is found right after its insert (a reseed rebuilds the table
  // during the insert), and the invariants hold at every power-of-two size.
  bflat::unordered_flat_map<uint64_t, uint64_t, H> d;
  bool found = true, inv = true;
  for (auto& kv : a) {
    d[kv.first] = kv.second;
    found &= d.contains(kv.first);
    if ((d.size() & (d.size() - 1)) == 0) inv &= d.check_invariants();
  }
  CHECK(found && inv);
  std::printf("  iteration order insert: %.3f s, shuffled %.3f s\n", t1 - t0, t2 - t1);
  return (t1 - t0) / (t2 - t1);
}

size_t tiered_capacity(size_t n) {
  const size_t slots = (n / 7) * 8 + ((n % 7) * 8 + 6) / 7;
  return std::max<size_t>(2, slots / 15 + 1) * 15 - 1;
}

int main() {
  // Exact group rounding with a sentinel; no power-of-two rounding.
  for (size_t n : {1ul, 14ul, 15ul, 16ul, 1000ul, 123457ul, 1000000ul, 10000000ul}) {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    m.reserve(n);
    const size_t want = tiered_capacity(n);
    CHECK(m.bucket_count() == want);
    CHECK(m.max_load() >= n);
    // filling to n does not grow
    for (size_t i = 0; i < n; ++i) m.emplace(i, i);
    CHECK(m.bucket_count() == want);
    CHECK(!m.is_migrating());
  }

  // Exercise each growth tier (thresholds may be lowered by the runner).
  {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    size_t last = 0, changes = 0;
    bool ok = true;
    for (uint64_t i = 0; i < 6000000; ++i) {
      m.emplace(i * 0x9E3779B97F4A7C15ull, i);
      const size_t c = m.bucket_count();
      if (c != last) {
        if (last >= 30) {
          const size_t old_groups = (last + 1) / 15;
          const size_t bytes = old_groups * 256;
          const size_t divisor = bytes < BFLAT_GROW_2X_BELOW ? 1 :
                                 bytes < BFLAT_GROW_1_5X_BELOW ? 2 :
                                 bytes < BFLAT_GROW_1_25X_BELOW ? 4 : 8;
          const double f = static_cast<double>((c + 1) / 15);
          const double want = static_cast<double>(old_groups + (old_groups + divisor - 1) / divisor);
          if (std::fabs(f - want) > 0.01) {
            std::printf("  growth %zu -> %zu factor %.3f want %.2f\n", last, c, f, want);
            ok = false;
          }
        }
        last = c;
        ++changes;
      }
    }
    CHECK(ok);
    std::printf("  growth: %zu capacity changes up to %zu slots\n", changes, last);
  }

  // Move-only mapped type, operator[], at, equal_range, erase by iterator.
  {
    bflat::unordered_flat_map<int, std::unique_ptr<int>> m;
    for (int i = 0; i < 100000; ++i) m.emplace(i, std::make_unique<int>(i));
    m[7] = std::make_unique<int>(70);
    CHECK(*m.at(7) == 70);
    bool thrown = false;
    try { m.at(-1); } catch (const std::out_of_range&) { thrown = true; }
    CHECK(thrown);
    auto er = m.equal_range(5);
    CHECK(er.first != m.end() && std::next(er.first) == er.second);
    auto it = m.find(5);
    m.erase(it);
    CHECK(m.find(5) == m.end() && m.size() == 99999);
    auto m2 = std::move(m);
    CHECK(m2.size() == 99999 && m.empty());
  }

  // Heterogeneous lookup.
  {
    bflat::unordered_flat_map<std::string, int, SvHash, SvEq> m{{"a", 1}, {"bb", 2}, {"ccc", 3}};
    std::string_view k = "bb";
    CHECK(m.find(k) != m.end() && m.find(k)->second == 2);
    CHECK(m.contains(std::string_view("ccc")) && m.count(std::string_view("zz")) == 0);
    CHECK(m.erase(std::string_view("a")) == 1);
    m.try_emplace(std::string_view("dd"), 4);
    CHECK(m.at("dd") == 4 && m.size() == 3);
  }

  // Iterator and reference stability on erase (also during a migration),
  // erase while iterating.
  {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    uint64_t i = 0;
    while (!m.is_migrating() || m.size() < 300000) m.emplace(i, i), ++i;
    CHECK(m.is_migrating());
    std::vector<std::pair<const uint64_t, uint64_t>*> ptrs;
    for (uint64_t k = 1; k < i; k += 2) ptrs.push_back(&*m.find(k));
    for (uint64_t k = 0; k < i; k += 2) m.erase(k);  // erase never migrates
    bool ok = true;
    size_t j = 0;
    for (uint64_t k = 1; k < i; k += 2, ++j) ok &= ptrs[j]->first == k;
    CHECK(ok);
    CHECK(m.is_migrating());
    size_t n = 0;
    for (auto it = m.begin(); it != m.end();) {
      if (it->first % 3 == 0) it = m.erase(it);
      else ++it, ++n;
    }
    CHECK(n == m.size());
    m.finish_migration();
    CHECK(!m.is_migrating() && m.check_invariants());
  }

  // Custom allocator: no mmap, all memory returned.
  {
    {
      bflat::unordered_flat_map<uint64_t, std::string, std::hash<uint64_t>, std::equal_to<uint64_t>,
                                CountingAlloc<std::pair<const uint64_t, std::string>>> m;
      for (uint64_t k = 0; k < 500000; ++k) m.emplace(k, std::to_string(k));
      auto c = m;
      CHECK(c == m);
      c.clear();
      c.rehash(0);
      CHECK(c.bucket_count() == 0);
      for (uint64_t k = 0; k < 500000; k += 2) m.erase(k);
      CHECK(m.size() == 250000 && m.check_invariants());
    }
    CHECK(g_live_bytes == 0);
  }

  // rehash / reserve while migrating; shrink.
  {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    uint64_t i = 0;
    while (!m.is_migrating()) m.emplace(i, i), ++i;
    m.reserve(m.size() * 4);
    CHECK(!m.is_migrating() && m.size() == i && m.check_invariants());
    for (uint64_t k = 0; k < i; k += 2) m.erase(k);
    m.rehash(0);
    CHECK(m.bucket_count() == tiered_capacity(m.size()));
    CHECK(m.check_invariants());
  }

  // Inserting a table's iteration order into an empty table: the keys
  // arrive in the home order of the source and pile up at the start of the
  // smaller tables of the growth. Bounded against the same keys shuffled.
  {
    std::vector<uint64_t> keys;
    for (uint64_t k = 0; k < 1000000; ++k) keys.push_back(k * 0x9E3779B97F4A7C15ull);
    CHECK(iteration_order_ratio<std::hash<uint64_t>>(keys) < 4.0);
    // An avalanching hash (no mixing) with random keys: reseeds only with
    // BFLAT_RESEED_AVALANCHING (checked for content either way).
    std::mt19937_64 rng(7);
    for (auto& k : keys) k = rng();
    const double r = iteration_order_ratio<IdentityHash>(keys);
    CHECK(!BFLAT_RESEED_AVALANCHING || r < 4.0);
  }

  // Erasing other elements while iterating: an erased element is not
  // visited, every other one exactly once (also in the old table).
  for (int migrating = 0; migrating < 2; ++migrating) {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    uint64_t i = 0;
    if (migrating) while (!m.is_migrating() || m.size() < 200000) m.emplace(i, i), ++i;
    else for (; i < 5000; ++i) m.emplace(i, i);
    CHECK(m.is_migrating() == (migrating != 0));
    std::vector<char> state(i, 0);  // 1 visited, 2 erased
    bool ok = true;
    size_t n = 0;
    for (auto it = m.begin(); it != m.end(); ++it, ++n) {
      ok &= state[it->first] == 0;
      state[it->first] = 1;
      auto nx = it;
      if (n % 3 == 0 && ++nx != m.end()) {
        state[nx->first] = 2;
        m.erase(nx->first);  // usually the next lane of the same group
      }
    }
    for (char c : state) ok &= c != 0;
    CHECK(ok && n == m.size());
    m.finish_migration();
    CHECK(m.check_invariants());
  }

#if !defined(__SANITIZE_ADDRESS__)
  // Iteration cost per element of a map in the L2/L3 cache, against a scan
  // of the same number of pairs in a vector.
  {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    std::mt19937_64 rng(3);
    for (int k = 0; k < 30000; ++k) m[rng()] = k;
    std::vector<std::pair<uint64_t, uint64_t>> v(m.begin(), m.end());
    uint64_t sm = 0, sv = 0;
    double tm = 1e9, tv = 1e9;
    for (int r = 0; r < 20; ++r) {
      double t0 = now_seconds();
      for (int j = 0; j < 20; ++j)
        for (auto& kv : m) sm += kv.second;
      double t1 = now_seconds();
      for (int j = 0; j < 20; ++j)
        for (auto& kv : v) sv += kv.second;
      double t2 = now_seconds();
      tm = std::min(tm, t1 - t0);
      tv = std::min(tv, t2 - t1);
    }
    CHECK(sm == sv);
    std::printf("  iteration: %.2f ns per element, vector %.2f ns\n", tm / (20.0 * m.size()) * 1e9,
                tv / (20.0 * m.size()) * 1e9);
    CHECK(tm < 8 * tv);
  }
#endif

  std::printf("test_api: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
