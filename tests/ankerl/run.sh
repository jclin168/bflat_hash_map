#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs
tar -xzf ankerl/corpus.tar.gz -C ankerl
flags=(-std=c++17 -I../include -Iankerl/shim -Iankerl/include)
g++ "${flags[@]}" -O2 ankerl/replay.cpp -o ankerl/replay > logs/ankerl-build.txt 2>&1
./ankerl/replay > logs/ankerl-replay.txt 2>&1
g++ "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DBFLAT_USE_MMAP=0 -DBFLAT_GROW_AT_ONCE_BELOW=0 -DBFLAT_MIGRATE_GROUPS=1 ankerl/replay.cpp -o ankerl/replay-sanitized > logs/ankerl-sanitized-build.txt 2>&1
ASAN_OPTIONS=detect_leaks=0 ./ankerl/replay-sanitized > logs/ankerl-sanitized.txt 2>&1
