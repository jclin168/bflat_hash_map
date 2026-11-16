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

## Iteration

An iterator holds the map, its element, its group's metadata and first slot,
and the mask of the lanes after its own (five words). An increment reads the
group's metadata again, so an erased element is never visited, and takes the
lowest occupied lane of the mask; within a group, and into a following
non-empty group of the same table, it needs neither the map nor a division
by 15. Empty groups, the end of a table and the step into the old table of a
migration go through the map. The previous iterator kept a unified slot
index and recomputed group and lane on every increment (a multiply for the
division, a variable shift and a bit scan in one dependency chain).

## Reseed

Iteration visits a table in home order and the home mapping is monotonic,
so inserting the iteration order of a table into a smaller table with the
same hash multiplier sends the keys sorted by home: they fill the start of
the table, every insert probes the full groups before it, and the loop takes
quadratic time (boost has the same behaviour). An insert that probes
`BFLAT_RESEED_STEPS` (64) groups past its home group marks a pile-up; random
keys probe at most about 20 at the load limit of a 10M table. The next
insert that reaches the slow path rebuilds the table, at the same capacity
when the elements fit, with the next of 16 checked multipliers. A table
with an avalanching hash (used unmixed) is then mixed with mulx too, but its
inline lookups still probe with the unmixed hash, so that no table pays a
test of the multiplier (a branch on it before the probe cost 2.5-3% in a
10M insert loop, a multiply by a per-table odd constant 2.5%): a key found
that way is the key, a miss is checked again with the mixed hash in a
noinline path (behind a test of `avail_`, which is 0 in such a table and
during a migration, in place of the test of the old table), and every
insert takes the slow path. A reseeded table of such
a hash is slower (10M keys: find hit 2.5x, miss 2.6x, insert 1.4x,
erase+insert 1.1x) but linear; `clear()` returns a table to the initial
multiplier. This is off by default (`BFLAT_RESEED_AVALANCHING=0`: an
avalanching hash keeps the pile-up, as before); build with
`-DBFLAT_RESEED_AVALANCHING=1` to enable it. Copying a 10M table in iteration
order into an empty one takes 1.0 s with an avalanching hash (0.67 s mixed,
31.7 s before); a xor seed instead of mulx does not help (38 s): it maps the
run of keys with the same top bits to another such run. A table rebuilds at most once per
doubling of its size, so a hash that no multiplier helps costs O(1)
rebuilds per element. Tables start with boost's multiplier, so capacities and iteration order are unchanged unless a
pile-up is seen. A rebuild follows the growth rules (at once below
`BFLAT_GROW_AT_ONCE_BELOW`, else incremental); during an incremental one the
old table keeps its multiplier (a lookup there hashes the key again), new
keys go to the new table, and the new table is written in the new hash
order rather than near the migration front.

## Exceptions and allocators

A migration step moves each element out of the old table as soon as it is
moved into the new one, so a throwing hash or move constructor leaves every
element in exactly one table; the element whose move threw is dropped (its
key may already be moved from) and the exception propagates from the insert.
`find`, `contains` and `count` are not `noexcept`: they call the hash and the
predicate. Copy assignment with a propagating allocator frees the old
storage with the old allocator. `insert(first, last)` reserves only when the
range does not fit the current load limit; it never shrinks the table.
`max_size()` is the load limit of the largest table, so `reserve(max_size())`
is not a length error.

## Tests and history

Run `BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh`.
Tests include low thresholds to exercise all four tiers, differential tests
under growth and weak hashes, arbitrary-group probe coverage, scalar/SSE2,
and ASan/UBSan (with and without mmap). `test_review.cpp` covers allocator
propagation, throwing hash / copy / move and allocation failure under
migration, size limits, range insert and iterator edge cases. `test_tiered_policy.cpp` uses Boost as a functional reference;
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
