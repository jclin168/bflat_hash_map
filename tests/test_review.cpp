// Review regression tests: allocator propagation, throwing hash / copy /
// allocation, size limits, probe coverage, range insert, iterator edge cases.
// Build variants with small BFLAT_GROW_*_BELOW / BFLAT_GROW_AT_ONCE_BELOW=0
// to exercise the growth tiers and incremental migration.
#include <bflat/unordered_flat_map.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
      if (++g_fail > 30) std::exit(1);                                        \
    }                                                                         \
  } while (0)

// ---------------------------------------------------------------- allocators
// Stateful allocator: each block remembers the arena that allocated it; a
// deallocation through another arena is counted as an error.
static std::map<void*, int> g_arena_of;
static int g_bad_dealloc = 0;
static long g_arena_live = 0;
static long g_alloc_countdown = -1;  // >= 0: throw bad_alloc when it reaches 0
template <typename T>
struct ArenaAlloc {
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  int id;
  explicit ArenaAlloc(int i = 0) : id(i) {}
  template <typename U>
  ArenaAlloc(const ArenaAlloc<U>& o) : id(o.id) {}
  T* allocate(size_t n) {
    if (g_alloc_countdown >= 0 && g_alloc_countdown-- == 0) throw std::bad_alloc();
    void* p = ::operator new(n * sizeof(T), std::align_val_t(64));
    g_arena_of[p] = id;
    g_arena_live += static_cast<long>(n * sizeof(T));
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t n) {
    auto it = g_arena_of.find(p);
    if (it == g_arena_of.end() || it->second != id) ++g_bad_dealloc;
    if (it != g_arena_of.end()) g_arena_of.erase(it);
    g_arena_live -= static_cast<long>(n * sizeof(T));
    ::operator delete(p, std::align_val_t(64));
  }
  template <typename U>
  bool operator==(const ArenaAlloc<U>& o) const { return id == o.id; }
  template <typename U>
  bool operator!=(const ArenaAlloc<U>& o) const { return id != o.id; }
};

// ------------------------------------------------------------- throwing types
struct HashError : std::runtime_error {
  HashError() : std::runtime_error("hash") {}
};
static uint64_t g_throw_key = ~0ull;
struct ThrowHash {
  size_t operator()(uint64_t k) const {
    if (k == g_throw_key) throw HashError();
    return std::hash<uint64_t>()(k);
  }
};

struct CopyError : std::runtime_error {
  CopyError() : std::runtime_error("copy") {}
};
static long g_copy_countdown = -1;  // >= 0: the copy that reaches 0 throws
static long g_live_values = 0;
// Copy may throw; move never does, so migrations are safe.
struct Value {
  uint64_t v;
  Value() : v(0) { ++g_live_values; }
  Value(uint64_t x) : v(x) { ++g_live_values; }
  Value(const Value& o) : v(o.v) {
    if (g_copy_countdown >= 0 && g_copy_countdown-- == 0) throw CopyError();
    ++g_live_values;
  }
  Value(Value&& o) noexcept : v(o.v) { ++g_live_values; }
  Value& operator=(const Value& o) = default;
  Value& operator=(Value&& o) noexcept = default;
  ~Value() { --g_live_values; }
  bool operator==(const Value& o) const { return v == o.v; }
};
// Move may throw (no noexcept): a migration with such a type only has the
// basic guarantee (no crash, no leak, size consistent).
struct ThrowMove {
  uint64_t v;
  ThrowMove(uint64_t x) : v(x) { ++g_live_values; }
  ThrowMove(const ThrowMove& o) : v(o.v) { ++g_live_values; }
  ThrowMove(ThrowMove&& o) : v(o.v) {
    if (g_copy_countdown >= 0 && g_copy_countdown-- == 0) throw CopyError();
    ++g_live_values;
  }
  ~ThrowMove() { --g_live_values; }
};

// ----------------------------------------------------------------- sections
static void test_allocator_propagation() {
  using A = ArenaAlloc<std::pair<const uint64_t, uint64_t>>;
  using Map = bflat::unordered_flat_map<uint64_t, uint64_t, std::hash<uint64_t>,
                                        std::equal_to<uint64_t>, A>;
  {
    Map a(A(1)), b(A(2));
    for (uint64_t i = 0; i < 20000; ++i) a.emplace(i, i), b.emplace(i * 7, i);
    b = a;  // propagates: b's old blocks must be freed by arena 2
    CHECK(b.get_allocator().id == 1);
    CHECK(b == a && b.check_invariants());
    Map c(A(3));
    c.emplace(1, 1);
    c = std::move(a);  // propagates: c's old block freed by arena 3
    CHECK(c.get_allocator().id == 1 && c.size() == 20000 && a.empty());
    Map d(A(4));
    d.emplace(5, 5);
    d.swap(c);
    CHECK(d.get_allocator().id == 1 && c.get_allocator().id == 4);
    CHECK(d.size() == 20000 && c.size() == 1);
    // Copy assignment onto a map migrating (incremental builds) and self.
    b = b;
    CHECK(b.size() == 20000);
  }
  CHECK(g_bad_dealloc == 0);
  CHECK(g_arena_live == 0 && g_arena_of.empty());
  std::printf("  allocator propagation: ok\n");
}

static void test_throwing_hash() {
  bflat::unordered_flat_map<uint64_t, uint64_t, ThrowHash> m;
  for (uint64_t i = 0; i < 1000; ++i) m.emplace(i, i);
  g_throw_key = 5;
  int caught = 0;
  try { (void)m.find(5); } catch (const HashError&) { ++caught; }
  try { (void)m.contains(5); } catch (const HashError&) { ++caught; }
  try { (void)m.count(5); } catch (const HashError&) { ++caught; }
  try { (void)m.at(5); } catch (const HashError&) { ++caught; }
  try { (void)m.erase(5); } catch (const HashError&) { ++caught; }
  try { (void)m.emplace(5, 1); } catch (const HashError&) { ++caught; }
  try { m[5] = 1; } catch (const HashError&) { ++caught; }
  try { (void)m.insert_or_assign(5, 1); } catch (const HashError&) { ++caught; }
  try { (void)m.try_emplace(5, 1); } catch (const HashError&) { ++caught; }
  CHECK(caught == 9);
  CHECK(m.size() == 1000);
  g_throw_key = ~0ull;
  CHECK(m.check_invariants() && m.at(5) == 5);
  // A hash that throws while a migration moves an element: the insert fails,
  // the map stays consistent.
  bflat::unordered_flat_map<uint64_t, uint64_t, ThrowHash> g;
  uint64_t i = 0;
  while (!g.is_migrating() && i < 5000000) g.emplace(i, i), ++i;
  if (g.is_migrating()) {
    // Keys in the old table: the last of the iteration and one in the
    // middle (hashing it throws while its group is being migrated).
    std::vector<uint64_t> old_keys;
    for (auto& kv : g) old_keys.push_back(kv.first);
    for (int round = 0; round < 2; ++round) {
      g_throw_key = round == 0 ? old_keys.back() : old_keys[old_keys.size() * 3 / 4];
      int thrown = 0;
      for (int t = 0; t < 100000 && g.is_migrating() && thrown < 50; ++t) {
        try { g.emplace(i, i); ++i; } catch (const HashError&) { ++thrown; }
      }
      g_throw_key = ~0ull;
      size_t cnt = 0;
      for (auto& kv : g) { (void)kv; ++cnt; }
      CHECK(cnt == g.size() && g.size() == i);
      CHECK(g.check_invariants());
      std::printf("  throwing hash during migration: thrown=%d migrating=%d\n", thrown,
                  int(g.is_migrating()));
    }
    g.finish_migration();
    CHECK(g.check_invariants() && g.size() == i);
    for (uint64_t k = 0; k < i; ++k) CHECK(g.at(k) == k);
  }
  g_throw_key = ~0ull;
  std::printf("  throwing hash: ok (migrating=%d)\n", int(i < 5000000));
}

static void test_size_limits() {
  using Map = bflat::unordered_flat_map<uint64_t, uint64_t>;
  Map m;
  bool thrown = false;
  try { m.reserve(m.max_size() + 1); } catch (const std::length_error&) { thrown = true; }
  CHECK(thrown);
  thrown = false;
  try { m.rehash(~size_t(0)); } catch (const std::length_error&) { thrown = true; }
  CHECK(thrown);
  // reserve(max_size()) is not a length error (it may fail to allocate).
  thrown = false;
  try { m.reserve(m.max_size()); } catch (const std::length_error&) { thrown = true; }
  catch (const std::bad_alloc&) {}
  CHECK(!thrown);
  CHECK(m.empty() && m.check_invariants());
  bflat::unordered_flat_map<uint64_t, std::string> ms;  // other slot size
  thrown = false;
  try { ms.reserve(ms.max_size()); } catch (const std::length_error&) { thrown = true; }
  catch (const std::bad_alloc&) {}
  CHECK(!thrown);
  // Tiny tables: 0, 1, a full smallest table (29 entries) and one more.
  Map t;
  t.rehash(0); t.reserve(0); t.rehash(1);
  CHECK(t.bucket_count() == 29 && t.max_load() == 29);
  for (uint64_t i = 0; i < 29; ++i) t.emplace(i, i);
  CHECK(t.bucket_count() == 29 && t.size() == 29 && t.load_factor() == 1.0f);
  for (uint64_t i = 0; i < 29; ++i) CHECK(t.find(i) != t.end());
  CHECK(t.find(29) == t.end() && t.find(1000) == t.end());
  t.emplace(29, 29);
  CHECK(t.bucket_count() > 29 && t.check_invariants());
  t.rehash(0);  // keeps the elements
  CHECK(t.size() == 30 && t.check_invariants());
  t.clear();
  t.rehash(0);
  CHECK(t.bucket_count() == 0);
  Map u(1);
  CHECK(u.bucket_count() == 29);
  std::printf("  size limits: ok\n");
}

static void test_range_insert_no_shrink() {
  bflat::unordered_flat_map<uint64_t, uint64_t> m;
  m.reserve(1000000);
  const size_t cap = m.bucket_count();
  for (uint64_t i = 0; i < 10; ++i) m.emplace(i, i);
  std::vector<std::pair<uint64_t, uint64_t>> v;
  for (uint64_t i = 100; i < 200; ++i) v.emplace_back(i, i);
  m.insert(v.begin(), v.end());
  CHECK(m.bucket_count() == cap);  // a range insert must not shrink the table
  CHECK(m.size() == 110 && m.check_invariants());
  std::printf("  range insert: ok\n");
}

static void test_probe_coverage() {
  // Every group is visited within 2 * n probes for any n and home.
  for (size_t n = 2; n <= 600; ++n) {
    std::vector<unsigned char> seen(n);
    for (size_t home = 0; home < n; ++home) {
      std::fill(seen.begin(), seen.end(), 0);
      bflat::detail::prober p(home);
      size_t steps = 0;
      do { CHECK(p.pos < n); seen[p.pos] = 1; ++steps; } while (p.next(n));
      CHECK(steps == 2 * n);
      for (size_t i = 0; i < n; ++i) CHECK(seen[i]);
    }
  }
  std::mt19937_64 rng(7);
  for (int t = 0; t < 40; ++t) {
    const size_t n = 601 + rng() % 200000;
    std::vector<unsigned char> seen(n);
    const size_t home = rng() % n;
    bflat::detail::prober p(home);
    size_t steps = 0;
    do { CHECK(p.pos < n); seen[p.pos] = 1; ++steps; } while (p.next(n));
    CHECK(steps == 2 * n);
    for (size_t i = 0; i < n; ++i) CHECK(seen[i]);
  }
  std::printf("  probe coverage: ok\n");
}

// Differential fuzz with throwing copies and bad_alloc; sizes around group
// and tier boundaries.
template <typename Alloc>
static void fuzz_exceptions(uint64_t seed, size_t ops, uint64_t keyspace, const Alloc& al,
                            bool with_alloc_failures) {
  using Map = bflat::unordered_flat_map<uint64_t, Value, std::hash<uint64_t>,
                                        std::equal_to<uint64_t>, Alloc>;
  std::mt19937_64 rng(seed);
  Map m(al);
  std::unordered_map<uint64_t, uint64_t> ref;
  size_t copy_throws = 0, alloc_throws = 0;
  for (size_t i = 0; i < ops; ++i) {
    const uint64_t r = rng() % 100;
    const uint64_t k = rng() % keyspace;
    const uint64_t v = rng();
    if (rng() % 5 == 0) g_copy_countdown = static_cast<long>(rng() % 3);
    else g_copy_countdown = -1;
    if (with_alloc_failures && rng() % 7 == 0) g_alloc_countdown = static_cast<long>(rng() % 2);
    else g_alloc_countdown = -1;
    try {
      if (r < 45) {
        const std::pair<const uint64_t, Value> src(k, Value(v));
        auto a = m.insert(src);  // copies the value
        auto b = ref.emplace(k, v);
        CHECK(a.second == b.second);
        CHECK(a.first->second.v == b.first->second);
      } else if (r < 55) {
        const Value val(v);
        auto a = m.try_emplace(k, val);
        auto b = ref.try_emplace(k, v);
        CHECK(a.second == b.second);
      } else if (r < 60) {
        auto a = m.emplace(k, Value(v));  // move, never throws
        auto b = ref.emplace(k, v);
        CHECK(a.second == b.second);
      } else if (r < 85) {
        CHECK(m.erase(k) == ref.erase(k));
      } else if (r < 97) {
        auto it = m.find(k);
        CHECK((it != m.end()) == (ref.count(k) == 1));
        if (it != m.end()) CHECK(it->second.v == ref[k]);
      } else if (r < 98) {
        g_copy_countdown = static_cast<long>(rng() % (ref.size() + 1));
        Map c(m);  // copy of (maybe migrating) table, may throw midway
        CHECK(c.size() == m.size() && c.check_invariants());
      } else if (r < 99) {
        m.rehash(rng() % (2 * ref.size() + 40));
      } else {
        m.reserve(rng() % (2 * ref.size() + 40));
      }
    } catch (const CopyError&) {
      ++copy_throws;
    } catch (const std::bad_alloc&) {
      ++alloc_throws;
    }
    g_copy_countdown = -1;
    g_alloc_countdown = -1;
    if (i % 997 == 0) {
      CHECK(m.size() == ref.size());
      CHECK(m.check_invariants());
    }
  }
  g_copy_countdown = -1;
  g_alloc_countdown = -1;
  CHECK(m.size() == ref.size() && m.check_invariants());
  for (auto& kv : ref) {
    auto it = m.find(kv.first);
    CHECK(it != m.end() && it->second.v == kv.second);
  }
  size_t n = 0;
  for (auto& kv : m) { ++n; CHECK(ref.count(kv.first) == 1); }
  CHECK(n == ref.size());
  std::printf("  exception fuzz seed=%llu keyspace=%llu: copy_throws=%zu alloc_throws=%zu size=%zu\n",
              (unsigned long long)seed, (unsigned long long)keyspace, copy_throws, alloc_throws,
              ref.size());
}

static void test_throwing_move_migration() {
  // A throwing move during migration: no crash, no leak, size unchanged.
  using Map = bflat::unordered_flat_map<uint64_t, ThrowMove>;
  const long live0 = g_live_values;
  {
    Map m;
    uint64_t i = 0;
    while (!m.is_migrating() && i < 3000000) m.emplace(i, ThrowMove(i)), ++i;
    size_t thrown = 0;
    while (m.is_migrating() && thrown < 2000) {
      g_copy_countdown = 3;
      try { m.emplace(i, ThrowMove(i)); ++i; } catch (const CopyError&) { ++thrown; }
      g_copy_countdown = -1;
      CHECK(m.size() + thrown == i);  // the element whose move threw is dropped
    }
    g_copy_countdown = -1;
    m.finish_migration();
    CHECK(m.size() + thrown == i && m.check_invariants());
    size_t n = 0;
    for (auto& kv : m) { (void)kv; ++n; }
    CHECK(n == m.size());
    CHECK(g_live_values - live0 == static_cast<long>(m.size()));
    std::printf("  throwing move during migration: thrown=%zu size=%zu\n", thrown, i);
  }
  CHECK(g_live_values == live0);
}

static void test_iterator_edges() {
  using Map = bflat::unordered_flat_map<uint64_t, uint64_t>;
  Map m;
  CHECK(m.begin() == m.end());
  CHECK(m.erase(m.begin(), m.end()) == m.end());
  m.rehash(100);
  CHECK(m.begin() == m.end());
  uint64_t i = 0;
  while (!m.is_migrating() && i < 3000000) m.emplace(i, i), ++i;
  // erase everything through iterators (new table then old table)
  const size_t sz = m.size();
  size_t erased = 0;
  for (auto it = m.cbegin(); it != m.cend();) {
    it = m.erase(it);
    ++erased;
  }
  CHECK(erased == sz && m.empty() && m.begin() == m.end());
  CHECK(m.check_invariants());
  // range erase ending at end()
  for (uint64_t k = 0; k < 100; ++k) m.emplace(k, k);
  auto it = m.begin();
  for (int t = 0; t < 50; ++t) ++it;
  CHECK(m.erase(it, m.cend()) == m.end());
  CHECK(m.size() == 50 && m.check_invariants());
  // const map iteration and const find
  const Map& cm = m;
  size_t n = 0;
  for (auto& kv : cm) n += kv.first < 1000;
  CHECK(n == 50);
  Map::const_iterator ci = cm.find(cm.begin()->first);
  CHECK(ci != cm.end());
  Map::const_iterator cj = m.find(cm.begin()->first);  // iterator -> const_iterator
  CHECK(cj == ci);
  std::printf("  iterator edges: ok\n");
}

static void test_move_only_and_strings() {
  bflat::unordered_flat_map<std::string, std::unique_ptr<int>> m;
  for (int i = 0; i < 50000; ++i) m.try_emplace(std::to_string(i), std::make_unique<int>(i));
  CHECK(*m.at("777") == 777);
  auto c = std::move(m);
  CHECK(m.empty() && c.size() == 50000);
  m = std::move(c);
  CHECK(c.empty() && m.size() == 50000);
  bflat::unordered_flat_map<std::string, std::unique_ptr<int>> d;
  d.emplace("x", std::make_unique<int>(1));
  d.swap(m);
  CHECK(d.size() == 50000 && m.size() == 1);
  // Key moved in; const key visible through iterators.
  std::string k = "moved";
  d.try_emplace(std::move(k), std::make_unique<int>(2));
  CHECK(d.find("moved") != d.end());
  static_assert(std::is_same<decltype(d.begin()->first), const std::string>::value, "const key");
  std::printf("  move-only values: ok\n");
}

int main(int argc, char** argv) {
  const uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  const double scale = argc > 2 ? std::atof(argv[2]) : 1.0;
  const std::string only = argc > 3 ? argv[3] : "";
  auto want = [&](const char* name) { return only.empty() || only == name; };
  auto S = [&](double x) { return static_cast<size_t>(x * scale); };
  std::printf("bflat test_review seed=%llu scale=%.2f grow_at_once_below=%llu migrate_groups=%d\n",
              (unsigned long long)seed, scale, (unsigned long long)BFLAT_GROW_AT_ONCE_BELOW,
              (int)BFLAT_MIGRATE_GROUPS);
  if (want("probe")) test_probe_coverage();
  if (want("limits")) test_size_limits();
  if (want("range")) test_range_insert_no_shrink();
  if (want("alloc")) test_allocator_propagation();
  if (want("hash")) test_throwing_hash();
  if (want("iter")) test_iterator_edges();
  if (want("moveonly")) test_move_only_and_strings();
  if (want("throwmove")) test_throwing_move_migration();
  if (!want("fuzz")) {
    std::printf("test_review: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
  }
  using VA = std::allocator<std::pair<const uint64_t, Value>>;
  using AA = ArenaAlloc<std::pair<const uint64_t, Value>>;
  // keyspaces near the smallest tables (29, 44, 59 slots), around the
  // 2 MiB mmap threshold / at-once threshold, and larger.
  const uint64_t spaces[] = {20, 29, 30, 45, 60, 300, 20000, 140000, 400000};
  for (uint64_t ks : spaces) {
    fuzz_exceptions(seed + ks, S(ks < 1000 ? 200000 : 600000), ks, VA(), false);
    fuzz_exceptions(seed + ks + 1, S(ks < 1000 ? 200000 : 600000), ks, AA(9), true);
  }
  CHECK(g_live_values == 0);
  CHECK(g_arena_live == 0 && g_bad_dealloc == 0);
  std::printf("test_review: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
