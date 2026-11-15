#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build results
OUT=${OUT:-results/tests.txt}
mkdir -p "$(dirname "$OUT")"
: > "$OUT"
CXX=${CXX:-g++}
BASE=(-std=c++17 -O2 -Wall -Wextra -Iinclude)
run() {
 local name=$1 source=$2; shift 2
 local -a options=()
 while [[ $# -gt 0 && $1 != -- ]]; do options+=("$1"); shift; done
 if [[ $# -gt 0 ]]; then shift; fi
 "$CXX" "${BASE[@]}" "${options[@]}" "$source" -o "build/t_$name" >> "$OUT" 2>&1
 ASAN_OPTIONS=detect_leaks=0 "build/t_$name" "$@" >> "$OUT" 2>&1
 echo "[$name] PASS" | tee -a "$OUT"
}
run avalanching tests/test_avalanching.cpp
if [[ -n ${BOOST_INCLUDE_DIR:-} ]]; then
 run avalanching_boost tests/test_avalanching.cpp "-I$BOOST_INCLUDE_DIR"
fi
run api tests/test_api.cpp
run api_tiers tests/test_api.cpp '-DBFLAT_GROW_2X_BELOW=(64ULL<<10)' '-DBFLAT_GROW_1_5X_BELOW=(256ULL<<10)' '-DBFLAT_GROW_1_25X_BELOW=(1ULL<<20)'
run api_incremental tests/test_api.cpp -DBFLAT_GROW_AT_ONCE_BELOW=0
run diff tests/test_diff.cpp -- 101 1
run diff_incremental tests/test_diff.cpp -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=1 -- 102 1
run diff_scalar tests/test_diff.cpp -DBFLAT_NO_SIMD=1 -DBFLAT_GROW_AT_ONCE_BELOW=0 -- 105 0.3
run asan_api tests/test_api.cpp -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DBFLAT_USE_MMAP=0
run asan_diff tests/test_diff.cpp -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DBFLAT_USE_MMAP=0 -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=3 -- 107 0.25
run review tests/test_review.cpp -- 1 1
run review_tiers tests/test_review.cpp -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=2 '-DBFLAT_GROW_2X_BELOW=(64ULL<<10)' '-DBFLAT_GROW_1_5X_BELOW=(256ULL<<10)' '-DBFLAT_GROW_1_25X_BELOW=(1ULL<<20)' -- 2 0.5
run review_scalar tests/test_review.cpp -DBFLAT_NO_SIMD=1 -DBFLAT_GROW_AT_ONCE_BELOW=0 -- 3 0.3
# The mmap / page release path under the sanitizers (the asan_* runs above use BFLAT_USE_MMAP=0).
run asan_review tests/test_review.cpp -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=3 '-DBFLAT_GROW_2X_BELOW=(4ULL<<20)' '-DBFLAT_GROW_1_5X_BELOW=(8ULL<<20)' '-DBFLAT_GROW_1_25X_BELOW=(16ULL<<20)' -- 4 0.25
run asan_diff_mmap tests/test_diff.cpp -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=5 -- 109 0.25
if [[ $(uname -m) == x86_64 ]]; then
 run sse2_diff tests/test_diff.cpp -msse2 -mno-avx -DBFLAT_GROW_AT_ONCE_BELOW=0 -- 108 0.1
fi
if [[ -n ${BOOST_INCLUDE_DIR:-} ]]; then
 run tiered_policy tests/test_tiered_policy.cpp "-I$BOOST_INCLUDE_DIR"
else
 echo '[tiered_policy] SKIP: set BOOST_INCLUDE_DIR to Boost 1.92 headers' | tee -a "$OUT"
fi
echo ALLDONE | tee -a "$OUT"
