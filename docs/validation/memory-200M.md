# Memory use at 200M entries

## Method

The test used `uint64_t` keys and values.
It generated unique keys during insertion.
It did not store a separate input array.
Each configuration used one fresh process on CPU 0.
The process inserted 200M entries and completed pending migration.
It then completed 20M churn pairs and completed migration again.

Each pair inserted a new key before it erased a different original key.
The live population stayed at 200M entries after each pair.
The test checked insertion and erasure counts, final size, and samples of key-value pairs.
Five configurations completed all checks.
One additional configuration reached the RSS stop limit.

## Environment

| Item | Value |
|---|---|
| Processor | Xeon Platinum 8573C |
| Compiler | GCC 14.2 |
| Options | `-std=c++17 -O3 -DNDEBUG -march=native` |
| Boost | 1.92 |
| sflat | Pair-emplace optimization, `SFLAT_PROCESS_SEED=1` |
| Memory limit | 8 GiB cgroup, no swap |
| RSS stop limit | 7 GiB |
| bflat header SHA256 | `84c0d8a82f094b41f726b47053a82190c5932a58c59e24fdec1d9c87783ca16d` |

The measured bflat version uses arbitrary group counts and tiered growth.
The caller selects reserve headroom.
The measurements were made before the exception-safety review fixes.

## Memory results

All values use GiB, where 1 GiB equals 2^30 bytes.
RSS comes from `smaps_rollup`.
VM comes from `/proc/status`.
The values include process baseline memory.
Peak values combine process records and external samples.
RSS and VM peaks can occur at different times.

| Reserve | Container | Fill RSS | Fill VM | Final RSS | Final VM | Peak RSS | Peak VM |
|---|---|---:|---:|---:|---:|---:|---:|
| 200M | Boost | 4.002 | 4.006 | 4.002 | 4.006 | 4.002 | 4.006 |
| 200M | sflat | 3.648 | 3.652 | 3.648 | 3.652 | 3.648 | 3.654 |
| 200M | bflat | 3.637 | 3.641 | 4.545 | 4.549 | 4.775 | 8.186 |
| 225M | sflat | 4.105 | 4.109 | 4.105 | 4.109 | 4.105 | 4.111 |
| 225M | bflat | 4.090 | 4.094 | 4.090 | 4.094 | 4.090 | 4.096 |

Fill values apply after initial insertion and migration.
Final values apply after churn and migration.
VM measures reserved address space, not physical memory use.

## Capacity results

| Reserve | Container | Fill capacity | Final capacity | Final load factor |
|---|---|---:|---:|---:|
| 200M | Boost | 251,658,239 | 251,658,239 | 0.794729 |
| 200M | sflat | 228,571,456 | 228,571,456 | 0.875000 |
| 200M | bflat | 228,571,439 | 285,714,299 | 0.700000 |
| 225M | sflat | 257,142,880 | 257,142,880 | 0.777778 |
| 225M | bflat | 257,142,869 | 257,142,869 | 0.777778 |

Boost with reserve 225M selected a capacity of 503,316,479 slots.
Its process reached the RSS stop limit after at least 3,961,856 confirmed insertions.
The observed peaks were 7.004 GiB RSS and 8.006 GiB VM.
The monitor stopped this process before completion.
This result is not an out-of-memory failure or a maximum-capacity measurement.

## Interpretation

bflat with 12.5% headroom kept the same capacity during the measured churn.
Exact reserve caused capacity growth during churn.
Headroom reduced final memory and peak memory in this case.
It does not prevent all future anti-drift rehashes.

The study measured memory, not operation time or latency.
Each configuration ran once.
Different containers round reserve requests to different capacity levels.
Do not apply the same headroom percentage without a capacity check.
