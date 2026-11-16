# Tests adapted from sflat

## Source

The source is `sflat_hash_map` commit `a0ac82ad06c00f9ee7bbe9d3428faf63745ffdce`.
The adapted files retain the required source attribution.
The [Apache license](../../tests/sflat/LICENSE) applies to the adapted tests.
The pair-emplace tests also include const-pair and conversion regression cases.

## Coverage

| bflat test | Source cases | Checks |
|---|---|---|
| `test_sflat_interface.cpp` | `map_interface.cpp` | Types, constructors, assignment, iterators, insertion, lookup, observers, and comparison. |
| `test_sflat_emplace.cpp` | Pair-emplace cases | Copy and move counts, const pairs, conversions, references, exceptions, and duplicates. |
| `test_sflat_copy.cpp` | `copy_correctness.cpp` | Hash state, equality state, independent copies, updates, migration, and failed assignment. |
| `test_sflat_stress.cpp` | `correctness.cpp` | Sequential keys, seven key patterns, self-aliasing, fixed-population churn, iteration erasure, and copy-clear-shrink insertion. |
| `test_sflat_lifecycle.cpp` | Input ranges, migration, and safety-net cases | Single-pass ranges, self-ranges, copy, move, clear, and 10,000 colliding keys. |
| `test_sflat_group.cpp` | `test_group_match` | Metadata masks, lane selection, byte offsets, sentinels, and overflow bits. |

The interface tests also run against `std::unordered_map`.
The churn test compares all final entries with a reference map.
The group test compares each result with scalar expectations.
It checks all 256 byte values across 2,000 metadata groups.

## Design differences

bflat implements a map, but no set.
Thus, set tests do not apply.
bflat has no local bucket iterators or adjustable maximum load factor.
The port excludes those interface checks.
Existing bflat tests cover its fixed load limit and reserve rules.

The seed modes, tombstones, and copy-slack settings are specific to sflat.
bflat uses its own reseed rules and related regression tests.
The port keeps the shared content checks and excludes those implementation-specific checks.
It does not require copy assignment to preserve element addresses.

The structured-key test checks public operations instead of sflat mixer internals.
The group test uses aligned bflat groups with 15 slots.
It does not use unaligned loads, which bflat does not support for groups.
The churn test reserves 12.5% headroom before insertion.
This reserve setting permits bflat's anti-drift policy while it keeps a stable capacity.

The port excludes machine-specific time limits and iteration-order differences caused by seeds.
The tests check results, not a fixed operation-time threshold.

## Regression fixes

The tests identified three missing allocator constructor forms.
They also identified a generic-emplace conversion failure and incorrect const-pair handling.
The implementation now supports these cases.

An alias test identified a heap-use-after-free during growth.
The slow insertion path now saves the new entry before migration can move or release its source.
It first checks the old table for duplicates.
The final transfer uses a copy if the move can throw and a copy is available.
The reseed case combines a sparse string hash with a stored value reference.
The direct insertion fast path does not use this temporary storage.
Slow insertion can require an additional key copy.
Exact lookup checks for duplicates before temporary construction in avalanching-reseed mode.
A duplicate does not consume the supplied key or value.
The capacity, probing, and anti-drift policies do not change.

## Procedure

Run the complete test suite:

```sh
BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh
```

The added cases run in 21 configurations.
These configurations include default, incremental migration, scalar, ASan, and UBSan builds.
The sanitizer builds use allocator storage to expose released-slot access.
The existing tests also check mmap migration and page release.

Run the existing Ankerl corpus replay separately:

```sh
bash tests/ankerl/run.sh
```

## Results

The runner includes 43 configurations on x86-64 with Boost available.
This count includes 22 existing configurations and 21 added configurations.
The [result list](../validation/sflat-test-results.txt) records completed validation.

The full suite passed 43 configurations.
The final exact-lookup change was followed by nine related rechecks.
Review rechecks select the guess and hashreseed cases.
Both default Ankerl replay builds also passed.
