#!/bin/bash
# Second NEON A/B after the byte_offset change: shrn vs boost emulation,
# plus reduced-hash table. Order alternates per round.
cd "$(dirname "$0")/.."
R=${R:-7}
OUT=${OUT:-results/ab2_raw.txt}
: > $OUT
for r in $(seq 1 $R); do
  if [ $((r % 2)) = 1 ]; then VS="bflat bflat_neonemu bflat_rhtable"; else VS="bflat_rhtable bflat_neonemu bflat"; fi
  for n in 1000000 10000000; do
    for v in $VS; do echo "round=$r $(taskset -c 1 build/bench_$v find $n)" >> $OUT; done
  done
done
echo ALLDONE >> $OUT
