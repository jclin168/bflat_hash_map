#!/bin/bash
# bflat vs boost vs sflat main. taskset -c 1, maps interleaved, R rounds.
# Output: results/main_raw.txt
cd "$(dirname "$0")/.."
R=${R:-3}
SIZES=${SIZES:-"1000000 10000000 50000000"}
OUT=${OUT:-results/main_raw.txt}
: > $OUT
for r in $(seq 1 $R); do
  for n in $SIZES; do
    for v in bflat boost sflat; do
      echo "round=$r $(taskset -c 1 build/bench_$v grow $n)" >> $OUT
      echo "round=$r $(taskset -c 1 build/bench_$v ops $n)" >> $OUT
    done
  done
done
echo ALLDONE >> $OUT
