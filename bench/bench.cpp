// bflat vs boost::unordered_flat_map vs sflat main. uint64 -> uint64.
// Hash: boost::hash<uint64_t> (identity) for all three. boost and bflat mix
// it with mulx (boost's policy for a non-avalanching hash); sflat always
// applies its own per-table seeded mulx-style mixer.
//
// Run: taskset -c 1 ./bench_X MODE N
//  MODE grow : insert N keys without reserve (throughput, untimed loop),
//              resident bytes/entry after, peak RSS during growth (VmHWM
//              above the baseline); then a fresh map with every insert timed:
//              max single insert, p99.9 / p99.99 / max of 16-insert batches.
//  MODE ops  : reserve(N) + insert, bytes/entry, find hit (shuffled order),
//              find miss, churn (erase a random live key + insert a new key,
//              min(N, 10M) times), find hit after churn.
//  MODE find : reserve + insert, then find hit / miss only (SIMD A/B).
#if defined(MAP_BOOST)
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/container_hash/hash.hpp>
using Map = boost::unordered_flat_map<uint64_t, uint64_t, boost::hash<uint64_t>>;
#elif defined(MAP_SFLAT)
#include <sflat/flat_hash_map.hpp>
#include <boost/container_hash/hash.hpp>
using Map = sflat::flat_hash_map<uint64_t, uint64_t, boost::hash<uint64_t>>;
#else
#include <bflat/unordered_flat_map.hpp>
#include <boost/container_hash/hash.hpp>
using Map = bflat::unordered_flat_map<uint64_t, uint64_t, boost::hash<uint64_t>>;
#endif
#ifndef MAP_NAME
#define MAP_NAME "bflat"
#endif
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <unistd.h>
#include <vector>

using u64 = uint64_t;
static inline u64 ticks() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
static double rss_bytes() {
  FILE* f = std::fopen("/proc/self/statm", "r");
  long a = 0, b = 0;
  if (f) { if (std::fscanf(f, "%ld %ld", &a, &b) != 2) b = 0; std::fclose(f); }
  return b * (double)sysconf(_SC_PAGESIZE);
}
static double hwm_bytes() {
  FILE* f = std::fopen("/proc/self/status", "r");
  char l[256];
  long v = 0;
  while (f && std::fgets(l, sizeof l, f))
    if (!std::strncmp(l, "VmHWM:", 6)) v = std::atol(l + 6);
  if (f) std::fclose(f);
  return v * 1024.0;
}
static void reset_hwm() {
  int fd = open("/proc/self/clear_refs", O_WRONLY);
  if (fd >= 0) { if (write(fd, "5", 1) != 1) {} close(fd); }
}
static volatile u64 sink;
static double finds(const Map& m, const std::vector<u64>& v) {
  u64 s = 0, t0 = ticks();
  for (u64 k : v) { auto it = m.find(k); s += it != m.end() ? it->second : 1; }
  sink = s;
  return (ticks() - t0) / (double)v.size();
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const char* mode = argv[1];
  const size_t n = std::strtoull(argv[2], nullptr, 10);
  std::mt19937_64 rng(42);
  std::vector<u64> keys(n), absent(n);
  for (auto& k : keys) k = rng() | 1;
  for (auto& k : absent) k = rng() & ~u64{1};

  if (!std::strcmp(mode, "grow")) {
    reset_hwm();
    const double base = rss_bytes();
    const double hwm0 = hwm_bytes();
    double ins_nr, bpe, peak_bpe;
    size_t cap;
    {
      Map m;
      const u64 t = ticks();
      for (u64 k : keys) m.emplace(k, k);
      ins_nr = (ticks() - t) / (double)n;
      bpe = (rss_bytes() - base) / n;
      peak_bpe = (hwm_bytes() - std::max(hwm0, base)) / n;
      cap = m.bucket_count();
    }
    // Latency run: every insert timed.
    constexpr size_t B = 16;
    std::vector<float> batch(n / B + 1);
    size_t nb = 0;
    u64 max1 = 0;
    {
      Map m;
      u64 prev = ticks(), bstart = prev;
      for (size_t i = 0; i < n; ++i) {
        m.emplace(keys[i], i);
        const u64 now = ticks();
        if (now - prev > max1) max1 = now - prev;
        prev = now;
        if ((i + 1) % B == 0) { batch[nb++] = (float)(now - bstart); bstart = now; }
      }
      sink = m.size();
    }
    std::sort(batch.begin(), batch.begin() + nb);
    auto pct = [&](double p) { return batch[std::min(nb - 1, (size_t)(p * nb))]; };
    std::printf("GROW map=%s n=%zu ins_nr_ns=%.2f cap=%zu bpe=%.2f peak_bpe=%.2f max1_us=%.1f "
                "b16_p999_us=%.2f b16_p9999_us=%.2f b16_max_us=%.1f\n",
                MAP_NAME, n, ins_nr, cap, bpe, peak_bpe, max1 / 1e3, pct(0.999) / 1e3,
                pct(0.9999) / 1e3, batch[nb - 1] / 1e3);
    return 0;
  }

  std::vector<u64> order(keys);
  std::shuffle(order.begin(), order.end(), rng);
  const bool find_only = !std::strcmp(mode, "find");
  const size_t churn_n = std::min<size_t>(n, 10000000);
  std::vector<u64> fresh, victim;
  if (!find_only) {
    fresh.resize(churn_n);
    victim.resize(churn_n);
    for (auto& k : fresh) k = rng() | 1;
    for (auto& v : victim) v = rng() % n;
  }
  const double base = rss_bytes();
  Map m;
  m.reserve(n);
  u64 t = ticks();
  for (u64 k : keys) m.emplace(k, k);
  const double ins_r = (ticks() - t) / (double)n;
  const double bpe = (rss_bytes() - base) / n;
  const size_t cap = m.bucket_count();
  const double hit = finds(m, order), miss = finds(m, absent);
  if (find_only) {
    std::printf("FIND map=%s n=%zu ins_r_ns=%.2f hit_ns=%.2f miss_ns=%.2f\n", MAP_NAME, n, ins_r,
                hit, miss);
    return 0;
  }
  t = ticks();
  for (size_t r = 0; r < churn_n; ++r) {
    const size_t j = victim[r];
    m.erase(keys[j]);
    keys[j] = fresh[r];
    m.emplace(keys[j], r);
  }
  const double churn = (ticks() - t) / (double)churn_n;
  if (m.size() != n) { std::printf("SIZE BAD\n"); return 1; }
  std::shuffle(keys.begin(), keys.end(), rng);
  const double hit2 = finds(m, keys);
  const double bpe2 = (rss_bytes() - base) / n;
  std::printf("OPS map=%s n=%zu ins_r_ns=%.2f cap=%zu bpe=%.2f hit_ns=%.2f miss_ns=%.2f "
              "churn_ns=%.2f hit_after_churn_ns=%.2f bpe_after_churn=%.2f cap_after=%zu\n",
              MAP_NAME, n, ins_r, cap, bpe, hit, miss, churn, hit2, bpe2, (size_t)m.bucket_count());
  return 0;
}
