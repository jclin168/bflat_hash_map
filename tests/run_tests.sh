#!/bin/bash
# Builds and runs all test configurations. Output: results/tests.txt
cd "$(dirname "$0")/.."
mkdir -p build
OUT=${OUT:-results/tests.txt}
: > $OUT
run() { local name=$1; shift; local bin=build/t_$name; local cmd="$1"; shift
  if ! eval "$cmd -o $bin" >> $OUT 2>&1; then echo "[$name] BUILD FAIL" >> $OUT; return; fi
  local pre=""; case $name in x86*) pre="qemu-x86_64 -cpu max";; esac
  echo "[$name] $*" >> $OUT; $pre "$bin" "$@" >> $OUT 2>&1; echo "[$name] rc=$?" >> $OUT; }
B="g++ -std=c++17 -O2 -g -Wall -Wextra -Iinclude"
SAN="clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Iinclude"
X="x86_64-linux-gnu-g++ -std=c++17 -O2 -static -Iinclude"
run api        "$B tests/test_api.cpp"
run api_tiers  "$B -DBFLAT_GROW_2X_BELOW='(1ULL<<20)' -DBFLAT_GROW_1_5X_BELOW='(8ULL<<20)' tests/test_api.cpp"
run diff       "$B tests/test_diff.cpp" 101 1
run diff_incr  "$B -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=1 tests/test_diff.cpp" 102 1
run diff_tiers "$B -DBFLAT_GROW_2X_BELOW='(64ULL<<10)' -DBFLAT_GROW_1_5X_BELOW='(1ULL<<20)' -DBFLAT_GROW_AT_ONCE_BELOW='(16ULL<<10)' tests/test_diff.cpp" 103 1
run diff_neonemu "$B -DBFLAT_NEON_SHRN=0 -DBFLAT_GROW_AT_ONCE_BELOW=0 tests/test_diff.cpp" 104 0.3
run diff_nosimd  "$B -DBFLAT_NO_SIMD=1 -DBFLAT_RH_TABLE=0 -DBFLAT_INSERT_ONE_LOAD=0 -DBFLAT_PREFETCH=2 -DBFLAT_GROW_AT_ONCE_BELOW=0 tests/test_diff.cpp" 105 0.3
run asan_diff_incr "$SAN -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=1 tests/test_diff.cpp" 106 0.25
run asan_diff_nommap "$SAN -DBFLAT_USE_MMAP=0 -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=3 tests/test_diff.cpp" 107 0.25
run asan_api   "$SAN -DBFLAT_USE_MMAP=0 tests/test_api.cpp"
run x86_sse2_api  "$X tests/test_api.cpp"
run x86_sse2_diff "$X -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=1 tests/test_diff.cpp" 108 0.05
run x86_avx2_diff "$X -mavx2 -DBFLAT_GROW_AT_ONCE_BELOW=0 tests/test_diff.cpp" 109 0.05
echo ALLDONE >> $OUT
