# bflat: Boost table policies with page-managed incremental growth

This version retains the Boost.Unordered 1.92 table policies requested for
comparison. The original prototype design and its historical measurements
are archived in [docs/prototype-7c24911.md](docs/prototype-7c24911.md). Those
measurements do not describe this changed version.

## Policies matching Boost

- Power-of-two group counts (minimum two), 15 slots per group, with the last
  slot reserved for the sentinel: capacity is `15 * groups - 1`.
- `reserve`, `rehash`, and initial maximum load use Boost's rounding and
  0.875 load policy; the smallest table permits full usage.
- Home group uses the high hash bits. Triangular quadratic probing wraps
  with `& (groups - 1)`, visits each group once, and has no linear tail.
- Insertion checks for an existing key, then probes for an available slot.
  The prototype's one-metadata-load insertion shortcut has been removed.
- Erasing a potentially overflowing element lowers the effective maximum
  load. The next absent insertion at that limit rehashes using Boost's
  `ceil(float(size + size/61 + 1) / 0.875)` capacity request.
  There is no extra drift budget or in-place overflow-only refresh.

## Memory behavior retained

Separate metadata/element arrays, Linux mmap, THP hints, incremental
migration, and release of migrated old pages are retained. Small tables
migrate synchronously; `BFLAT_GROW_AT_ONCE_BELOW` and
`BFLAT_MIGRATE_GROUPS` still control migration timing. The former 1.5x/1.25x
capacity tiers are removed: rounding now follows Boost's capacity levels.

During migration, operations may inspect two arrays, inserts advance the
migration, and iterator/reference guarantees still differ from Boost.
The SIMD backend and allocator implementation also remain bflat's. Matching
these table policies does not claim identical generated instructions or
full API/exception guarantees. `finish_migration()` is available when a
single-table state is required.

## Tests

Run `BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh`.
The runner checks API behavior, differential operations and growth,
incremental migration, scalar and x86 SSE2 paths, and ASan/UBSan.
`test_boost_policy.cpp` compares capacity and anti-drift max_load against
Boost after each operation (finishing migrations), under ordinary and weak
hashes, and checks complete quadratic probe traversal.

Boost Software License terms are in [LICENSE](LICENSE).
