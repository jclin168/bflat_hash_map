# bflat: boost::unordered_flat_map with sflat memory management (prototype)

`bflat::unordered_flat_map<K, V, Hash, Eq, Alloc>` is a C++17 header-only map
(`include/bflat/unordered_flat_map.hpp`). It keeps the Boost.Unordered "foa"
group architecture and replaces its memory management with the design of
sflat_hash_map (origin/main aaded6f):

- Non-power-of-2 capacity.
- Small tables grow at once. Large tables grow by tiers, with incremental
  migration.
- Linux mmap, with page release behind the migration front and THP hints.

Only the map is implemented. The set was not done (time).

Status: prototype. All tests pass (see "Tests"). Measured on aarch64
Neoverse-V2 (2 vCPU VM) only.

## Design

### Layout (as boost)

- Groups of 16 bytes: metadata of 15 slots plus 1 overflow byte (index 15).
- Metadata byte 0 means "available". Other values are the reduced hash:
  the low byte of the hash, with 0 and 1 mapped to 8 and 9 (boost's table,
  254 values).
- Overflow byte: bit `hash % 8` is set on each full group that an insert
  passes.
- Two arrays: groups (64-byte aligned) and elements (`ng * 15` slots).
  Boost uses one allocation. Two arrays let the element pages be released
  separately.
- Memory per slot for `uint64 -> uint64`: 16 + 16/15 = 17.07 bytes. At load
  0.875 this is 19.5 bytes/entry.

### Hash and position

- `h = mulx(hash(k))`, as boost, unless the hash is avalanching. The code uses
  `boost::hash_is_avalanching` when Boost is present, else a nested
  `is_avalanching` typedef.
- Home group: `fastrange(h, ng) = (h * ng) >> 64`, the 128-bit product, one
  `umulh` on AArch64. This is monotonic in `h`, the same property that boost's
  `h >> shift` has. The ordered migration relies on it.

### Probing

- Triangular steps (`pos += step`) modulo the group count `ng`, as boost's
  `pow2_quadratic_prober`.
- Triangular numbers modulo `ng` visit all groups only when `ng` is a power
  of two. Thus after `ng - 1` triangular steps the sequence continues with
  `ng` linear steps. Insert always finds an available slot, and every probe
  stops after `2 * ng` steps. In practice probes stop after 1 to a few groups.
  The linear tail only bounds the worst case.
- Early miss: find stops at the first group whose overflow bit for `h` is
  clear (boost semantics). Insert, migration and old-table inserts all use the
  same sequence and mark every full group they pass. Thus the early-miss rule
  stays correct across wraparound.

### Capacity

- `reserve(n)` gives `ceil(n / 0.875)` slots, rounded up to 15-slot groups.
- Max load 0.875. Tables of 1 or 2 groups allow 100% (as boost's
  `2N - 1` rule).
- `max_load_factor(x)` has no effect (as boost).

### Growth

- The table grows only when the elements reach the load limit.
- The growth factor depends on the table size (elements plus metadata), as
  sflat: 2x below 2 GiB, 1.5x below 10 GiB, else 1.25x (`BFLAT_GROW_2X_BELOW`,
  `BFLAT_GROW_1_5X_BELOW`).
- Tables below 2 MiB (`BFLAT_GROW_AT_ONCE_BELOW`) move all elements in the
  insert that starts the growth.

### Incremental migration (sflat design)

- The old and new arrays live together.
- Each insert that comes to the slow path moves the next 16 old groups (240
  slots; sflat moves 256 slots) to the new table, in index order (front
  cursor `front_`).
- `find`, `erase` and iteration never move elements.
- During a migration the fast path of insert is off (`avail_ = 0`), as sflat.
- After a group is moved, its slot bytes become 0 and its overflow byte is
  kept. Thus a probe of the old table passes through the moved groups exactly
  as before and stops at the same place. `find` checks the new table first,
  then (only on a miss) the old table.
- A new key whose old home group is at or after the front goes into the old
  table, as sflat. Then the new table gets writes only near the front, in
  order. If the old-table probe reaches a moved group (wraparound), the key
  goes to the new table.
- `finish_migration()` and `is_migrating()` are extensions.
- `rehash` / `reserve` finish a migration first, then rehash at once (in
  index order).

### Memory (Linux, `std::allocator`)

- Arrays of 2 MiB or more get their own 2 MiB-aligned `mmap`.
- Old element pages behind the front are released with `MADV_DONTNEED`.
- New element pages get `MADV_HUGEPAGE` from the front up to two huge pages
  ahead. Pages farther ahead stay `MADV_NOHUGEPAGE`. At the end of a
  migration, `MADV_COLLAPSE`.
- Group arrays and the elements of a `reserve` / `rehash` table get
  `MADV_HUGEPAGE` at once.
- Because "available" is 0, fresh mmap pages are already valid metadata.
  `reserve` writes nothing, so all pages are committed lazily. sflat writes
  `0xFF` control bytes.
- A custom allocator, or `BFLAT_USE_MMAP=0`, uses the allocator only (no
  page release, no THP).

### Anti-drift (changed from boost)

- Boost lowers `ml` on each erase from a group whose overflow bit for that
  element is set. It rehashes (and grows by `size/61`) when `size` reaches
  `ml`.
- With bflat's exact capacity (`reserve(n)` gives load 0.875), this made an
  erase+insert churn at full load grow the table 2x (measured: 1M reserved
  table to 2.29M slots).
- bflat separates the two conditions:
  - Growth happens only at the load limit `lim_`.
  - `ml_` starts at `lim_` plus sflat's "used limit" budget: 3/8 of the
    slots above the load limit. Drift lowers `ml_` as in boost.
  - When the elements reach `ml_` below `lim_`, `refresh_overflow()`
    recomputes all overflow bytes in place. It clears them, then for each
    element outside its home group it replays its probe sequence and marks
    the groups that it passes. No element moves and no memory is allocated.
- Under churn this runs about 6 times per n operations. It costs about
  6 ns/slot: a pause of about 6 ms at 1M and 60 ms at 10M (sflat's in-place
  rebuild is in the same range).

### SIMD (each item measured, see "SIMD A/B")

- SSE2: boost's `pcmpeqb` + `pmovmskb` (`& 0x7FFF`). Kept for x86. There is
  no AVX2-specific path, because the group is 16 bytes.
- NEON: `cmeq` + `shrn #4` gives a 64-bit mask with 4 bits per lane
  (`& 0x0111...`), instead of boost's movemask emulation
  (`vand` + `vzip` + `vaddv`). The element byte offset is `ctz(m) * (S/4)`
  (one shift), not `(ctz >> 2) * S`.
- Insert: one metadata load of the home group gives the fingerprint match
  and the available mask. If the key is not in the home group, the overflow
  bit is clear and a slot is available, the key goes there. Boost does
  `find`, then a second probe for an available slot.
- Prefetch: boost's prefetch-on-match is kept. sflat's prefetch before the
  metadata load was slower here.
- Reduced hash: boost's 256-byte table is kept. The arithmetic form was not
  faster.
- Scalar fallback (`BFLAT_NO_SIMD=1`) for other CPUs.

## Deviations

### From boost::unordered_flat_map (changed API guarantees)

| Item | boost | bflat |
|---|---|---|
| Iterator / reference invalidation | Only when an insert causes a rehash | Also **any insert while `is_migrating()`**: it moves up to 16 old groups. Old-table elements can move although the capacity does not change. `erase` and `find` never move elements. |
| Iterator after `swap` / move of the container | Stays valid (points into the other container) | Invalid. The iterator holds a pointer to the map, so that it can go from the new table to the old one. |
| Iterator size | 2 pointers | 3 words (map, element, slot index) |
| `bucket_count()` | `2^k * 15 - 1` | `groups * 15`, any group count |
| `max_load()` | drifted `ml` | `min(load limit, drifted budget)` |
| Growth trigger | `size == ml` (also by drift; grows by `size/61`) | only at the load limit; drift: in-place overflow refresh |
| `rehash` / `reserve` / copy while migrating | n/a | finish the migration first / copy into a compact table |
| Element move during growth | `move_if_noexcept` | move constructor (a throwing move during a migration step would lose the element) |
| Default hash | `boost::hash` | `std::hash` (the benchmark passes `boost::hash`) |
| Not implemented | | `merge`, `pull`, stats, fancy pointers, `concurrent_flat_map` conversion, set |

The exception guarantee of insert: if the construction throws, nothing is
inserted, but a growth that this insert started stays.

### From sflat (main)

- Boost groups (15 slots + 8-bit overflow byte in the group) instead of
  16-byte control groups and a 32-bit overflow word per group.
- Triangular probing modulo `ng` (with a linear tail) instead of linear group
  probing.
- No tombstones (boost `erase` makes the slot available). Drift is handled by
  the budget and the in-place overflow refresh, instead of
  `drop_tombstones()`.
- Fastrange on 64 bits (`umulh`) instead of the 32-bit form.
- No per-table seed, multiplier table or reseed safety net: boost's fixed
  mulx. Inserting a 1M table in its iteration order into an empty table is
  fast (in `test_api`). Quadratic probing spreads the pile-up, but there is no
  guarantee.
- Moved old groups: slots cleared, overflow byte kept (the probe passes
  through). sflat marks the groups deleted and jumps to the front.
- Copy: boost-style clone of the same layout (memcpy for trivially copyable
  elements). No `SFLAT_COPY_SLACK`.

## Configuration macros

| Macro | Default | Effect |
|---|---|---|
| `BFLAT_USE_MMAP` | 1 on Linux | 0: allocator only |
| `BFLAT_USE_THP` | 1 | 0: no huge page hints |
| `BFLAT_GROW_2X_BELOW` | 2 GiB | tier limit |
| `BFLAT_GROW_1_5X_BELOW` | 10 GiB | tier limit |
| `BFLAT_GROW_AT_ONCE_BELOW` | 2 MiB | smaller tables grow at once; 0 = always incremental |
| `BFLAT_MIGRATE_GROUPS` | 16 | old groups moved per insert |
| `BFLAT_NEON_SHRN` | 1 | 0: boost's NEON movemask emulation |
| `BFLAT_INSERT_ONE_LOAD` | 1 | 0: boost order (find, then probe again) |
| `BFLAT_PREFETCH` | 1 | 0 none, 1 boost (on match), 2 sflat (before the metadata load) |
| `BFLAT_RH_TABLE` | 1 | 0: arithmetic reduced hash |
| `BFLAT_NO_SIMD` | 0 | 1: scalar group |

## Tests

`tests/run_tests.sh` builds and runs all configurations. The output is in
`results/tests.txt`.

- `tests/test_diff.cpp`: randomized differential test against
  `std::unordered_map`. Operations: `emplace`, `insert`, `try_emplace`,
  `insert_or_assign`, `operator[]`, `erase` (by key, by iterator, by range),
  `erase_if`, `find`, `count`, `contains`, `rehash`, `reserve`, `clear`, copy,
  copy assignment, move, swap, and full iteration compare with an internal
  invariant check.
  - Key types: `uint64` (small and large key spaces) and `std::string`.
  - Hashes: a bad hash (64 values, mixed) and an unmixed 13-value hash marked
    avalanching.
  - Growth runs: `uint64` 0 to 20M and string 0 to 2M, with checks during
    migrations, then erase and re-insert.
- `tests/test_api.cpp`:
  - reserve formula;
  - growth tier factors (also with lowered tier limits);
  - move-only values, `at`, `equal_range`;
  - heterogeneous lookup;
  - pointer stability across erase during a migration;
  - erase while iterating during a migration;
  - a custom allocator (no mmap, all memory returned);
  - rehash and shrink while migrating;
  - insert of a table's iteration order.
- Configurations (all PASS):

  | Configuration | Build |
  |---|---|
  | Default | g++ -O2 |
  | Always incremental, 1 group per step | `BFLAT_GROW_AT_ONCE_BELOW=0`, `BFLAT_MIGRATE_GROUPS=1` |
  | Lowered tiers (2x / 1.5x / 1.25x all used) | lowered tier limits |
  | NEON emulation | `BFLAT_NEON_SHRN=0` |
  | Scalar + boost-order insert + sflat prefetch + arithmetic reduced hash | `BFLAT_NO_SIMD=1` and the other macros |
  | ASan + UBSan (clang) | incremental, with mmap and without mmap (so that ASan sees the element arrays) |
  | x86-64 SSE2 and `-mavx2` | `x86_64-linux-gnu-g++` 14, `-static`, run under `qemu-x86_64` (functional only, small scale) |

- Not tested: exceptions from throwing constructors or moves; the 1.5x and
  1.25x tiers at real size (above 2 GiB; only with lowered limits).

## Benchmark

### Setup

- Machine: aarch64 Neoverse-V2, 2 vCPU VM, 7.7 GiB, THP `madvise`, kernel
  6.12.
- Compiler: g++ 14.2 `-O3 -mcpu=native -DNDEBUG`. Runs use `taskset -c 1`.
  Maps are interleaved, 3 rounds. The tables show the median (raw files have
  all rounds and the min..max).
- Maps: Boost 1.92.0 (`unordered_flat_map`); sflat main aaded6f; bflat at
  the commit of this README.
- Hash: `boost::hash<uint64_t>` (identity) for all three. Boost and bflat
  apply mulx. sflat applies its own seeded mixer.
- Keys: random 64-bit. Find hit uses shuffled order.
- Churn: erase a random live key and insert a new key, `min(n, 10M)` times.
- Memory: RSS above the baseline (after the key vectors). Peak = `VmHWM`
  above the baseline (after a `clear_refs` reset).
- Latency: second no-reserve run with every insert timed (`steady_clock`).
  "max" is the largest single insert. p99.9 / p99.99 are of 16-insert
  batches.
- Harness: `bench/bench.cpp`, `bench/build.sh`, `bench/run_main.sh`,
  `bench/summ.py`. Raw data: `results/main_raw.txt`. Summary:
  `results/main_summary.txt`.

Load after `reserve(n)`:

| n | bflat | boost | sflat |
|---|---|---|---|
| 1M | 0.875 | 0.51 | 0.875 |
| 10M | 0.875 | 0.64 | 0.875 |
| 50M | 0.875 | 0.79 | 0.875 |

### Results (ns/op unless noted; median of 3)

| metric | 1M bflat | 1M boost | 1M sflat | 10M bflat | 10M boost | 10M sflat | 50M bflat | 50M boost | 50M sflat |
|---|---|---|---|---|---|---|---|---|---|
| insert, reserve | **14.2** | 34.1 | 15.2 | **34.4** | 45.1 | 37.1 | **50.0** | 87.2 | 52.4 |
| insert, no reserve | 51.5 | 69.8 | **48.9** | 58.9 | 72.1 | **56.7** | **63.8** | 109.0 | 65.0 |
| find hit (random order) | **20.3** | 20.8 | 23.1 | 32.8 | **29.2** | 34.9 | **37.3** | 49.5 | 38.8 |
| find miss | 10.0 | **8.8** | 11.7 | 23.2 | **23.1** | 28.4 | **27.1** | 30.8 | 33.6 |
| churn (erase + insert) | 163.6 | **111.1** | 177.2 | 256.3 | **163.3** | 284.2 | **257.4** | 276.3 | 340.9 |
| find hit after churn | 24.0 | **22.4** | 26.2 | 37.7 | **29.7** | 37.3 | **38.8** | 51.7 | 43.6 |
| bytes/entry, reserved | **20.09** | 33.55 | 20.32 | **19.67** | 26.84 | 20.13 | **19.51** | 21.47 | 19.76 |
| bytes/entry, no reserve (end) | 35.66 | **33.69** | 37.76 | 27.05 | **26.86** | 28.96 | 21.52 | **21.48** | 23.16 |
| peak bytes/entry during growth | **39.68** | 50.44 | 39.79 | **28.10** | 40.28 | 30.21 | **22.23** | 32.21 | 24.03 |
| max single insert (ms) | **1.35** | 25.8 | 1.72 | **1.63** | 172.1 | 2.25 | **3.77** | 664.8 | 9.54 |
| p99.9 of 16-insert batches (us) | 81.4 | **8.6** | 80.7 | 72.7 | **12.6** | 50.1 | 65.0 | **19.1** | 49.2 |
| p99.99 of 16-insert batches (us) | **189** | 336 | 306 | 153 | **23.4** | 121 | 142 | **30.9** | 124 |

### Equal load and THP effect (find mode; `results/extra_raw.txt`)

At n = 13,762,559, boost is also at load 0.875 (its max for 2^20 groups).
bflat-noTHP is `BFLAT_USE_THP=0`.

| metric | 13.76M bflat | 13.76M bflat-noTHP | 13.76M boost | 13.76M sflat | 50M bflat | 50M bflat-noTHP | 50M boost | 50M sflat |
|---|---|---|---|---|---|---|---|---|
| insert, reserve | 37.1 | 43.6 | 47.1 | 40.6 | 50.3 | 83.8 | 88.8 | 52.0 |
| find hit | 35.5 | 35.0 | 34.4 | 37.3 | 38.9 | 54.8 | 54.7 | 39.6 |
| find miss | 24.8 | 28.2 | 31.5 | 30.8 | 28.1 | 33.5 | 30.6 | 34.7 |

Reading:

- At 50M, bflat's find hit advantage over boost comes from THP (without THP
  it is equal).
- At equal load, find hit is a tie. bflat's miss is faster than boost also
  without THP (28.2 vs 31.5).
- At 10M, boost's faster hit and churn come from its lower load (0.64).

### SIMD A/B (bflat variants, one macro changed; `results/ab*_raw.txt`)

| change (kept / dropped) | metric | 1M A | 1M B | 10M A | 10M B |
|---|---|---|---|---|---|
| one-load insert (kept) vs boost order | insert, reserve | **14.8** | 18.4 | **36.4** | 48.5 |
| NEON shrn + ctz offset (kept) vs boost emulation | find hit | **20.7** | 21.2 | **33.8** | 33.9 |
| same | find miss | **10.1** | 10.8 | **23.1** | 25.5 |
| same | insert, reserve | **12.5** | 13.7 | **32.8** | 35.8 |
| boost prefetch on match (kept) vs none vs sflat pre-metadata | find hit | **22.3** / 23.0 / 25.6 | | **36.5** / 40.5 / 40.5 | |
| arithmetic reduced hash (dropped) vs boost table | find hit | 20.7 | **19.9** | 33.8 | **33.0** |
| same | find miss | 10.1 | **9.8** | 23.1 | **22.9** |

Rounds: 5 for one-load and prefetch; 7 for the NEON and reduced-hash rows.

The first NEON A/B (before the `ctz * (S/4)` offset) had shrn slower on hits
(36.5 vs 34.2 at 10M). The extra shift on the hit path caused it.

## What did not work / limits

1. Boost's anti-drift with exact capacity grew a reserved table 2x under
   churn. It was replaced (see "Anti-drift").
2. Churn is 1.5x slower than boost at 1M and 10M (boost has load 0.51 or
   0.64), and 7% faster at 50M. Part of the cost is the in-place overflow
   refresh. It is a pause of about 6 ns/slot (60 ms at 10M).
3. Incremental migration trades the single spike for many medium batches:
   p99.9 of 16 inserts is 65 to 80 us (each insert moves about 200 elements),
   against 9 to 19 us for boost. The worst insert is 1.3 to 3.8 ms against
   26 to 665 ms. The 1M worst case (1.35 ms) is the at-once growth of the
   last table below 2 MiB.
4. The sflat-style prefetch and the arithmetic reduced hash did not help.
   They were dropped (the macros remain for A/B).
5. x86 was compiled and functionally tested under qemu only. There are no
   x86 timings.
6. There is no set, and no exception tests with throwing types.

## Files

- `include/bflat/unordered_flat_map.hpp`: the map.
- `tests/test_diff.cpp`, `tests/test_api.cpp`, `tests/run_tests.sh`.
- `bench/bench.cpp`, `bench/build.sh`, `bench/run_main.sh`, `bench/run_ab.sh`,
  `bench/run_ab2.sh`, `bench/run_extra.sh`, `bench/summ.py`.
- `results/`: raw and summary files of all runs, and `tests.txt`.
- The third-party sources are outside the repository in `../thirdparty/`:
  Boost 1.92.0 headers, and the sflat main aaded6f `include/` from
  `git archive`.
