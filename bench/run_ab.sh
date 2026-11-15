#!/bin/bash
# SIMD / hot-path A/B of bflat variants. taskset -c 1, interleaved, R rounds.
# Output: results/ab_raw.txt
cd "$(dirname "$0")/.."
R=${R:-5}
OUT=${OUT:-results/ab_raw.txt}
: > $OUT
for r in $(seq 1 $R); do
  for n in 1000000 10000000; do
    for v in bflat bflat_neonemu bflat_twoload bflat_pf0 bflat_pf2 bflat_rhtable; do
      echo "round=$r $(taskset -c 1 build/bench_$v find $n)" >> $OUT
    done
  done
done
echo ALLDONE >> $OUT
