# bflat::unordered_flat_map

bflat is a header-only C++17 hash map for large in-memory datasets. It combines
SIMD-assisted open addressing with precise capacity reservation, progressively
smaller growth factors, and incremental migration that releases old resident
pages as entries move. Its goal is to balance lookup and insertion throughput
with predictable capacity overhead and lower resident-memory peaks during growth.

The interface includes `emplace`, `try_emplace`, `operator[]`, lookup, iteration,
erasure, `reserve`, and `rehash`. It supports custom hash/equality functions,
transparent lookup, move-only mapped values, and custom allocators. It is a flat
container: growth and migration can move elements, so it does not provide the
reference stability of a node-based map.

## Quick start

Add `include/` to the compiler's include paths; compile with C++17 or later.

```cpp
#include <bflat/unordered_flat_map.hpp>
#include <cstdint>
#include <string>

int main() {
    bflat::unordered_flat_map<std::uint64_t, std::string> users;
    users.reserve(1000);
    users.try_emplace(42, "Alice");
    if (auto it = users.find(42); it != users.end()) {
        it->second = "Bob";
    }
    users.erase(42);
}
```

## Design

### SIMD groups and overflow tracking

Each group contains 15 element slots and 16 bytes of metadata: one reduced-hash
byte per slot and an overflow byte. SSE2 or NEON compares metadata in parallel;
a scalar implementation is also available. Reduced-hash matches identify which
keys require full equality checks. Overflow bits record hash classes that have
probed beyond a group, allowing unsuccessful lookups to stop early.

Insertion first searches for an existing key, then probes for an available slot.
This keeps duplicate detection separate from placement and preserves the
Boost.Unordered-style two-stage insertion and overflow-based anti-drift policy.
Erasing an entry that may have caused overflow reduces the allowed insertion
budget. Exhausting that budget can trigger rehashing, even if the live population
has not increased. A rehash keeps the current capacity when the required entries
and slack fit; otherwise it grows the table.

### Precise capacity and complete probing

A group count need not be a power of two. `reserve(n)` rounds to whole 15-slot
groups, with at least two groups and one final sentinel slot. Thus usable
capacity is `15 * groups - 1`. The live-entry limit is exactly
`floor(capacity * 7/8)`; the smallest two-group table can use its full capacity.
Integer arithmetic avoids large-table floating-point rounding errors.

The home group is the high half of `hash * group_count`. A triangular probe
prefix is followed by a linear tail, covering every group within at most twice
the group count in visits. This coverage is necessary because triangular probing
alone does not guarantee full coverage for arbitrary group counts.

Hash functions marked avalanching bypass additional mixing. The marker may be
`using is_avalanching = void` or a type whose boolean `value` is true. False or
absent markers retain mixing. Boost hash traits are consulted when available.

### Growth proportional to table size

Small tables favor fewer allocations; large tables favor smaller capacity jumps.
Thresholds use the current element-slot and metadata capacity in bytes.

| Current table bytes | Growth factor | Threshold macro |
|---|---:|---|
| Below 256 MiB | 2x | `BFLAT_GROW_2X_BELOW` |
| 256 MiB to below 2 GiB | 1.5x | `BFLAT_GROW_1_5X_BELOW` |
| 2 GiB to below 16 GiB | 1.25x | `BFLAT_GROW_1_25X_BELOW` |
| 16 GiB and above | 1.125x | — |

Growth adds `ceil(groups / divisor)` groups, with divisors 1, 2, 4, and 8,
and must accommodate the requested live entries and anti-drift slack
(`size/61 + 1` additional entries). Threshold macros must be ordered.
Multiplicative growth preserves amortization; smaller factors trade more total
migration work for less unused capacity. For example, the last tier grows a
64 GiB table to approximately 72 GiB.

### Migration and the memory model

Element storage and group metadata use separate arrays. On supported platforms,
the default allocator uses anonymous `mmap` mappings and transparent-huge-page
hints. Custom allocators and builds with `BFLAT_USE_MMAP=0` use allocator-backed
storage. Huge-page hints are advisory.

Tables below `BFLAT_GROW_AT_ONCE_BELOW` (2 MiB by default) migrate in one step.
Larger tables migrate incrementally as absent-key insertions advance the
migration frontier. Lookups consult both tables while migration is active.
New entries whose old home group has not been migrated can enter the old table,
keeping writes near the frontier in the new table. Reclaimable pages behind
the frontier are released to reduce resident-memory overlap.

Resident memory and virtual address space measure different costs. Old and new
mappings can coexist even after old resident pages are released. A 64-to-72 GiB
resize can therefore reserve roughly 136 GiB of virtual address space. Migration
is not a strict latency bound: some operations can complete pending migration.
Call `finish_migration()` explicitly to complete it before measuring settled
memory or entering a phase that should not retain the old table.

Treat insertion during migration, rehashing, and explicit migration completion
as invalidating iterators, pointers, and references. Erasing an element does not
move surviving elements. Read-only lookup does not advance migration. External
synchronization is required for concurrent mutation.

### Reserve for sustained churn

For a known population that repeatedly inserts and erases, reserve headroom can
reduce anti-drift-triggered growth:

```cpp
const std::size_t expected = 200'000'000;
map.reserve(expected + expected / 8 + (expected % 8 != 0));
```

This requests 12.5% more entries; it is a workload choice, not an automatic
adjustment inside `reserve`. Insert-before-erase temporarily holds one extra
entry, which must also fit. Headroom reduces occupancy and consumes additional
memory initially; it does not guarantee that arbitrarily long churn avoids
rehashing. Different containers round the same reserve request differently.

## Benchmark snapshot

Xeon Platinum 8573C, GCC 14.2, C++17, `-O3 -DNDEBUG -march=native`, CPU 0;
Boost 1.92 and sflat with its pair-emplace optimization. These results apply to
the stated workloads and occupancy levels, rather than a general speed ranking.

At 50M `uint64_t` keys and values, insert-before-erase churn, median of three
fresh processes, nanoseconds per individual insert or erase (lower is better):

| Churn pairs | bflat, reserve 56.25M | Boost, reserve 50M | sflat, reserve 50M |
|---|---:|---:|---:|
| 1M | 110.45 | 99.18 | 126.86 |
| 20M | 113.29 | 104.35 | 142.42 |

Capacities were approximately 64.29M, 62.91M, and 57.14M respectively, so these
columns have different occupancy. Setup is outside the timer; growth or rehash
triggered during churn is included. One insert plus one erase is two operations.

At 200M entries followed by 20M insert-before-erase churn pairs, each configuration
ran in a fresh process. GiB means 2^30 bytes; settled values are after migration.

| Container / reserve | Settled RSS | Settled VM | Peak RSS | Peak VM |
|---|---:|---:|---:|---:|
| bflat / 225M | 4.090 | 4.094 | 4.090 | 4.096 |
| bflat / 200M | 4.545 | 4.549 | 4.775 | 8.186 |
| Boost / 200M | 4.002 | 4.006 | 4.002 | 4.006 |
| sflat / 200M | 3.648 | 3.652 | 3.648 | 3.654 |

bflat with headroom kept capacity at 257,142,869 throughout; exact reserve grew
from 228,571,439 to 285,714,299 during churn. The memory study is a single run per
configuration; RSS and VM peaks need not occur simultaneously. See the
[200M memory report](docs/validation/memory-200M.md) for full conditions and
additional reserve comparisons.

## Validation

```sh
BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh
bash tests/ankerl/run.sh
```

The unit runner covers API behavior, arbitrary-group probe coverage, growth tiers,
weak hashes, differential checks against `std::unordered_map`, scalar/SSE2 paths,
and ASan/UBSan. All 12 configurations passed with Boost available.

The Ankerl replay includes 363 unchanged corpus files from `fuzz_insert_erase`
and `fuzz_string`, each checked without reserve, with exact reserve, and with
12.5% headroom: 1,089 cases per configuration. Release and ASan/UBSan with forced
incremental migration passed. This covers those two corpus workloads, not the
entire `unordered_dense` test suite or continuous coverage-guided fuzzing.
Sources, licenses, corpus, and results are included under `tests/ankerl` and
[docs/validation](docs/validation).

## License

bflat is distributed under the Boost Software License; see [LICENSE](LICENSE).
Bundled Ankerl test dependencies retain their [MIT license](tests/ankerl/LICENSE).
