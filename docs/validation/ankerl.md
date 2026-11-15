# Ankerl corpus validation

## Source

The replay uses `martinus/unordered_dense` commit `7c58a8b7a48e790fe72f6508eb3b46317cbba9d5`.
The `fuzz_insert_erase` workload has 117 corpus files.
The `fuzz_string` workload has 246 corpus files.
The source files, provider, input bytes, dependencies, and licenses are in `tests/ankerl`.
Original upstream files keep their original text.

## Method

The adapter keeps operation order, hash functions, and all reference-container assertions.
A reference pass measures the peak live population.
Each input then runs in three reserve modes:

- No reserve request.
- Exact reserve for the peak population.
- Reserve for the peak population plus 12.5% headroom.

An assertion failure identifies the corpus file and reserve mode.

## Results

All 1,089 cases passed in the release configuration.
All 1,089 cases also passed with ASan and UBSan.
The sanitizer configuration disables mmap and forces incremental migration.
Each migration step moves one group.

These results cover the two named corpus workloads.
They do not cover continuous fuzzing or the full upstream test suite.
The hash-marker test also checks void, true, false, and absent markers.
Recorded unit validation passed 12 configurations.

## Procedure

Run these commands from the repository root:

```sh
bash tests/ankerl/run.sh
BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh
```

The adjacent logs record the measured version and results.
