// API and edge cases of bflat::unordered_flat_map.
#include <bflat/unordered_flat_map.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

int main() {
  // reserve: ceil(n / 0.875) slots rounded up to 15-slot groups.
  for (size_t n : {1ul, 14ul, 15ul, 16ul, 1000ul, 123457ul, 1000000ul, 10000000ul}) {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    m.reserve(n);
    const size_t slots = static_cast<size_t>(std::ceil(n / 0.875));
    const size_t want = (slots + 14) / 15 * 15;
    CHECK(m.bucket_count() == want);
    CHECK(m.max_load() >= n);
    // filling to n does not grow
    for (size_t i = 0; i < n; ++i) m.emplace(i, i);
    CHECK(m.bucket_count() == want);
    CHECK(!m.is_migrating());
  }

  // Growth sequence: tier factors by table bytes.
  {
    bflat::unordered_flat_map<uint64_t, uint64_t> m;
    size_t last = 0, changes = 0;
    bool ok = true;
    for (uint64_t i = 0; i < 6000000; ++i) {
      m.emplace(i * 0x9E3779B97F4A7C15ull, i);
      const size_t c = m.bucket_count();
      if (c != last) {
        if (last >= 30) {
          const double bytes = last / 15.0 * (15 * 16 + 16);
          const double f = static_cast<double>(c) / last;
          const double want = bytes < (double)BFLAT_GROW_2X_BELOW ? 2.0
                               : bytes < (double)BFLAT_GROW_1_5X_BELOW ? 1.5 : 1.25;
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
    CHECK(m.bucket_count() == (static_cast<size_t>(std::ceil(m.size() / 0.875)) + 14) / 15 * 15);
    CHECK(m.check_invariants());
  }

  // Inserting a table's iteration order into an empty table (no seed, as
  // boost): measure only.
  {
    bflat::unordered_flat_map<uint64_t, uint64_t> a;
    for (uint64_t k = 0; k < 1000000; ++k) a.emplace(k * 0x9E3779B97F4A7C15ull, k);
    bflat::unordered_flat_map<uint64_t, uint64_t> b;
    for (auto& kv : a) b[kv.first] = kv.second;
    CHECK(a == b);
  }

  std::printf("test_api: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
