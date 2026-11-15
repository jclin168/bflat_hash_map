# bflat::unordered_flat_map

bflat is a C++17 hash map for large data sets in memory.
The library has one header file.
It uses SIMD metadata checks to reduce full key comparisons.
It reserves capacity in small groups and reduces growth factors as table size increases.
During growth, it moves entries in steps and releases old resident pages.
These features balance operation time, unused capacity, and peak resident memory.

The map supports insertion, lookup, iteration, erasure, reservation, and rehashing.
It accepts custom hash functions, equality functions, and allocators.
It also supports transparent lookup and move-only values.
Growth can move entries and invalidate references.
The map does not give the reference stability of a node-based map.

## Installation and use

Add `include/` to the compiler include paths.
Use C++17 or a later version.

```cpp
#include <bflat/unordered_flat_map.hpp>
#include <cstdint>
#include <string>

int main() {
    bflat::unordered_flat_map<std::uint64_t, std::string> users;
    users.reserve(1000);
    users.try_emplace(42, "ready");
    if (auto it = users.find(42); it != users.end()) {
        it->second = "done";
    }
    users.erase(42);
}
```

## Design

### Groups and metadata

Each group has 15 element slots and 16 metadata bytes.
Each slot has one reduced-hash byte.
The last metadata byte contains overflow bits.
SSE2 or NEON instructions compare metadata bytes in parallel.
A scalar implementation is also available.

A reduced-hash match identifies an entry for a full key comparison.
Overflow bits identify hash classes that can have entries beyond the group.
These bits let a failed lookup stop before the end of the probe sequence.
Insertion first checks for an existing key.
It then searches for an available slot.

### Capacity and probe coverage

The group count does not need to be a power of two.
`reserve(n)` rounds the capacity to whole groups.
A table has at least two groups and one final sentinel slot.
Usable capacity is `15 * groups - 1`.
The normal entry limit is `floor(capacity * 7/8)`.
The two-group table can use all its slots.

Integer calculations prevent capacity errors from floating-point rounding.
The home group is the high half of `hash * group_count`.
The probe sequence starts with triangular steps.
A linear tail then gives full group coverage.
The sequence makes no more than twice the group count in visits.
Triangular steps alone do not give this coverage for all group counts.

### Hash markers

An avalanching hash function does not need additional mixing.
A hash type can declare `using is_avalanching = void`.
It can also declare a marker type with a true boolean `value`.
A false marker or an absent marker causes additional mixing.
The map also uses Boost hash traits when they are available.

### Growth factors

Small tables use large growth factors to reduce allocation and migration work.
Large tables use smaller factors to reduce unused capacity.
Thresholds use the current capacity bytes for element slots and metadata.

| Current table size | Growth factor | Threshold macro |
|---|---:|---|
| Less than 256 MiB | 2x | `BFLAT_GROW_2X_BELOW` |
| 256 MiB to less than 2 GiB | 1.5x | `BFLAT_GROW_1_5X_BELOW` |
| 2 GiB to less than 16 GiB | 1.25x | `BFLAT_GROW_1_25X_BELOW` |
| 16 GiB or more | 1.125x | — |

Growth adds `ceil(groups / divisor)` groups.
The divisors are 1, 2, 4, and 8.
The new table must also contain the requested entries and the anti-drift slack.
The slack request is `size/61 + 1` additional entries.
Threshold macros must be in increasing order.
Smaller growth factors reduce spare capacity but increase total migration work.

### Anti-drift maintenance

The map uses the Boost-style overflow decrement rule.
Erasure reduces the insertion budget when an entry can have caused overflow.
When this budget is empty, an insertion can start a rehash.
This rehash can occur with no increase in the live entry count.
The map keeps the current capacity if the entries and slack fit.
Otherwise, the map grows.

### Incremental migration

Element storage and group metadata use separate arrays.
With the default allocator, supported platforms use anonymous `mmap` mappings.
Custom allocators use allocator storage.
`BFLAT_USE_MMAP=0` also selects allocator storage.
Transparent huge-page hints are advisory.

Small tables complete migration in one operation.
The default limit for this behavior is 2 MiB.
`BFLAT_GROW_AT_ONCE_BELOW` controls this limit.
Larger tables move entries in steps during absent-key insertions.
Lookups check both tables while migration is active.

The migration frontier separates old groups that have moved from groups that have not moved.
A new entry can enter the old table if its old home group is beyond the frontier.
This rule limits writes ahead of the frontier in the new table.
The map releases eligible resident pages behind the frontier.

Old and new virtual mappings can exist at the same time.
Released resident pages do not remove the full old mapping.
A growth step from 64 GiB to 72 GiB can thus reserve approximately 136 GiB of virtual address space.
Some operations complete pending migration.
Incremental migration does not give a strict operation-time limit.

Call `finish_migration()` to complete pending migration.
Use this function before you measure settled memory.
Read-only lookup does not advance migration.

### Iterators and exceptions

Insertion during migration can invalidate iterators, pointers, and references.
Rehashing and explicit migration completion can also invalidate them.
Erasure does not move the surviving entries.
Use external synchronization for concurrent mutation.

Each entry leaves the old table after its move completes.
A throwing hash or move leaves each remaining entry in one table.
If an entry move throws, the map removes that entry.
Its key can already have changed.
The insertion passes the exception to the caller.

`find`, `contains`, and `count` can pass hash or equality exceptions to the caller.
Copy assignment releases old storage with the old allocator before allocator propagation.
Range insertion reserves capacity only if the range exceeds the current load limit.
It does not reduce table capacity.
`max_size()` gives the entry limit of the largest table.

### Capacity for repeated insertion and erasure

Reserve headroom can reduce growth caused by anti-drift maintenance.
For a known population, you can reserve 12.5% more entries:

```cpp
const std::size_t expected = 200'000'000;
map.reserve(expected + expected / 8 + (expected % 8 != 0));
```

The caller selects this headroom.
The `reserve` function does not add it automatically.
An insertion before an erasure temporarily adds one live entry.
Capacity must also permit this entry.
Headroom uses more initial memory and reduces the load factor.
It does not prevent all future rehashes.

## Benchmark summary

The tests used a Xeon Platinum 8573C processor and GCC 14.2.
Compiler options were `-std=c++17 -O3 -DNDEBUG -march=native`.
Each process used CPU 0.
The comparison used Boost 1.92 and sflat with pair-emplace optimization.
The results describe the measured conditions.
They do not give a general performance ranking.

### Operation time

The tables contained 50M `uint64_t` keys and values.
Each churn pair inserted one new key, then erased one live key.
Each configuration ran in three fresh processes.
The table shows median nanoseconds per insertion or erasure.
Lower values indicate less time.

| Churn pairs | bflat, reserve 56.25M | Boost, reserve 50M | sflat, reserve 50M |
|---|---:|---:|---:|
| 1M | 110.45 | 99.18 | 126.86 |
| 20M | 113.29 | 104.35 | 142.42 |

The capacities were approximately 64.29M, 62.91M, and 57.14M.
Thus, the configurations had different load factors.
Setup time is not in the results.
Rehash and growth time during churn are in the results.
The [summary data](docs/validation/churn-summary.csv) contains the measured values.

### Memory

Each configuration inserted 200M entries and then completed 20M churn pairs.
Each configuration used one fresh process.
Settled values apply after migration completion.
The unit GiB means 2^30 bytes.

| Container / reserve | Settled RSS | Settled VM | Peak RSS | Peak VM |
|---|---:|---:|---:|---:|
| bflat / 225M | 4.090 | 4.094 | 4.090 | 4.096 |
| bflat / 200M | 4.545 | 4.549 | 4.775 | 8.186 |
| Boost / 200M | 4.002 | 4.006 | 4.002 | 4.006 |
| sflat / 200M | 3.648 | 3.652 | 3.648 | 3.654 |

With headroom, bflat kept a capacity of 257,142,869 slots.
With exact reserve, bflat grew from 228,571,439 to 285,714,299 slots during churn.
RSS and VM peaks can occur at different times.
The [memory report](docs/validation/memory-200M.md) gives more conditions and results.
These memory and operation-time measurements were made before the exception-safety review fixes.

## Validation

Run the unit tests with Boost available:

```sh
BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh
```

Run the Ankerl corpus replay:

```sh
bash tests/ankerl/run.sh
```

The unit tests cover API behavior, group coverage, growth factors, weak hashes, and differential checks.
They also cover scalar and SSE2 paths, ASan, and UBSan.
Recorded baseline validation passed 12 configurations.
The runner also includes tests for allocator propagation, exceptions, size limits, and range insertion.

The Ankerl replay uses 363 unchanged corpus files from two workloads.
The workloads are `fuzz_insert_erase` and `fuzz_string`.
Each file runs without reserve, with exact reserve, and with 12.5% headroom.
All 1,089 cases passed in each release and sanitizer configuration.
This result covers the two corpus workloads.
It does not cover the full upstream test suite or continuous fuzzing.
See [validation records](docs/validation) for sources and results.

## Technical terms

| Term | Meaning in this project |
|---|---|
| Slot | Storage for one key-value entry. |
| Group | Fifteen slots with sixteen metadata bytes. |
| Reduced hash | A hash byte that selects possible key matches. |
| Home group | The first group in a key probe sequence. |
| Sentinel | A reserved slot that marks the table end. |
| Load factor | The live entry count divided by usable slot capacity. |
| Migration | Transfer of entries from an old table to a new table. |
| Migration frontier | The boundary between moved and unmoved old groups. |
| Anti-drift | Maintenance that limits overflow effects after erasure. |
| Churn | Repeated insertion and erasure with a stable live population. |
| Corpus | Stored input files for test replay. |
| RSS | Resident set size: memory pages resident in physical memory. |
| VM | Virtual address space reserved by the process. |

## License

The [Boost Software License](LICENSE) applies to bflat.
The [MIT license](tests/ankerl/LICENSE) applies to the bundled Ankerl test dependencies.
