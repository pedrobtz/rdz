# rdz work in progress

Updated: 2026-06-28

## Goal

Build a low-latency R object serializer with a stable speed-first format and an
optional balanced format that reduces file size only where the transform has a
defensible performance tradeoff.

## Completed

- The `speed` preset retains the original `RDZFILE1` direct-storage format.
- The `balanced` preset uses `RDZFILE2` and currently provides:
  - exact low-cardinality numeric dictionaries with bit-packed indexes and
    adaptive LZ4 blocks for compressible index streams;
  - exact constant-delta numeric sequence encoding for objects such as `Date`
    and `POSIXct`;
  - exact XOR-delta and byte-transpose encoding for high-cardinality doubles,
    selected only when its first block and complete payload save at least 12.5%;
  - two-bit logical encoding, including `NA`;
  - frame-of-reference integer encoding for ranges requiring at most eight
    bits, with an `NA` code only when needed;
  - adaptive dictionary or flat layouts for character vectors;
  - independent 64 KiB LZ4 blocks for compressible flat string payloads;
  - bounded positional byte transposition and prefix/suffix coding for
    structured high-cardinality strings, selected from one representative block;
  - run-length encoded varints for flat string lengths when smaller;
  - 64 KiB buffered output for 1-, 2-, and 4-byte string dictionary indexes.
- Unsupported and compact ALTREP objects continue to use R serialization.
- Native decoding validates encoding tags, indexes, padding, range overflow,
  truncated input, and malformed metadata.
- The fst benchmark asserts that rdz selected its native codec, explicitly
  configures fst's thread count, and records both requested and effective
  threads.

## Latest performance results

The fst comparison used R 4.5.2, fst 0.9.8, fstcore 0.10.0, one explicitly
configured fst thread, five seeded iterations, and strict `identical()`
validation. The installed macOS fst binary resolves both one and all requested
threads to one effective thread, so multithreaded fst remains unmeasured here.

| case | write speedup | read speedup | balanced/fst size |
|---|---:|---:|---:|
| mixed | 2.12x | 2.84x | 0.63x |
| repeated | 1.54x | 2.78x | 0.32x |
| temporal | 3.09x | 1.98x | 0.30x |
| unique strings | 1.19x | 1.01x | 0.50x |
| wide numeric | 3.12x | 1.66x | 1.00x |

- rdz won all five writes and reads; unique-string reads were effectively
  tied at 47.75 ms versus 48.35 ms.
- Geometric-mean speedups were 2.07x for writes and 1.92x for reads.
- Balanced files were 0.50x the size of default fst files geometrically.

Focused results:

- One million repeated strings: buffering reduced rdz write time from
  58.4 ms to 13.0 ms; fst took 23.5 ms. The rdz file remained 1,001,973
  bytes versus 2,411,875 bytes for fst.
- One million logicals: 4,000,026 bytes became 250,027 bytes. Focused write
  time changed from 1.60 to 1.36 ms and read time from 0.81 to 0.93 ms.
- One million integers cycling through `1:100` and `NA`: 4,000,026 bytes
  became 875,033 bytes. A focused one-column frame read in 1.60 ms versus
  2.77 ms for fst; packing writes took 3.33 ms versus 2.71 ms for fst.
- One million numerics cycling through `0`, `1`, and `NA` became 1,099 bytes.
  Balanced rdz wrote the vector in 2.46 ms and read it in 2.18 ms; default
  qs2 took 2.90 ms and 3.45 ms. Sampled small dictionaries pack during their
  exact validation scan; larger dictionaries capture indexes during discovery
  and avoid a second full hash-probe pass.
- A random 64-value numeric vector retained raw packed indexes. Balanced rdz
  wrote and read it in 6.50 ms and 3.08 ms versus 32.97 ms and 12.81 ms for
  default qs2.
- The full repeated frame is 2.03 MiB in balanced mode versus 6.40 MiB for fst.
- XOR-delta byte transposition reduced one million uniform random doubles to
  0.61x direct size, a noisy trend to 0.59x, and a random walk to 0.79x. Normal
  data with only a marginal gain and random bit patterns remain direct.
- The mixed atomic-list benchmark is 17.40 MiB in balanced mode versus 30.52
  MiB in speed mode and 13.87 MiB for default qs2. Balanced rdz wrote and
  read it in 93.1 ms and 28.9 ms versus 251.5 ms and 47.7 ms for default qs2.
- One million structured unique strings use 0.084 MiB in balanced mode versus
  4.85 MiB before this transform, 0.362 MiB for qs2, and 0.400 MiB for qdata.
  Balanced rdz wrote in 120.4 ms and read in 92.9 ms versus 94.8/131.7 ms
  for qs2 and 58.5/101.9 ms for qdata.
- The 500,000-row unique-string frame is 4.27 MiB in balanced mode versus 17.64
  MiB in speed mode and 8.47 MiB for fst. Balanced reads effectively tie fst;
  speed-mode rdz remains the fastest writer.

Detailed results and the reproducible script are in:

- `inst/benchmarks/data-frames-fst-results.md`
- `inst/benchmarks/data-frames-fst.R`
- `inst/benchmarks/many-objects.R`

## Verification status

- All testthat tests pass.
- Integer tests cover every selected width from one through eight bits, missing
  values, signed ranges, `INT_MAX`, factors, direct fallback, invalid padding,
  invalid flags, and malformed metadata.
- Numeric tests cover compressed and raw packed dictionaries, every selected
  bit width, XOR-delta raw-bit preservation, direct fallback, block boundaries,
  malformed LZ4 blocks, invalid indexes, and truncation.
- String tests cover dictionary indexes of one, two, and four bytes.
- Flat-string tests cover LZ4, transpose, prefix/suffix, and raw selection;
  mixed encodings; direct and run-length encoded length metadata; value-block
  boundaries; invalid metadata; malformed block sizes; decompression failure;
  and truncation.
- A byte-for-byte comparison against commit `9e96fa8` confirms that the speed
  preset is unchanged.
- The current reader round-trips an uncompressed numeric dictionary written by
  commit `9e96fa8`, preserving existing `RDZFILE2` compatibility.
- The full fst matrix and a reduced 27-object smoke matrix pass. The full-size
  27-object run was terminated by the local environment before completion.
- The local source-tarball `R CMD check --as-cran --no-manual` run, with remote
  incoming checks disabled, has no errors, warnings, or notes.
- `git diff --check` passes.

## Current working-tree state

The numeric XOR transform and fst thread-control fix are committed through
`8e55d6f`. The string transposition and prefix/suffix transforms, tests,
documentation, and updated benchmark results are not committed. Review the
complete diff before committing; do not discard the uncommitted files.

Key implementation files:

- `src/rdz.c`
- `src/numeric_codec.c` and `src/numeric_codec.h`
- `src/logical_codec.c` and `src/logical_codec.h`
- `src/integer_codec.c` and `src/integer_codec.h`
- `src/lz4.c` and `src/lz4.h` (vendored LZ4 1.10.0)
- `tests/testthat/test-roundtrip.R`

## What to do next

1. Run `R CMD check --as-cran --no-manual` on the final diff and verify the
   vendored LZ4 build on Linux and Windows CI.
2. Run the fst comparison on a Linux host with OpenMP-enabled fst and retain
   both explicit one-thread and all-thread results.
3. Run the complete 27-object matrix in an environment with enough memory and
   retain its CSV. The reduced smoke matrix already passes all object types.
4. Add representative real-world string corpora and distributions whose later
   blocks differ from the selector's first block, then tune the bounded selector
   only if those measurements expose a systematic miss.

Do not add rdz multithreading yet. First establish the comparison against an
OpenMP-enabled fst build; explicit SIMD should follow profiling rather than be
assumed to help.

## Running tests and benchmarks

Install the current checkout into the isolated library first:

```sh
R CMD INSTALL --preclean --clean --library=/tmp/rdz-r-lib .
```

Run the test suite:

```sh
R_LIBS_USER=/tmp/rdz-r-lib Rscript -e \
  'testthat::test_local(reporter="summary")'
```

Run the main comparison against `qs2` and `qdata`:

```sh
R_LIBS_USER=/tmp/rdz-r-lib \
  Rscript inst/benchmarks/benchmark.R
```

Run the 27-object correctness and performance matrix and retain its CSV:

```sh
env R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_OUTPUT=/tmp/rdz-many-objects.csv \
  Rscript inst/benchmarks/many-objects.R
```

Run the data-frame comparison against `fst` and retain its CSV:

```sh
env R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_OUTPUT=/tmp/rdz-fst.csv \
  Rscript inst/benchmarks/data-frames-fst.R
```

Use reduced workloads for a quick smoke run before a full benchmark:

```sh
env R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_N=100000 \
  RDZ_BENCH_ITERATIONS=2 \
  Rscript inst/benchmarks/benchmark.R

env R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_N=100000 \
  RDZ_BENCH_ITERATIONS=2 \
  RDZ_BENCH_OUTPUT=/tmp/rdz-many-smoke.csv \
  Rscript inst/benchmarks/many-objects.R

env R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_FST_N=100000 \
  RDZ_BENCH_ITERATIONS=2 \
  RDZ_BENCH_OUTPUT=/tmp/rdz-fst-smoke.csv \
  Rscript inst/benchmarks/data-frames-fst.R
```

Benchmark environment variables:

| variable | used by | default | purpose |
|---|---|---:|---|
| `RDZ_BENCH_ITERATIONS` | all scripts | 5 | Timed iterations per operation. |
| `RDZ_BENCH_N` | qs2 and many-object scripts | 2,000,000 / 1,000,000 | Base workload size. |
| `RDZ_FST_N` | fst script | 1,000,000 | Base row count for fst cases. |
| `RDZ_FST_THREADS` | fst script | 1 | fst thread count; use 0 for all available threads. |
| `RDZ_BENCH_OUTPUT` | many-object and fst scripts | temporary CSV | CSV output path. |

The qs2 script prints results to the console. The other two scripts print the
final CSV path; set `RDZ_BENCH_OUTPUT` explicitly if the results need to be
kept.
