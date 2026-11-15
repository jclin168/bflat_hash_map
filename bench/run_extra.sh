#!/bin/bash
# Find at equal load (n = boost's max load for 2^20 groups: both at 0.875)
# and at 50M; bflat with and without THP hints. taskset -c 1, interleaved.
cd "$(dirname "$0")/.."
R=${R:-3}
OUT=${OUT:-results/extra_raw.txt}
: > $OUT
for r in $(seq 1 $R); do
  for n in 13762559 50000000; do
    for v in bflat bflat_nothp boost sflat; do
      echo "round=$r $(taskset -c 1 build/bench_$v find $n)" >> $OUT
    done
  done
done
echo ALLDONE >> $OUT
