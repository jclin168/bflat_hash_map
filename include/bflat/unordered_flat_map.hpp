// bflat::unordered_flat_map -- prototype.
//
// The boost::unordered_flat_map architecture (Boost.Unordered "foa" core):
//   * groups of 15 slots + 1 overflow byte (16 bytes of metadata per group),
//   * reduced hash in each metadata byte (boost's mapping: 0 = available,
//     1 = sentinel in boost, so reduced hash values 0 and 1 are mapped to 8
//     and 9; 254 values),
//   * an 8-bit overflow byte per group (bit hash % 8) for an early miss,
//   * triangular ("quadratic") probing over groups,
//   * SSE2 / NEON group match,
// with the memory management of sflat_hash_map:
//   * any group count (not a power of two); home group by fastrange
//     (high 64 bits of the 128-bit product hash * groups),
//   * reserve(n) gives ceil(n / 0.875) slots, rounded up to 15-slot groups,
//   * growth: small tables (below BFLAT_GROW_AT_ONCE_BELOW bytes) grow at
//     once; larger tables grow with incremental migration (old and new arrays
//     live together; each insert moves a batch of old groups); tiered growth
//     factor 2x / 1.5x / 1.25x by table size,
//   * Linux + std::allocator: arrays of 2 MiB or more are mmap'ed, the old
//     element pages behind the migration front are released with
//     MADV_DONTNEED, and THP hints are given (MADV_HUGEPAGE near the front,
//     MADV_NOHUGEPAGE ahead of it, MADV_COLLAPSE at the end).
//
// See README.md for the design, the deviations from boost and sflat, and the
// changed guarantees (iterator invalidation during migration).
#ifndef BFLAT_UNORDERED_FLAT_MAP_HPP
#define BFLAT_UNORDERED_FLAT_MAP_HPP

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(__has_include)
#if __has_include(<boost/container_hash/hash_is_avalanching.hpp>)
#include <boost/container_hash/hash_is_avalanching.hpp>
#define BFLAT_HAS_BOOST_AVALANCHING 1
#endif
#endif

// ------------------------------------------------------------ configuration
#if !defined(BFLAT_USE_MMAP)
#if defined(__linux__)
#define BFLAT_USE_MMAP 1
#else
#define BFLAT_USE_MMAP 0
#endif
#endif
#if !defined(BFLAT_USE_THP)
#define BFLAT_USE_THP 1
#endif
// Growth tiers by the size of the current table in bytes (elements plus
// metadata), as in sflat.
#if !defined(BFLAT_GROW_2X_BELOW)
#define BFLAT_GROW_2X_BELOW (2ULL << 30)
#endif
#if !defined(BFLAT_GROW_1_5X_BELOW)
#define BFLAT_GROW_1_5X_BELOW (10ULL << 30)
#endif
// Tables smaller than this many bytes move all elements in the insert that
// starts growth. 0: always incremental.
#if !defined(BFLAT_GROW_AT_ONCE_BELOW)
#define BFLAT_GROW_AT_ONCE_BELOW (2ULL << 20)
#endif
// Old groups moved per insert during a migration (16 groups = 240 slots;
// sflat moves 256 slots).
#if !defined(BFLAT_MIGRATE_GROUPS)
#define BFLAT_MIGRATE_GROUPS 16
#endif
// NEON match: 1 = cmeq + shrn #4 (4 bits per lane, sflat style),
// 0 = boost's movemask emulation (vand + vzip + vaddv).
#if !defined(BFLAT_NEON_SHRN)
#define BFLAT_NEON_SHRN 1
#endif
// Insert fast path: 1 = one metadata load for the fingerprint match and the
// available mask of the home group; 0 = boost order (find, then a second
// probe for an available slot).
#if !defined(BFLAT_INSERT_ONE_LOAD)
#define BFLAT_INSERT_ONE_LOAD 1
#endif
// Element prefetch in find: 0 none, 1 boost (on a match, before the key
// compare), 2 sflat (home group elements before the metadata load).
#if !defined(BFLAT_PREFETCH)
#define BFLAT_PREFETCH 1
#endif
// Reduced hash: 0 = arithmetic (x | (x < 2) << 3), 1 = boost's 256-byte table.
#if !defined(BFLAT_RH_TABLE)
#define BFLAT_RH_TABLE 0
#endif

#if !defined(BFLAT_NO_SIMD)
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define BFLAT_SSE2 1
#include <emmintrin.h>
#elif (defined(__ARM_NEON) || defined(__ARM_NEON__)) && \
    (!defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define BFLAT_NEON 1
#include <arm_neon.h>
#endif
#endif

#if BFLAT_USE_MMAP
#include <sys/mman.h>
#define BFLAT_HAS_MMAP 1
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#else
#define BFLAT_HAS_MMAP 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define BFLAT_LIKELY(x) __builtin_expect(!!(x), 1)
#define BFLAT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BFLAT_NOINLINE __attribute__((noinline))
#define BFLAT_FORCEINLINE inline __attribute__((always_inline))
#define BFLAT_PREFETCH_ADDR(p) __builtin_prefetch((const void*)(p))
#else
#define BFLAT_LIKELY(x) (x)
#define BFLAT_UNLIKELY(x) (x)
#define BFLAT_NOINLINE
#define BFLAT_FORCEINLINE inline
#define BFLAT_PREFETCH_ADDR(p) ((void)0)
#endif

namespace bflat {

#if defined(BFLAT_STATS)
inline size_t bflat_stats_refresh = 0;
#endif

// ---------------------------------------------------------- hash traits
// A hash marked as avalanching is used as is; others are mixed with mulx
// (as boost). Uses boost::hash_is_avalanching when Boost is available, else
// a nested is_avalanching typedef.
template <typename Hash, typename = void>
struct hash_is_avalanching_nested : std::false_type {};
template <typename Hash>
struct hash_is_avalanching_nested<Hash, std::void_t<typename Hash::is_avalanching>>
    : std::integral_constant<bool, Hash::is_avalanching::value> {};
template <typename Hash>
struct hash_is_avalanching
#if defined(BFLAT_HAS_BOOST_AVALANCHING)
    : std::integral_constant<bool, boost::hash_is_avalanching<Hash>::value ||
                                       hash_is_avalanching_nested<Hash>::value> {
#else
    : hash_is_avalanching_nested<Hash> {
#endif
};

namespace detail {

// boost::unordered::detail::mulx (64-bit).
inline uint64_t mulx(uint64_t x) noexcept {
  constexpr uint64_t C = 0x9E3779B97F4A7C15ull;
#if defined(__SIZEOF_INT128__)
  const __uint128_t r = static_cast<__uint128_t>(x) * C;
  return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
#else
  const uint64_t a = x & 0xFFFFFFFFu, b = x >> 32, c = C & 0xFFFFFFFFu, d = C >> 32;
  const uint64_t ac = a * c, bc = b * c, ad = a * d, bd = b * d;
  const uint64_t cross = (ac >> 32) + (bc & 0xFFFFFFFFu) + ad;
  const uint64_t hi = bd + (bc >> 32) + (cross >> 32);
  const uint64_t lo = (cross << 32) | (ac & 0xFFFFFFFFu);
  return lo ^ hi;
#endif
}

// Home group: fastrange on the full 64-bit hash (high half of hash * n).
// Monotonic in the hash, so a key has its home at the same relative
// position in tables of any size (ordered migration relies on this).
inline size_t fastrange(uint64_t h, size_t n) noexcept {
#if defined(__SIZEOF_INT128__)
  return static_cast<size_t>((static_cast<__uint128_t>(h) * n) >> 64);
#else
  const uint64_t a = h & 0xFFFFFFFFu, b = h >> 32, c = n & 0xFFFFFFFFu, d = (uint64_t)n >> 32;
  const uint64_t ac = a * c, bc = b * c, ad = a * d, bd = b * d;
  const uint64_t cross = (ac >> 32) + (bc & 0xFFFFFFFFu) + ad;
  return static_cast<size_t>(bd + (bc >> 32) + (cross >> 32));
#endif
}

inline unsigned ctz64(uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctzll(x));
#else
  unsigned n = 0;
  while (!(x & 1)) { x >>= 1; ++n; }
  return n;
#endif
}

// boost's reduced hash: low byte, with 0 and 1 mapped to 8 and 9 (0 marks
// an available slot, 1 is boost's sentinel).
inline uint8_t reduced_hash(uint64_t h) noexcept {
#if BFLAT_RH_TABLE
  static constexpr unsigned char table[] = {
      8,9,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
      32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
      64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
      96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
      128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
      160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
      192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
      224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255};
  return table[static_cast<unsigned char>(h)];
#else
  const uint8_t x = static_cast<uint8_t>(h);
  return static_cast<uint8_t>(x | (static_cast<uint8_t>(x < 2) << 3));
#endif
}

// --------------------------------------------------------------- group15
// 16 bytes: metadata of slots 0..14 and the overflow byte at index 15.
// A Mask has one set bit per matching lane (bit lane << kShift).
struct alignas(16) group15 {
  static constexpr size_t N = 15;
  unsigned char m[16];

#if defined(BFLAT_SSE2)
  using Mask = uint32_t;
  static constexpr unsigned kShift = 0;
  static constexpr Mask kAll = 0x7FFFu;
  using Vec = __m128i;
  BFLAT_FORCEINLINE Vec load() const noexcept {
    return _mm_load_si128(reinterpret_cast<const __m128i*>(m));
  }
  static BFLAT_FORCEINLINE Mask match(Vec v, uint8_t rh) noexcept {
    return static_cast<Mask>(_mm_movemask_epi8(
               _mm_cmpeq_epi8(v, _mm_set1_epi8(static_cast<char>(rh))))) & kAll;
  }
  static BFLAT_FORCEINLINE Mask match_available(Vec v) noexcept {
    return static_cast<Mask>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_setzero_si128()))) & kAll;
  }
  static BFLAT_FORCEINLINE Mask match_occupied(Vec v) noexcept {
    return ~match_available(v) & kAll;
  }
#elif defined(BFLAT_NEON) && BFLAT_NEON_SHRN
  using Mask = uint64_t;
  static constexpr unsigned kShift = 2;
  static constexpr Mask kAll = 0x0111111111111111ull;
  using Vec = uint8x16_t;
  BFLAT_FORCEINLINE Vec load() const noexcept { return vld1q_u8(m); }
  static BFLAT_FORCEINLINE Mask narrow(uint8x16_t eq) noexcept {
    return vget_lane_u64(
        vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0);
  }
  static BFLAT_FORCEINLINE Mask match(Vec v, uint8_t rh) noexcept {
    return narrow(vceqq_u8(v, vdupq_n_u8(rh))) & kAll;
  }
  static BFLAT_FORCEINLINE Mask match_available(Vec v) noexcept {
    return narrow(vceqzq_u8(v)) & kAll;
  }
  static BFLAT_FORCEINLINE Mask match_occupied(Vec v) noexcept {
    return narrow(vtstq_u8(v, v)) & kAll;
  }
#elif defined(BFLAT_NEON)
  // boost's NEON code: simde movemask emulation.
  using Mask = uint32_t;
  static constexpr unsigned kShift = 0;
  static constexpr Mask kAll = 0x7FFFu;
  using Vec = uint8x16_t;
  BFLAT_FORCEINLINE Vec load() const noexcept { return vld1q_u8(m); }
  static BFLAT_FORCEINLINE Mask movemask(uint8x16_t a) noexcept {
    static constexpr uint8_t md[16] = {1, 2, 4, 8, 16, 32, 64, 128,
                                       1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t masked = vandq_u8(vld1q_u8(md), a);
    uint8x8x2_t tmp = vzip_u8(vget_low_u8(masked), vget_high_u8(masked));
    uint16x8_t x = vreinterpretq_u16_u8(vcombine_u8(tmp.val[0], tmp.val[1]));
#if defined(__aarch64__)
    return vaddvq_u16(x);
#else
    uint64x2_t t64 = vpaddlq_u32(vpaddlq_u16(x));
    return static_cast<Mask>(vgetq_lane_u64(t64, 0) + vgetq_lane_u64(t64, 1));
#endif
  }
  static BFLAT_FORCEINLINE Mask match(Vec v, uint8_t rh) noexcept {
    return movemask(vceqq_u8(v, vdupq_n_u8(rh))) & kAll;
  }
  static BFLAT_FORCEINLINE Mask match_available(Vec v) noexcept {
    return movemask(vceqq_u8(v, vdupq_n_u8(0))) & kAll;
  }
  static BFLAT_FORCEINLINE Mask match_occupied(Vec v) noexcept {
    return movemask(vcgtq_u8(v, vdupq_n_u8(0))) & kAll;
  }
#else
  // Portable: a 15-bit mask from a byte loop.
  using Mask = uint32_t;
  static constexpr unsigned kShift = 0;
  static constexpr Mask kAll = 0x7FFFu;
  struct Vec { unsigned char b[16]; };
  BFLAT_FORCEINLINE Vec load() const noexcept {
    Vec v;
    std::memcpy(v.b, m, 16);
    return v;
  }
  static BFLAT_FORCEINLINE Mask match(const Vec& v, uint8_t rh) noexcept {
    Mask r = 0;
    for (unsigned i = 0; i < N; ++i) r |= Mask(v.b[i] == rh) << i;
    return r;
  }
  static BFLAT_FORCEINLINE Mask match_available(const Vec& v) noexcept { return match(v, 0); }
  static BFLAT_FORCEINLINE Mask match_occupied(const Vec& v) noexcept {
    return ~match_available(v) & kAll;
  }
#endif

  static BFLAT_FORCEINLINE unsigned lane(Mask x) noexcept { return ctz64(x) >> kShift; }
  // Lanes >= l.
  static BFLAT_FORCEINLINE Mask from_lane(unsigned l) noexcept {
    return static_cast<Mask>(~Mask(0) << (l << kShift));
  }

  BFLAT_FORCEINLINE Mask match(uint8_t rh) const noexcept { return match(load(), rh); }
  BFLAT_FORCEINLINE Mask match_available() const noexcept { return match_available(load()); }
  BFLAT_FORCEINLINE Mask match_occupied() const noexcept { return match_occupied(load()); }

  BFLAT_FORCEINLINE bool is_not_overflowed(uint64_t h) const noexcept {
    return !(m[N] & (1u << (h % 8)));
  }
  BFLAT_FORCEINLINE void mark_overflow(uint64_t h) noexcept {
    m[N] = static_cast<unsigned char>(m[N] | (1u << (h % 8)));
  }
  // The element at lane pos may have caused overflow (boost's anti-drift).
  BFLAT_FORCEINLINE bool maybe_caused_overflow(unsigned pos) const noexcept {
    return (m[N] & (1u << (m[pos] % 8))) != 0;
  }
  BFLAT_FORCEINLINE void set(unsigned pos, uint8_t rh) noexcept { m[pos] = rh; }
  BFLAT_FORCEINLINE void reset(unsigned pos) noexcept { m[pos] = 0; }
  BFLAT_FORCEINLINE bool is_occupied(unsigned pos) const noexcept { return m[pos] != 0; }
  // Clear slots 0..14, keep the overflow byte.
  BFLAT_FORCEINLINE void clear_slots_keep_overflow() noexcept {
    const unsigned char o = m[N];
    std::memset(m, 0, 16);
    m[N] = o;
  }
};
static_assert(sizeof(group15) == 16, "group15 must be 16 bytes");

// Empty table: one group with no slots used and no overflow, so a find needs
// no capacity check.
struct alignas(64) EmptyGroups {
  group15 g[4];
};
inline const EmptyGroups kEmptyGroups{};

// Probe sequence over n groups (any n). Triangular steps, as boost's
// pow2_quadratic_prober, taken modulo n. Triangular numbers modulo n visit
// every residue only when n is a power of two, so after n - 1 triangular
// steps the sequence continues with n linear steps; then every group has
// been visited. In practice an insert stops after a few steps; the linear
// tail only bounds the worst case.
struct prober {
  size_t pos;
  size_t step = 0;
  explicit prober(size_t p) noexcept : pos(p) {}
  BFLAT_FORCEINLINE bool next(size_t n) noexcept {
    ++step;
    pos += step < n ? step : 1;
    if (pos >= n) pos -= n;
    return step < 2 * n;
  }
};

// ------------------------------------------------------------ page memory
inline constexpr size_t kCacheLine = 64;
inline constexpr size_t kHugePage = size_t(2) << 20;
inline constexpr size_t round_up(size_t x, size_t a) noexcept { return (x + a - 1) / a * a; }
inline constexpr bool is_mapped(size_t bytes) noexcept {
  return BFLAT_HAS_MMAP && bytes >= kHugePage;
}

// Arrays of 2 MiB or more: their own 2 MiB-aligned mmap (zero-filled).
// huge: MADV_HUGEPAGE for the whole array, else MADV_NOHUGEPAGE.
inline void* page_alloc(size_t bytes, size_t align, bool huge, bool& zeroed) {
  zeroed = false;
#if BFLAT_HAS_MMAP
  if (is_mapped(bytes)) {
    const size_t len = round_up(bytes, kHugePage);
    void* raw = ::mmap(nullptr, len + kHugePage, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::bad_alloc();
    const uintptr_t r = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t a = round_up(r, kHugePage);
    if (a != r) ::munmap(raw, a - r);
    const uintptr_t tail = (r + len + kHugePage) - (a + len);
    if (tail) ::munmap(reinterpret_cast<void*>(a + len), tail);
    void* p = reinterpret_cast<void*>(a);
    if (BFLAT_USE_THP) ::madvise(p, len, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    zeroed = true;
    return p;
  }
#endif
  (void)huge;
  return ::operator new(bytes, std::align_val_t(align));
}

inline void page_free(void* p, size_t bytes, size_t align) noexcept {
  if (!p) return;
#if BFLAT_HAS_MMAP
  if (is_mapped(bytes)) {
    ::munmap(p, round_up(bytes, kHugePage));
    return;
  }
#endif
  (void)bytes;
  ::operator delete(p, std::align_val_t(align));
}

// Give back the whole huge pages inside [from, to) of a mapped array.
inline void page_release(void* p, size_t total, size_t from, size_t to) noexcept {
#if BFLAT_HAS_MMAP
  if (!is_mapped(total)) return;
  from = round_up(from, kHugePage);
  to = to / kHugePage * kHugePage;
  if (to > from) ::madvise(static_cast<char*>(p) + from, to - from, MADV_DONTNEED);
#else
  (void)p, (void)total, (void)from, (void)to;
#endif
}

// Ask for huge pages in [from, to) (rounded out). collapse: MADV_COLLAPSE too.
inline void page_huge(void* p, size_t total, size_t from, size_t to, bool collapse) noexcept {
#if BFLAT_HAS_MMAP
  if (!BFLAT_USE_THP || !is_mapped(total)) return;
  const size_t len = round_up(total, kHugePage);
  from = from / kHugePage * kHugePage;
  to = round_up(to, kHugePage);
  if (to > len) to = len;
  if (to <= from) return;
  char* b = static_cast<char*>(p) + from;
  ::madvise(b, to - from, MADV_HUGEPAGE);
  if (collapse) ::madvise(b, to - from, MADV_COLLAPSE);
#else
  (void)p, (void)total, (void)from, (void)to, (void)collapse;
#endif
}

template <typename T, typename = void>
struct is_transparent : std::false_type {};
template <typename T>
struct is_transparent<T, std::void_t<typename T::is_transparent>> : std::true_type {};

}  // namespace detail

// ===================================================================== map
template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = std::allocator<std::pair<const Key, T>>>
class unordered_flat_map {
 public:
  using key_type = Key;
  using mapped_type = T;
  using value_type = std::pair<const Key, T>;
  using init_type = std::pair<Key, T>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using allocator_type = Allocator;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = typename std::allocator_traits<Allocator>::pointer;
  using const_pointer = typename std::allocator_traits<Allocator>::const_pointer;

 private:
  using group = detail::group15;
  using Mask = group::Mask;
  static constexpr size_t N = group::N;
  using slot_type = typename std::aligned_storage<sizeof(value_type), alignof(value_type)>::type;
  using alloc_traits = std::allocator_traits<Allocator>;
  using slot_alloc = typename alloc_traits::template rebind_alloc<slot_type>;
  using slot_traits = std::allocator_traits<slot_alloc>;
  using group_alloc = typename alloc_traits::template rebind_alloc<group>;
  using group_traits = std::allocator_traits<group_alloc>;
  static constexpr bool kOwnMemory =
      std::is_same<Allocator, std::allocator<value_type>>::value;
  static constexpr bool kMix = !hash_is_avalanching<Hash>::value;
  static constexpr float kMaxLoadFactor = 0.875f;
  static constexpr size_t kSlotAlign =
      alignof(slot_type) > detail::kCacheLine ? alignof(slot_type) : detail::kCacheLine;
  static constexpr size_t kMigrateGroups = BFLAT_MIGRATE_GROUPS;
  static constexpr size_t kMaxGroups =
      std::numeric_limits<size_t>::max() / (sizeof(slot_type) * N + sizeof(group)) / 2;

  template <typename K>
  using enable_transparent = typename std::enable_if<
      detail::is_transparent<Hash>::value && detail::is_transparent<KeyEqual>::value &&
          (sizeof(K) > 0),
      int>::type;

 public:
  // ---------------------------------------------------------- iterators
  // An iterator holds the map, its element and the unified slot index:
  // [0, groups * 15) for the current table, then the old table of an ongoing
  // migration. (boost's iterator is two pointers; bflat needs the map to
  // cross from the new table into the old one.)
  template <bool IsConst>
  class iterator_impl {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = typename unordered_flat_map::value_type;
    using difference_type = std::ptrdiff_t;
    using reference = typename std::conditional<IsConst, const value_type&, value_type&>::type;
    using pointer = typename std::conditional<IsConst, const value_type*, value_type*>::type;

    iterator_impl() noexcept = default;
    template <bool O, typename = typename std::enable_if<IsConst && !O>::type>
    iterator_impl(const iterator_impl<O>& o) noexcept : m_(o.m_), p_(o.p_), i_(o.i_) {}

    reference operator*() const noexcept { return *p_; }
    pointer operator->() const noexcept { return p_; }
    iterator_impl& operator++() noexcept {
      m_->advance(i_ + 1, i_, p_);
      return *this;
    }
    iterator_impl operator++(int) noexcept {
      iterator_impl t = *this;
      ++*this;
      return t;
    }
    friend bool operator==(const iterator_impl& a, const iterator_impl& b) noexcept {
      return a.p_ == b.p_;
    }
    friend bool operator!=(const iterator_impl& a, const iterator_impl& b) noexcept {
      return a.p_ != b.p_;
    }

   private:
    friend class unordered_flat_map;
    template <bool>
    friend class iterator_impl;
    iterator_impl(const unordered_flat_map* m, value_type* p, size_t i) noexcept
        : m_(m), p_(p), i_(i) {}
    const unordered_flat_map* m_ = nullptr;
    value_type* p_ = nullptr;
    size_t i_ = 0;
  };
  using iterator = iterator_impl<false>;
  using const_iterator = iterator_impl<true>;

  // erase(iterator) returns this; it finds the next element only when
  // converted (as boost's erase_return_type).
  struct erase_return_type {
    const unordered_flat_map* m;
    size_t i;
    operator iterator() const noexcept {
      iterator r(m, nullptr, 0);
      m->advance(i + 1, r.i_, r.p_);
      return r;
    }
    operator const_iterator() const noexcept { return iterator(*this); }
  };

  // ------------------------------------------------------ construction
  unordered_flat_map() noexcept(std::is_nothrow_default_constructible<Hash>::value &&
                                std::is_nothrow_default_constructible<KeyEqual>::value &&
                                std::is_nothrow_default_constructible<Allocator>::value) {}
  explicit unordered_flat_map(size_type n, const hasher& h = hasher(),
                              const key_equal& eq = key_equal(),
                              const allocator_type& a = allocator_type())
      : hash_(h), eq_(eq), al_(a) {
    if (n) rehash(n);
  }
  unordered_flat_map(size_type n, const allocator_type& a)
      : unordered_flat_map(n, hasher(), key_equal(), a) {}
  unordered_flat_map(size_type n, const hasher& h, const allocator_type& a)
      : unordered_flat_map(n, h, key_equal(), a) {}
  explicit unordered_flat_map(const allocator_type& a) : al_(a) {}
  template <typename InputIt>
  unordered_flat_map(InputIt first, InputIt last, size_type n = 0,
                     const hasher& h = hasher(), const key_equal& eq = key_equal(),
                     const allocator_type& a = allocator_type())
      : unordered_flat_map(n, h, eq, a) {
    insert(first, last);
  }
  template <typename InputIt>
  unordered_flat_map(InputIt first, InputIt last, size_type n, const allocator_type& a)
      : unordered_flat_map(first, last, n, hasher(), key_equal(), a) {}
  unordered_flat_map(std::initializer_list<value_type> il, size_type n = 0,
                     const hasher& h = hasher(), const key_equal& eq = key_equal(),
                     const allocator_type& a = allocator_type())
      : unordered_flat_map(il.begin(), il.end(), n, h, eq, a) {}
  unordered_flat_map(std::initializer_list<value_type> il, const allocator_type& a)
      : unordered_flat_map(il.begin(), il.end(), 0, hasher(), key_equal(), a) {}

  unordered_flat_map(const unordered_flat_map& o)
      : unordered_flat_map(o, alloc_traits::select_on_container_copy_construction(o.al_)) {}
  unordered_flat_map(const unordered_flat_map& o, const allocator_type& a)
      : hash_(o.hash_), eq_(o.eq_), al_(a) {
    copy_from(o);
  }
  unordered_flat_map(unordered_flat_map&& o) noexcept(
      std::is_nothrow_move_constructible<Hash>::value &&
      std::is_nothrow_move_constructible<KeyEqual>::value)
      : hash_(std::move(o.hash_)), eq_(std::move(o.eq_)), al_(std::move(o.al_)) {
    steal(o);
  }
  unordered_flat_map(unordered_flat_map&& o, const allocator_type& a)
      : hash_(std::move(o.hash_)), eq_(std::move(o.eq_)), al_(a) {
    if (a == o.al_) {
      steal(o);
    } else {
      reserve(o.size());
      for (auto& v : o) emplace(std::move(const_cast<key_type&>(v.first)), std::move(v.second));
      o.clear();
    }
  }
  ~unordered_flat_map() { destroy_all(true); }

  unordered_flat_map& operator=(const unordered_flat_map& o) {
    if (this != &o) {
      unordered_flat_map t(o, alloc_traits::propagate_on_container_copy_assignment::value
                                  ? o.al_ : al_);
      swap_storage(t);
      hash_ = o.hash_;
      eq_ = o.eq_;
      if (alloc_traits::propagate_on_container_copy_assignment::value) al_ = o.al_;
    }
    return *this;
  }
  unordered_flat_map& operator=(unordered_flat_map&& o) noexcept(
      (alloc_traits::propagate_on_container_move_assignment::value ||
       alloc_traits::is_always_equal::value) &&
      std::is_nothrow_move_assignable<Hash>::value &&
      std::is_nothrow_move_assignable<KeyEqual>::value) {
    if (this == &o) return *this;
    if (alloc_traits::propagate_on_container_move_assignment::value || al_ == o.al_) {
      destroy_all(true);
      reset_members();
      hash_ = std::move(o.hash_);
      eq_ = std::move(o.eq_);
      if (alloc_traits::propagate_on_container_move_assignment::value) al_ = std::move(o.al_);
      steal(o);
    } else {
      clear();
      hash_ = o.hash_;
      eq_ = o.eq_;
      reserve(o.size());
      for (auto& v : o) emplace(std::move(const_cast<key_type&>(v.first)), std::move(v.second));
      o.clear();
    }
    return *this;
  }
  unordered_flat_map& operator=(std::initializer_list<value_type> il) {
    clear();
    insert(il);
    return *this;
  }

  allocator_type get_allocator() const noexcept { return al_; }

  // ----------------------------------------------------------- iteration
  iterator begin() noexcept {
    iterator r(this, nullptr, 0);
    advance(0, r.i_, r.p_);
    return r;
  }
  const_iterator begin() const noexcept { return const_cast<unordered_flat_map*>(this)->begin(); }
  const_iterator cbegin() const noexcept { return begin(); }
  iterator end() noexcept { return iterator(this, nullptr, 0); }
  const_iterator end() const noexcept { return const_iterator(this, nullptr, 0); }
  const_iterator cend() const noexcept { return end(); }

  // ------------------------------------------------------------ capacity
  bool empty() const noexcept { return size_ == 0; }
  size_type size() const noexcept { return size_; }
  size_type max_size() const noexcept { return kMaxGroups * N * 7 / 8; }

  // --------------------------------------------------------- modifiers
  template <typename... Args>
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace(Args&&... args) {
    return emplace_dispatch(std::forward<Args>(args)...);
  }
  template <typename... Args>
  iterator emplace_hint(const_iterator, Args&&... args) {
    return emplace(std::forward<Args>(args)...).first;
  }
  // template<typename = void>: {k, v} prefers init_type (as boost).
  template <typename = void>
  BFLAT_FORCEINLINE std::pair<iterator, bool> insert(const value_type& v) {
    return insert_impl(v.first, [&](value_type* p) { construct(p, v); });
  }
  template <typename = void>
  BFLAT_FORCEINLINE std::pair<iterator, bool> insert(value_type&& v) {
    return insert_impl(v.first, [&](value_type* p) {
      construct(p, std::move(const_cast<key_type&>(v.first)), std::move(v.second));
    });
  }
  BFLAT_FORCEINLINE std::pair<iterator, bool> insert(const init_type& v) {
    return insert_impl(v.first, [&](value_type* p) { construct(p, v.first, v.second); });
  }
  BFLAT_FORCEINLINE std::pair<iterator, bool> insert(init_type&& v) {
    return insert_impl(v.first, [&](value_type* p) {
      construct(p, std::move(v.first), std::move(v.second));
    });
  }
  template <typename P, typename = typename std::enable_if<
                            std::is_constructible<value_type, P&&>::value &&
                            !std::is_same<typename std::decay<P>::type, value_type>::value &&
                            !std::is_same<typename std::decay<P>::type, init_type>::value>::type>
  std::pair<iterator, bool> insert(P&& v) {
    return emplace(std::forward<P>(v));
  }
  iterator insert(const_iterator, const value_type& v) { return insert(v).first; }
  iterator insert(const_iterator, value_type&& v) { return insert(std::move(v)).first; }
  iterator insert(const_iterator, init_type&& v) { return insert(std::move(v)).first; }
  template <typename InputIt>
  void insert(InputIt first, InputIt last) {
    using cat = typename std::iterator_traits<InputIt>::iterator_category;
    if constexpr (std::is_base_of<std::forward_iterator_tag, cat>::value) {
      const size_t n = static_cast<size_t>(std::distance(first, last));
      if (n > size_) reserve(n);  // as sflat: max(size(), n), not size() + n
    }
    for (; first != last; ++first) emplace(*first);
  }
  void insert(std::initializer_list<value_type> il) { insert(il.begin(), il.end()); }

  template <typename... Args>
  BFLAT_FORCEINLINE std::pair<iterator, bool> try_emplace(const key_type& k, Args&&... args) {
    return insert_impl(k, [&](value_type* p) {
      construct(p, std::piecewise_construct, std::forward_as_tuple(k),
                std::forward_as_tuple(std::forward<Args>(args)...));
    });
  }
  template <typename... Args>
  BFLAT_FORCEINLINE std::pair<iterator, bool> try_emplace(key_type&& k, Args&&... args) {
    return insert_impl(k, [&](value_type* p) {
      construct(p, std::piecewise_construct, std::forward_as_tuple(std::move(k)),
                std::forward_as_tuple(std::forward<Args>(args)...));
    });
  }
  template <typename K, typename... Args, enable_transparent<K> = 0,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<K>::type, key_type>::value>::type>
  std::pair<iterator, bool> try_emplace(K&& k, Args&&... args) {
    return insert_impl(k, [&](value_type* p) {
      construct(p, std::piecewise_construct, std::forward_as_tuple(std::forward<K>(k)),
                std::forward_as_tuple(std::forward<Args>(args)...));
    });
  }
  template <typename... Args>
  iterator try_emplace(const_iterator, const key_type& k, Args&&... args) {
    return try_emplace(k, std::forward<Args>(args)...).first;
  }
  template <typename... Args>
  iterator try_emplace(const_iterator, key_type&& k, Args&&... args) {
    return try_emplace(std::move(k), std::forward<Args>(args)...).first;
  }
  template <typename M>
  std::pair<iterator, bool> insert_or_assign(const key_type& k, M&& obj) {
    auto r = try_emplace(k, std::forward<M>(obj));
    if (!r.second) r.first->second = std::forward<M>(obj);
    return r;
  }
  template <typename M>
  std::pair<iterator, bool> insert_or_assign(key_type&& k, M&& obj) {
    auto r = try_emplace(std::move(k), std::forward<M>(obj));
    if (!r.second) r.first->second = std::forward<M>(obj);
    return r;
  }
  template <typename M>
  iterator insert_or_assign(const_iterator, const key_type& k, M&& obj) {
    return insert_or_assign(k, std::forward<M>(obj)).first;
  }
  template <typename M>
  iterator insert_or_assign(const_iterator, key_type&& k, M&& obj) {
    return insert_or_assign(std::move(k), std::forward<M>(obj)).first;
  }

  // erase never moves other elements (also during a migration).
  erase_return_type erase(iterator pos) noexcept { return erase(const_iterator(pos)); }
  erase_return_type erase(const_iterator pos) noexcept {
    erase_at(pos.i_);
    return {this, pos.i_};
  }
  iterator erase(const_iterator first, const_iterator last) noexcept {
    while (first != last) {
      const size_t i = first.i_;
      ++first;
      erase_at(i);
    }
    return iterator(this, last.p_, last.i_);
  }
  BFLAT_FORCEINLINE size_type erase(const key_type& k) {
    return erase_key(k);
  }
  template <typename K, enable_transparent<K> = 0,
            typename = typename std::enable_if<
                !std::is_convertible<K, iterator>::value &&
                !std::is_convertible<K, const_iterator>::value>::type>
  size_type erase(K&& k) {
    return erase_key(k);
  }

  void clear() noexcept {
    destroy_all(false);
    free_old();
    if (ng_) {
      std::memset(static_cast<void*>(groups_), 0, ng_ * sizeof(group));
      set_limits();
    }
    size_ = 0;
    reset_avail();
  }

  void swap(unordered_flat_map& o) noexcept(
      std::is_nothrow_swappable<Hash>::value && std::is_nothrow_swappable<KeyEqual>::value) {
    using std::swap;
    swap(hash_, o.hash_);
    swap(eq_, o.eq_);
    if (alloc_traits::propagate_on_container_swap::value) swap(al_, o.al_);
    swap_storage(o);
  }

  // ------------------------------------------------------------- lookup
  BFLAT_FORCEINLINE iterator find(const key_type& k) noexcept { return find_iter(k); }
  BFLAT_FORCEINLINE const_iterator find(const key_type& k) const noexcept {
    return const_cast<unordered_flat_map*>(this)->find_iter(k);
  }
  template <typename K, enable_transparent<K> = 0>
  iterator find(const K& k) { return find_iter(k); }
  template <typename K, enable_transparent<K> = 0>
  const_iterator find(const K& k) const { return const_cast<unordered_flat_map*>(this)->find_iter(k); }

  BFLAT_FORCEINLINE bool contains(const key_type& k) const noexcept { return find_ptr(k) != nullptr; }
  template <typename K, enable_transparent<K> = 0>
  bool contains(const K& k) const { return find_ptr(k) != nullptr; }
  size_type count(const key_type& k) const noexcept { return contains(k) ? 1 : 0; }
  template <typename K, enable_transparent<K> = 0>
  size_type count(const K& k) const { return contains(k) ? 1 : 0; }
  std::pair<iterator, iterator> equal_range(const key_type& k) {
    iterator it = find(k);
    if (it == end()) return {it, it};
    iterator nx = it;
    return {it, ++nx};
  }
  std::pair<const_iterator, const_iterator> equal_range(const key_type& k) const {
    const_iterator it = find(k);
    if (it == end()) return {it, it};
    const_iterator nx = it;
    return {it, ++nx};
  }

  mapped_type& at(const key_type& k) {
    value_type* p = find_ptr(k);
    if (!p) throw std::out_of_range("bflat::unordered_flat_map::at");
    return p->second;
  }
  const mapped_type& at(const key_type& k) const {
    const value_type* p = find_ptr(k);
    if (!p) throw std::out_of_range("bflat::unordered_flat_map::at");
    return p->second;
  }
  BFLAT_FORCEINLINE mapped_type& operator[](const key_type& k) { return try_emplace(k).first->second; }
  BFLAT_FORCEINLINE mapped_type& operator[](key_type&& k) {
    return try_emplace(std::move(k)).first->second;
  }

  // ---------------------------------------------------------- hash policy
  size_type bucket_count() const noexcept { return ng_ * N; }
  float load_factor() const noexcept {
    return ng_ ? static_cast<float>(size_) / static_cast<float>(ng_ * N) : 0.0f;
  }
  float max_load_factor() const noexcept { return kMaxLoadFactor; }
  void max_load_factor(float) noexcept {}  // no effect, as in boost
  size_type max_load() const noexcept { return ml_ < lim_ ? ml_ : lim_; }

  // boost semantics: at least n slots and at least size() / mlf; may shrink.
  // A migration is finished first; the rehash itself is done at once (in
  // index order, releasing old pages behind the cursor).
  void rehash(size_type n) {
    finish_migration();
    const size_t need = slots_for(size_);
    if (need > n) n = need;
    const size_t g = n ? groups_for_slots(n) : 0;
    if (g == ng_) return;
    if (g == 0) {
      destroy_all(true);
      reset_members();
      return;
    }
    relocate_to(g, true, true);
  }
  void reserve(size_type n) { rehash(slots_for(n)); }

  hasher hash_function() const { return hash_; }
  key_equal key_eq() const { return eq_; }

  // ------------------------------------------------- bflat extensions
  bool is_migrating() const noexcept { return ong_ != 0; }
  void finish_migration() {
    while (ong_) migrate_step(ong_);
  }

  // Test hook: checks the internal invariants; returns false on a violation.
  bool check_invariants() const {
    size_t cnt = 0;
    for (size_t g = 0; g < ng_; ++g)
      for (unsigned l = 0; l < N; ++l)
        if (groups_[g].is_occupied(l)) {
          ++cnt;
          const value_type* e = elem(elems_, g, l);
          if (detail::reduced_hash(hash_of(e->first)) != groups_[g].m[l]) return false;
          if (find_ptr(e->first) != e) return false;
        }
    for (size_t g = 0; g < ong_; ++g)
      for (unsigned l = 0; l < N; ++l)
        if (ogroups_[g].is_occupied(l)) {
          if (g < front_) return false;  // migrated groups must be empty
          ++cnt;
          const value_type* e = elem(oelems_, g, l);
          if (find_ptr(e->first) != e) return false;
        }
    if (cnt != size_) return false;
    if ((size_ > ml_ || size_ > lim_) && ng_ > 0) return false;
    if (!ong_ && avail_ != std::min(ml_, lim_) - size_) return false;
    if (ong_ && avail_ != 0) return false;
    return true;
  }

  friend bool operator==(const unordered_flat_map& a, const unordered_flat_map& b) {
    if (a.size() != b.size()) return false;
    for (const auto& v : a) {
      const value_type* p = b.find_ptr(v.first);
      if (!p || !(p->second == v.second)) return false;
    }
    return true;
  }
  friend bool operator!=(const unordered_flat_map& a, const unordered_flat_map& b) {
    return !(a == b);
  }
  friend void swap(unordered_flat_map& a, unordered_flat_map& b) noexcept(noexcept(a.swap(b))) {
    a.swap(b);
  }

 private:
  // ------------------------------------------------------------ helpers
  template <typename K>
  BFLAT_FORCEINLINE uint64_t hash_of(const K& k) const {
    const uint64_t h = static_cast<uint64_t>(hash_(k));
    if constexpr (kMix) return detail::mulx(h);
    else return h;
  }
  static BFLAT_FORCEINLINE value_type* elem(slot_type* base, size_t g, unsigned l) noexcept {
    return reinterpret_cast<value_type*>(base + g * N + l);
  }
  static BFLAT_FORCEINLINE const value_type* elem(const slot_type* base, size_t g, unsigned l) noexcept {
    return reinterpret_cast<const value_type*>(base + g * N + l);
  }
  template <typename... Args>
  void construct(value_type* p, Args&&... args) {
    alloc_traits::construct(al_, p, std::forward<Args>(args)...);
  }
  void destroy(value_type* p) noexcept { alloc_traits::destroy(al_, p); }

  // Slots for n elements at the max load: ceil(n / 0.875).
  static size_t slots_for(size_t n) noexcept {
    if (n == 0) return 0;
    const double need = static_cast<double>(n) / static_cast<double>(kMaxLoadFactor);
    size_t c = static_cast<size_t>(need);
    if (static_cast<double>(c) < need) ++c;
    return c;
  }
  static size_t groups_for_slots(size_t slots) {
    const size_t g = (slots + N - 1) / N;
    if (g > kMaxGroups) throw std::length_error("bflat::unordered_flat_map: too many elements");
    return g ? g : 1;
  }
  // boost: 100% usage for capacities up to 2N - 1 (here up to 2 groups).
  static size_t initial_ml(size_t ng) noexcept {
    const size_t cap = ng * N;
    if (ng <= 2) return cap;
    return static_cast<size_t>(static_cast<double>(kMaxLoadFactor) * static_cast<double>(cap));
  }
  // lim_: the load limit (growth when the elements reach it). ml_: boost's
  // anti-drift max load, but starting at lim_ plus a budget (sflat's used
  // limit: 3/8 of the slots above the load limit); each erase from a group
  // whose overflow bit for that element is set lowers it by one. When the
  // elements reach ml_ below lim_, the overflow bytes are recomputed in
  // place (no element moves) and ml_ is reset.
  void set_limits() noexcept {
    lim_ = initial_ml(ng_);
    ml_ = lim_ + (ng_ * N - lim_) * 3 / 8;
  }
  void reset_avail() noexcept {
    const size_t m = ml_ < lim_ ? ml_ : lim_;
    avail_ = (!ong_ && m > size_) ? m - size_ : 0;
  }
  static size_t table_bytes(size_t ng) noexcept { return ng * (N * sizeof(slot_type) + sizeof(group)); }

  // ------------------------------------------------------------- memory
  group* alloc_groups(size_t ng) {
    const size_t bytes = ng * sizeof(group);
    group* p;
    bool zeroed = false;
    if constexpr (kOwnMemory) {
      p = static_cast<group*>(detail::page_alloc(bytes, detail::kCacheLine, true, zeroed));
    } else {
      group_alloc a(al_);
      p = group_traits::allocate(a, ng);
    }
    if (!zeroed) std::memset(static_cast<void*>(p), 0, bytes);
    return p;
  }
  void free_groups(group* p, size_t ng) noexcept {
    if (!p || ng == 0) return;
    if constexpr (kOwnMemory) {
      detail::page_free(p, ng * sizeof(group), detail::kCacheLine);
    } else {
      group_alloc a(al_);
      group_traits::deallocate(a, p, ng);
    }
  }
  slot_type* alloc_slots(size_t ng, bool huge) {
    if constexpr (kOwnMemory) {
      bool z;
      return static_cast<slot_type*>(
          detail::page_alloc(ng * N * sizeof(slot_type), kSlotAlign, huge, z));
    } else {
      slot_alloc a(al_);
      return slot_traits::allocate(a, ng * N);
    }
  }
  void free_slots(slot_type* p, size_t ng) noexcept {
    if (!p || ng == 0) return;
    if constexpr (kOwnMemory) {
      detail::page_free(p, ng * N * sizeof(slot_type), kSlotAlign);
    } else {
      slot_alloc a(al_);
      slot_traits::deallocate(a, p, ng * N);
    }
  }

  // ------------------------------------------------------- find (core)
  // Probe the current table. Returns the element or nullptr.
  template <typename K>
  BFLAT_FORCEINLINE value_type* find_in(const group* groups, slot_type* elems, size_t ng,
                                        const K& k, uint64_t h, size_t* out_g = nullptr,
                                        unsigned* out_l = nullptr) const {
    const uint8_t rh = detail::reduced_hash(h);
    detail::prober pb(detail::fastrange(h, ng));
#if BFLAT_PREFETCH == 2
    BFLAT_PREFETCH_ADDR(elems + pb.pos * N);
#endif
    do {
      const group* pg = groups + pb.pos;
      Mask m = pg->match(rh);
      if (m) {
        slot_type* base = elems + pb.pos * N;
#if BFLAT_PREFETCH == 1
        BFLAT_PREFETCH_ADDR(base);
#endif
        do {
          const unsigned l = group::lane(m);
          value_type* e = reinterpret_cast<value_type*>(base + l);
          if (BFLAT_LIKELY(eq_(k, e->first))) {
            if (out_g) { *out_g = pb.pos; *out_l = l; }
            return e;
          }
          m &= m - 1;
        } while (m);
      }
      if (BFLAT_LIKELY(pg->is_not_overflowed(h))) return nullptr;
    } while (pb.next(ng));
    return nullptr;
  }

  // Element with key k in the old table of a migration, or nullptr.
  // Migrated groups (before front_) have no occupied slots but keep their
  // overflow byte, so the probe passes through them exactly as before the
  // migration and stops where it would have stopped.
  template <typename K>
  BFLAT_NOINLINE value_type* find_old(const K& k, uint64_t h, size_t* out_g = nullptr,
                                      unsigned* out_l = nullptr) const {
    return find_in(ogroups_, oelems_, ong_, k, h, out_g, out_l);
  }

  template <typename K>
  BFLAT_FORCEINLINE value_type* find_ptr(const K& k) const {
    const uint64_t h = hash_of(k);
    value_type* p = find_in(groups_, elems_, ng_, k, h);
    if (BFLAT_UNLIKELY(!p) && BFLAT_UNLIKELY(ong_ != 0)) p = find_old(k, h);
    return p;
  }
  template <typename K>
  BFLAT_FORCEINLINE iterator find_iter(const K& k) {
    const uint64_t h = hash_of(k);
    size_t g;
    unsigned l;
    value_type* p = find_in(groups_, elems_, ng_, k, h, &g, &l);
    if (BFLAT_LIKELY(p)) return iterator(this, p, g * N + l);
    if (BFLAT_UNLIKELY(ong_ != 0)) {
      p = find_old(k, h, &g, &l);
      if (p) return iterator(this, p, ng_ * N + g * N + l);
    }
    return end();
  }

  // ---------------------------------------------------------- iteration
  // First occupied unified index >= i; sets p (nullptr at the end).
  void advance(size_t i, size_t& out_i, value_type*& out_p) const noexcept {
    const size_t ncap = ng_ * N;
    if (i < ncap) {
      size_t g = i / N;
      unsigned l = static_cast<unsigned>(i - g * N);
      Mask m = groups_[g].match_occupied() & group::from_lane(l);
      for (;;) {
        if (m) {
          l = group::lane(m);
          out_i = g * N + l;
          out_p = elem(elems_, g, l);
          return;
        }
        if (++g >= ng_) break;
        m = groups_[g].match_occupied();
      }
      i = ncap;
    }
    if (ong_) {
      size_t j = i - ncap;
      size_t g = j / N;
      unsigned l = static_cast<unsigned>(j - g * N);
      if (g < front_) { g = front_; l = 0; }
      if (g < ong_) {
        Mask m = ogroups_[g].match_occupied() & group::from_lane(l);
        for (;;) {
          if (m) {
            l = group::lane(m);
            out_i = ncap + g * N + l;
            out_p = elem(oelems_, g, l);
            return;
          }
          if (++g >= ong_) break;
          m = ogroups_[g].match_occupied();
        }
      }
    }
    out_i = 0;
    out_p = nullptr;
  }

  // ------------------------------------------------------------- insert
  template <typename... Args>
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(Args&&... args) {
    init_type tmp(std::forward<Args>(args)...);
    return insert(std::move(tmp));
  }
  template <typename K, typename V>
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(K&& k, V&& v) {
    if constexpr (std::is_same<typename std::decay<K>::type, key_type>::value) {
      return insert_impl(k, [&](value_type* p) {
        construct(p, std::forward<K>(k), std::forward<V>(v));
      });
    } else {
      init_type tmp(std::forward<K>(k), std::forward<V>(v));
      return insert(std::move(tmp));
    }
  }
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(const value_type& v) { return insert(v); }
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(value_type&& v) { return insert(std::move(v)); }
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(value_type& v) { return insert(static_cast<const value_type&>(v)); }
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(const init_type& v) { return insert(v); }
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(init_type&& v) { return insert(std::move(v)); }
  BFLAT_FORCEINLINE std::pair<iterator, bool> emplace_dispatch(init_type& v) { return insert(static_cast<const init_type&>(v)); }

  template <typename K, typename F>
  BFLAT_FORCEINLINE std::pair<iterator, bool> insert_impl(const K& k, F&& make) {
    const uint64_t h = hash_of(k);
#if BFLAT_INSERT_ONE_LOAD
    // Fast path: one metadata load of the home group gives both the
    // fingerprint matches and the available slots.
    const size_t pos = detail::fastrange(h, ng_);
    group* pg = groups_ + pos;
    const auto v = pg->load();
    Mask m = group::match(v, detail::reduced_hash(h));
    if (m) {
      slot_type* base = elems_ + pos * N;
      do {
        const unsigned l = group::lane(m);
        value_type* e = reinterpret_cast<value_type*>(base + l);
        if (eq_(k, e->first)) return {iterator(this, e, pos * N + l), false};
        m &= m - 1;
      } while (m);
    }
    if (BFLAT_LIKELY(avail_ != 0) && BFLAT_LIKELY(pg->is_not_overflowed(h))) {
      // Absent (not in the home group and no key with this hash spilled).
      const Mask a = group::match_available(v);
      if (BFLAT_LIKELY(a != 0)) {
        const unsigned l = group::lane(a);
        value_type* e = elem(elems_, pos, l);
        make(e);
        pg->set(l, detail::reduced_hash(h));
        ++size_;
        --avail_;
        return {iterator(this, e, pos * N + l), true};
      }
      return insert_absent_new(h, make);
    }
    return insert_slow(k, h, make);
#else
    // boost order: find, then probe again for an available slot.
    size_t g;
    unsigned l;
    value_type* e = find_in(groups_, elems_, ng_, k, h, &g, &l);
    if (e) return {iterator(this, e, g * N + l), false};
    if (BFLAT_LIKELY(avail_ != 0)) return insert_absent_new(h, make);
    return insert_slow(k, h, make);
#endif
  }

  // The key is known absent and the table has room: place it in the current
  // table (first available slot in probe order, marking overflow).
  template <typename F>
  std::pair<iterator, bool> insert_absent_new(uint64_t h, F& make) {
    size_t g;
    unsigned l;
    value_type* e = place_new(h, g, l);
    make(e);
    groups_[g].set(l, detail::reduced_hash(h));
    ++size_;
    if (avail_) --avail_;
    return {iterator(this, e, g * N + l), true};
  }

  // First available slot of h in the current table; marks overflow on the
  // full groups it passes. The table always has an available slot here.
  value_type* place_new(uint64_t h, size_t& g, unsigned& l) noexcept {
    detail::prober pb(detail::fastrange(h, ng_));
    for (;;) {
      group* pg = groups_ + pb.pos;
      const Mask a = pg->match_available();
      if (BFLAT_LIKELY(a != 0)) {
        g = pb.pos;
        l = group::lane(a);
        return elem(elems_, g, l);
      }
      pg->mark_overflow(h);
      if (!pb.next(ng_)) {
        assert(false && "bflat: no available slot");
        pb = detail::prober(0);
      }
    }
  }

  // During a migration a new key whose old home group is at or after the
  // front goes into the old table (as in sflat), so the new table gets writes
  // only near the front, in order, and old pages behind the front can be
  // released. The probe follows the old table's sequence; if it reaches a
  // migrated group (it wrapped around), the key goes to the new table.
  // Returns false if the key must go to the new table.
  bool place_old(uint64_t h, size_t& g, unsigned& l) noexcept {
    detail::prober pb(detail::fastrange(h, ong_));
    if (pb.pos < front_) return false;
    for (;;) {
      if (pb.pos < front_) return false;
      group* pg = ogroups_ + pb.pos;
      const Mask a = pg->match_available();
      if (a) {
        g = pb.pos;
        l = group::lane(a);
        return true;
      }
      pg->mark_overflow(h);
      if (!pb.next(ong_)) return false;
    }
  }

  template <typename K, typename F>
  BFLAT_NOINLINE std::pair<iterator, bool> insert_slow(const K& k, uint64_t h, F& make) {
    if (ong_) {
      migrate_step(kMigrateGroups);
    } else if (avail_ == 0) {
      // Growth, first allocation or drift refresh. As boost, only for an
      // absent key.
      size_t g;
      unsigned l;
      if (value_type* e = find_in(groups_, elems_, ng_, k, h, &g, &l))
        return {iterator(this, e, g * N + l), false};
      make_room();
    }
    // Locate: current table, then the old table.
    {
      size_t g;
      unsigned l;
      if (value_type* e = find_in(groups_, elems_, ng_, k, h, &g, &l))
        return {iterator(this, e, g * N + l), false};
      if (ong_) {
        if (value_type* e = find_old(k, h, &g, &l))
          return {iterator(this, e, ng_ * N + g * N + l), false};
      }
    }
    if (BFLAT_UNLIKELY(size_ >= ml_ || size_ >= lim_)) {
      // The current table is at its limit before the migration ended (rare:
      // many erases from overflowed groups during the migration): finish,
      // then grow or refresh.
      finish_migration();
      make_room();
    }
    if (ong_) {
      size_t g;
      unsigned l;
      if (place_old(h, g, l)) {
        value_type* e = elem(oelems_, g, l);
        make(e);
        ogroups_[g].set(l, detail::reduced_hash(h));
        ++size_;
        return {iterator(this, e, ng_ * N + g * N + l), true};
      }
    }
    size_t g;
    unsigned l;
    value_type* e = place_new(h, g, l);
    make(e);
    groups_[g].set(l, detail::reduced_hash(h));
    ++size_;
    reset_avail();
    return {iterator(this, e, g * N + l), true};
  }

  // ------------------------------------------------------------- growth
  // Growth by tiers of the current table size (sflat): 2x below 2 GiB, 1.5x
  // below 10 GiB, else 1.25x. need: groups for size + size/61 + 1 elements
  // (boost's anti-drift slack). If the trigger came from boost's anti-drift
  // (erases from overflowed groups lowered the max load) and the current
  // capacity still fits, the table is rebuilt at the same size.
  size_t grown_groups() const {
    const size_t need = groups_for_slots(slots_for(size_ + 1));
    if (ng_ == 0) return need;
    if (need <= ng_) return ng_;
    const uint64_t bytes = table_bytes(ng_);
    const uint64_t c = ng_;
    uint64_t grown = bytes < BFLAT_GROW_2X_BELOW     ? c * 2
                     : bytes < BFLAT_GROW_1_5X_BELOW ? c + c / 2
                                                     : c + c / 4;
    if (grown < need) grown = need;
    if (grown > kMaxGroups) {
      if (need > kMaxGroups) throw std::length_error("bflat::unordered_flat_map: too many elements");
      grown = kMaxGroups;
    }
    return static_cast<size_t>(grown);
  }

  // Not migrating, and no room: grow when the elements reach the load
  // limit, else (anti-drift budget used up) recompute the overflow bytes.
  void make_room() {
    if (ng_ != 0 && size_ < lim_) {
      refresh_overflow();
      return;
    }
    grow();
  }

  // Recompute every overflow byte in place: clear them, then for each
  // element outside its home group replay its probe sequence from the home
  // group up to its group and mark the groups passed. The elements do not
  // move; the stale bits that erased elements left behind are gone.
  BFLAT_NOINLINE void refresh_overflow() noexcept {
#if defined(BFLAT_STATS)
    ++bflat_stats_refresh;
#endif
    for (size_t g = 0; g < ng_; ++g) groups_[g].m[N] = 0;
    for (size_t g = 0; g < ng_; ++g) {
      Mask m = groups_[g].match_occupied();
      while (m) {
        const unsigned l = group::lane(m);
        const uint64_t h = hash_of(elem(elems_, g, l)->first);
        detail::prober pb(detail::fastrange(h, ng_));
        while (pb.pos != g) {
          groups_[pb.pos].mark_overflow(h);
          pb.next(ng_);
        }
        m &= m - 1;
      }
    }
    set_limits();
    reset_avail();
  }

  void grow() {
    const size_t g = grown_groups();
    const bool at_once = table_bytes(ng_) < BFLAT_GROW_AT_ONCE_BELOW;
    relocate_to(g, at_once, at_once);
  }

  // Start moving all elements to a new table of g groups. at_once: finish in
  // this call. huge: MADV_HUGEPAGE on the whole new element array (else only
  // near the migration front).
  void relocate_to(size_t g, bool at_once, bool huge) {
    assert(!ong_);
    group* ngr = alloc_groups(g);
    slot_type* nel;
    try {
      nel = alloc_slots(g, huge);
    } catch (...) {
      free_groups(ngr, g);
      throw;
    }
    if (ng_ == 0) {
      groups_ = ngr;
      elems_ = nel;
      ng_ = g;
      set_limits();
      reset_avail();
      return;
    }
    ogroups_ = groups_;
    oelems_ = elems_;
    ong_ = ng_;
    front_ = 0;
    old_released_ = 0;
    huge_hint_ = 0;
    groups_ = ngr;
    elems_ = nel;
    ng_ = g;
    set_limits();
    avail_ = 0;
    if (at_once) finish_migration();
    else migrate_step(kMigrateGroups);
  }

  // Move up to `batch` old groups (from the front, in index order) to the
  // current table. Elements are moved, not copied (move_if_noexcept is not
  // applied: a throwing move here would leave the element destroyed).
  void migrate_step(size_t batch) {
    if (!ong_) return;
    const size_t stop = ong_ - front_ > batch ? front_ + batch : ong_;
    for (; front_ < stop; ++front_) {
      group* og = ogroups_ + front_;
      Mask m = og->match_occupied();
      while (m) {
        const unsigned l = group::lane(m);
        value_type* s = elem(oelems_, front_, l);
        const uint64_t h = hash_of(s->first);
        size_t g;
        unsigned nl;
        value_type* d = place_new(h, g, nl);
        construct(d, std::move(const_cast<key_type&>(s->first)), std::move(s->second));
        destroy(s);
        groups_[g].set(nl, og->m[l]);
        m &= m - 1;
      }
      og->clear_slots_keep_overflow();
    }
    if (front_ >= ong_) {
      free_old();
      reset_avail();
      if constexpr (kOwnMemory)
        detail::page_huge(elems_, ng_ * N * sizeof(slot_type), 0, ng_ * N * sizeof(slot_type), true);
      return;
    }
    if constexpr (kOwnMemory) manage_pages();
  }

  void manage_pages() noexcept {
    constexpr size_t gsz = N * sizeof(slot_type);
    const size_t old_bytes = ong_ * gsz;
    if (detail::is_mapped(old_bytes)) {
      const size_t done = front_ * gsz / detail::kHugePage * detail::kHugePage;
      if (done > old_released_) {
        detail::page_release(oelems_, old_bytes, old_released_, done);
        old_released_ = done;
      }
    }
    const size_t new_bytes = ng_ * gsz;
    if (detail::is_mapped(new_bytes)) {
      // Front in the new table: same relative position (fastrange is
      // monotonic). Hint two huge pages ahead to cover probe spill.
#if defined(__SIZEOF_INT128__)
      const size_t fg = static_cast<size_t>((static_cast<__uint128_t>(front_) * ng_) / ong_);
#else
      const size_t fg = static_cast<size_t>(static_cast<double>(front_) * ng_ / ong_);
#endif
      const size_t want = fg * gsz + 2 * detail::kHugePage;
      if (want > huge_hint_) {
        detail::page_huge(elems_, new_bytes, huge_hint_, want, false);
        huge_hint_ = detail::round_up(want, detail::kHugePage);
      }
    }
  }

  void free_old() noexcept {
    if (!ong_) return;
    free_groups(ogroups_, ong_);
    free_slots(oelems_, ong_);
    ogroups_ = nullptr;
    oelems_ = nullptr;
    ong_ = 0;
    front_ = 0;
    old_released_ = 0;
    huge_hint_ = 0;
  }

  // -------------------------------------------------------------- erase
  template <typename K>
  BFLAT_FORCEINLINE size_t erase_key(const K& k) {
    const uint64_t h = hash_of(k);
    size_t g;
    unsigned l;
    if (find_in(groups_, elems_, ng_, k, h, &g, &l)) {
      erase_new(g, l);
      return 1;
    }
    if (BFLAT_UNLIKELY(ong_ != 0) && find_old(k, h, &g, &l)) {
      erase_old(g, l);
      return 1;
    }
    return 0;
  }
  void erase_at(size_t i) noexcept {
    const size_t ncap = ng_ * N;
    if (i < ncap) erase_new(i / N, static_cast<unsigned>(i % N));
    else erase_old((i - ncap) / N, static_cast<unsigned>((i - ncap) % N));
  }
  // boost's recover_slot: if the slot may have caused overflow, lower the
  // max load (anti-drift); the slot becomes available.
  BFLAT_FORCEINLINE void erase_new(size_t g, unsigned l) noexcept {
    destroy(elem(elems_, g, l));
    group& gr = groups_[g];
    if (gr.maybe_caused_overflow(l)) --ml_;
    gr.reset(l);
    --size_;
    if (!ong_) reset_avail();
  }
  void erase_old(size_t g, unsigned l) noexcept {
    destroy(elem(oelems_, g, l));
    ogroups_[g].reset(l);
    --size_;
  }

  // ------------------------------------------------------ whole-table ops
  void destroy_all(bool dealloc) noexcept {
    if (!std::is_trivially_destructible<value_type>::value ||
        !std::is_same<Allocator, std::allocator<value_type>>::value) {
      for (size_t g = 0; g < ng_; ++g) {
        Mask m = groups_[g].match_occupied();
        while (m) {
          destroy(elem(elems_, g, group::lane(m)));
          m &= m - 1;
        }
      }
      for (size_t g = front_; g < ong_; ++g) {
        Mask m = ogroups_[g].match_occupied();
        while (m) {
          destroy(elem(oelems_, g, group::lane(m)));
          m &= m - 1;
        }
      }
    }
    if (dealloc) {
      free_old();
      if (ng_) {
        free_groups(groups_, ng_);
        free_slots(elems_, ng_);
      }
    }
  }
  void reset_members() noexcept {
    groups_ = empty_groups();
    elems_ = nullptr;
    ng_ = 0;
    size_ = 0;
    ml_ = 0;
    lim_ = 0;
    avail_ = 0;
    ogroups_ = nullptr;
    oelems_ = nullptr;
    ong_ = 0;
    front_ = 0;
    old_released_ = 0;
    huge_hint_ = 0;
  }
  static group* empty_groups() noexcept {
    return const_cast<group*>(detail::kEmptyGroups.g);
  }
  void steal(unordered_flat_map& o) noexcept {
    groups_ = o.groups_;
    elems_ = o.elems_;
    ng_ = o.ng_;
    size_ = o.size_;
    ml_ = o.ml_;
    lim_ = o.lim_;
    avail_ = o.avail_;
    ogroups_ = o.ogroups_;
    oelems_ = o.oelems_;
    ong_ = o.ong_;
    front_ = o.front_;
    old_released_ = o.old_released_;
    huge_hint_ = o.huge_hint_;
    o.reset_members();
  }
  void swap_storage(unordered_flat_map& o) noexcept {
    using std::swap;
    swap(groups_, o.groups_);
    swap(elems_, o.elems_);
    swap(ng_, o.ng_);
    swap(size_, o.size_);
    swap(ml_, o.ml_);
    swap(lim_, o.lim_);
    swap(avail_, o.avail_);
    swap(ogroups_, o.ogroups_);
    swap(oelems_, o.oelems_);
    swap(ong_, o.ong_);
    swap(front_, o.front_);
    swap(old_released_, o.old_released_);
    swap(huge_hint_, o.huge_hint_);
  }

  // Copy: same table layout (metadata copied, each element copied to its
  // slot, as boost) when the source is not migrating; else a compact table
  // and inserts.
  void copy_from(const unordered_flat_map& o) {
    if (o.size_ == 0) return;
    if (!o.ong_) {
      group* ngr = alloc_groups(o.ng_);
      slot_type* nel;
      try {
        nel = alloc_slots(o.ng_, true);
      } catch (...) {
        free_groups(ngr, o.ng_);
        throw;
      }
      size_t g = 0;
      try {
        if constexpr (std::is_trivially_copyable<value_type>::value && kOwnMemory) {
          std::memcpy(static_cast<void*>(nel), o.elems_, o.ng_ * N * sizeof(slot_type));
          g = o.ng_;
        } else {
          for (; g < o.ng_; ++g) {
            Mask m = o.groups_[g].match_occupied();
            while (m) {
              const unsigned l = group::lane(m);
              construct(elem(nel, g, l), *elem(o.elems_, g, l));
              ngr[g].set(l, o.groups_[g].m[l]);
              m &= m - 1;
            }
          }
        }
      } catch (...) {
        for (size_t i = 0; i <= g && i < o.ng_; ++i) {
          Mask m = ngr[i].match_occupied();
          while (m) {
            destroy(elem(nel, i, group::lane(m)));
            m &= m - 1;
          }
        }
        free_groups(ngr, o.ng_);
        free_slots(nel, o.ng_);
        throw;
      }
      std::memcpy(static_cast<void*>(ngr), o.groups_, o.ng_ * sizeof(group));
      groups_ = ngr;
      elems_ = nel;
      ng_ = o.ng_;
      size_ = o.size_;
      ml_ = o.ml_;
      lim_ = o.lim_;
      reset_avail();
      return;
    }
    try {
      reserve(o.size_);
      for (const auto& v : o) insert(v);
    } catch (...) {
      destroy_all(true);
      reset_members();
      throw;
    }
  }

  // ------------------------------------------------------------- members
  // Hot members first: find reads groups_, elems_, ng_ and (on a miss with
  // the overflow bit set) ong_.
  group* groups_ = empty_groups();
  slot_type* elems_ = nullptr;
  size_t ng_ = 0;       // groups in the current table
  size_t ong_ = 0;      // groups in the old table (!= 0 while migrating)
  size_t avail_ = 0;    // inserts left before the slow path (0 while migrating)
  size_t size_ = 0;     // elements in both tables
  size_t ml_ = 0;       // anti-drift max load (see set_limits)
  size_t lim_ = 0;      // load limit of the current table
  group* ogroups_ = nullptr;
  slot_type* oelems_ = nullptr;
  size_t front_ = 0;          // first old group not migrated
  size_t old_released_ = 0;   // bytes of old elements given back
  size_t huge_hint_ = 0;      // bytes of new elements with MADV_HUGEPAGE
  Hash hash_;
  KeyEqual eq_;
  Allocator al_;
};

template <typename K, typename T, typename H, typename E, typename A, typename Pred>
typename unordered_flat_map<K, T, H, E, A>::size_type erase_if(
    unordered_flat_map<K, T, H, E, A>& m, Pred pred) {
  typename unordered_flat_map<K, T, H, E, A>::size_type n = 0;
  for (auto it = m.begin(); it != m.end();) {
    if (pred(*it)) {
      it = m.erase(it);
      ++n;
    } else {
      ++it;
    }
  }
  return n;
}

}  // namespace bflat

#endif  // BFLAT_UNORDERED_FLAT_MAP_HPP
