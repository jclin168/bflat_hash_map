# Ankerl corpus validation

The replay uses `martinus/unordered_dense` commit
`7c58a8b7a48e790fe72f6508eb3b46317cbba9d5`:

- `fuzz_insert_erase`: 117 corpus files.
- `fuzz_string`: 246 corpus files.

Original workload sources, the provider, corpus bytes, dependencies and licenses
are included in `tests/ankerl`. The adapter retains operation order, hash
functions and every reference-container assertion. A reference pass determines
peak population; each input is then replayed without reserve, with exact peak
reserve, and with 12.5% headroom. Assertion failures report the corpus file and
reserve mode.

All 1,089 cases passed in both release and ASan/UBSan configurations. The latter
disables mmap and forces incremental migration at one group per step. These are
two corpus workloads, not continuous coverage-guided fuzzing or the complete
unordered_dense test suite.

The hash-marker regression covers `is_avalanching = void`, true/false value
markers, and unmarked hashes. The recorded unit validation passed 12 configurations.

Run from the repository root:

```sh
bash tests/ankerl/run.sh
BOOST_INCLUDE_DIR=/path/to/boost ./tests/run_tests.sh
```

The adjacent release, sanitizer and unit logs record the measured version.
