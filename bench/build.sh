#!/bin/bash
# Builds the benchmark binaries into build/. Same flags for all maps.
set -e
cd "$(dirname "$0")/.."
TP=../thirdparty
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O3 -mcpu=native -DNDEBUG -I$TP/boost-1.92.0 -Iinclude -I$TP/sflat_main_aaded6f/include"
mkdir -p build
$CXX $FLAGS -DMAP_BOOST -DMAP_NAME='"boost"' bench/bench.cpp -o build/bench_boost &
$CXX $FLAGS -DMAP_SFLAT -DMAP_NAME='"sflat"' bench/bench.cpp -o build/bench_sflat &
$CXX $FLAGS -DMAP_NAME='"bflat"' bench/bench.cpp -o build/bench_bflat &
wait
# SIMD / hot-path A/B variants of bflat (each differs from bflat in one macro).
build_var() { $CXX $FLAGS -DMAP_NAME="\"$1\"" $2 bench/bench.cpp -o build/bench_$1; }
build_var bflat_neonemu "-DBFLAT_NEON_SHRN=0" &
build_var bflat_twoload "-DBFLAT_INSERT_ONE_LOAD=0" &
wait
build_var bflat_pf0 "-DBFLAT_PREFETCH=0" &
build_var bflat_pf2 "-DBFLAT_PREFETCH=2" &
wait
build_var bflat_rharith "-DBFLAT_RH_TABLE=0" &
build_var bflat_nothp "-DBFLAT_USE_THP=0" &
wait
ls build/bench_*
