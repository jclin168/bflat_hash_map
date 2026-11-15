# bflat: arbitrary group counts and tiered growth

Group counts no longer round to powers of two. `reserve(n)` rounds only to
15-slot groups (minimum two), including the last sentinel slot. The initial
load limit is exactly floor(capacity * 7/8), with the existing full-use rule
for the smallest table. Integer arithmetic avoids large-n float rounding.

## Default growth policy

Thresholds count the current element and metadata capacity in bytes:

| Current table bytes | Growth | Macro threshold |
|---|---:|---|
| below 256 MiB | 2x | BFLAT_GROW_2X_BELOW |
| 256 MiB to below 2 GiB | 1.5x | BFLAT_GROW_1_5X_BELOW |
| 2 GiB to below 16 GiB | 1.25x | BFLAT_GROW_1_25X_BELOW |
| 16 GiB and above | 1.125x | — |

Growth adds ceil(groups/divisor) groups (divisors 1, 2, 4, 8), and must also
fit the requested entries and the size/61 anti-drift slack. Thresholds must
be ordered. The sentinel, SIMD group format, two-stage find/insert path,
and Boost-style overflow anti-drift decrement are retained. A drift-only
rehash stays at the same capacity when the requested slack still fits;
otherwise it uses the selected growth tier. There is no extra drift budget
or overflow-only refresh.

## Arbitrary group probing

Home group is the high half of hash * groups (monotonic range reduction).
The triangular probe prefix is followed by a linear tail if necessary;
it terminates after at most 2*groups visits and covers every group. The
Boost bitmask and its power-of-two-only coverage guarantee cannot be used
for arbitrary group counts.

mmap, THP hints, incremental migration, and release of migrated pages are
retained. Reducing a growth factor reduces the next allocation size but
increases total migration work. Virtual space may still contain both old
and new mappings even when old resident pages have been released.

For example, a 64 GiB table grows to about 72 GiB in the last tier rather
than 128 GiB. The two-table virtual reservation can still approach 136 GiB.
Constant-size growth increments are not the default: repeatedly rehashing
an ever-larger table after each fixed increment can make total work quadratic.
Every multiplicative policy remains geometric in the number of resizes;
table memory is proportional to live entry count, not exponential in it.

## Tests and history

Run `BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh`.
Tests include low thresholds to exercise all four tiers, differential tests
under growth and weak hashes, arbitrary-group probe coverage, scalar/SSE2,
and ASan/UBSan. `test_tiered_policy.cpp` uses Boost as a functional reference;
it does not assert equal capacities now that capacity policies differ.

The previous power-of-two version is documented in docs/boost-policy.md;
the original prototype is in docs/prototype-7c24911.md. Earlier performance
and memory measurements describe those versions and have not been rerun
for this tiered variant. Boost Software License terms are in LICENSE.

## Ankerl hash marker compatibility

An avalanching hash can use either `using is_avalanching = void` (as in
Ankerl) or a type with a boolean `value`. Both true markers now suppress
remixing; a false marker or absent marker retains mixing. The fix does not
change capacity, probing, insertion, drift maintenance or the selected
workload reserve headroom. `tests/test_avalanching.cpp` checks both forms,
false/unmarked forms and actual insert/find/erase.

The selected churn policy is `reserve(N + N/8 + (N%8 != 0))`; this remains
an application decision and is not applied to the reserve API automatically.
