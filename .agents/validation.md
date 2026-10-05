# RDZ Validation and Benchmarking

## Validation layers

Use three complementary layers:

1. The R-free C harness (`tools/run-c-tests`, `tools/c-tests/`) and the libFuzzer
   targets (`tools/run-fuzz`, `fuzz/`) for records, lengths, blocks, checksums,
   container finalization, and malformed input. They compile without R, under
   ASan and UBSan, with warnings as errors, and the fuzz gate trusts a target only
   after its canary has crashed. `tools/run-mutation-check` disables each reader
   guard marked `/* GUARD: name */` in turn and requires its hostile file
   (`tools/c-tests/probe.c`) to get a different answer, so no marked guard is
   vacuous; a new guard against hostile input gets a marker and a case.
2. The Rust reference corpus (`tests/testthat/fixtures/rust/`): every fixture
   must read back `identical()` to its spec and report the Rust build's
   `rdz_info()`, and the C writer must reproduce the generic fixtures byte for
   byte. (The Savvy boundary tests of the Rust implementation are retired with
   it.)
3. R `testthat` integration tests as the authoritative public-API and semantic
   round-trip suite.

Run the existing project commands documented in [AGENTS.md](../AGENTS.md).
Format tests should be deterministic and quick enough for ordinary development.
Larger benchmarks and fuzz campaigns live under `tools/` and do not run as normal
correctness tests.

## Universal native round-trip matrix

Every supported value needs:

```r
path <- tempfile(fileext = ".rdz")
write_rdz(x, path)
expect_identical(read_rdz(path), x)
```

Cover:

- lengths zero and one;
- all missing and mixed missing/non-missing values;
- absent, empty, duplicate, and ordinary names where valid;
- constant, low-entropy, patterned, and high-entropy values;
- block-size minus one, exact block size, block-size plus one, and multiblock
  lengths;
- temporary-file cleanup after failures;
- stable native golden fixtures after each representation is frozen.

`expect_equal()` is insufficient for serialization semantics. Use
`expect_identical()` and add bit-level assertions where R's display or equality
collapses meaningful distinctions.

## Attribute preservation

Treat attributes as recursively serialized values. For ordinary native and
generic round trips, assert both the object and `attributes()` are identical.
Cover at least:

- absent, empty, ordinary, non-syntactic, and Unicode attribute names;
- `names`, `dim`, `dimnames`, `class`, `levels`, `row.names`, `tsp`, time-zone,
  units, and arbitrary user attributes;
- attributes on atomic vectors, lists, list elements, factors, and data frames;
- attribute values that are vectors, lists, classed values, and reference objects;
- multiple attributes and their ordering where R exposes it;
- unsupported attribute values nested inside an otherwise native-friendly root.

For the initial codec, attributes outside the owning native type's documented
allowlist must make automatic mode select one whole-root R payload and must make
strict-native mode fail. Assert that the original root is unchanged and that no
supported attributes were written while unsupported ones were dropped. When a
general native attribute map is introduced, add graph tests proving that sharing
and cycles crossing the object/attribute boundary survive.

The registered transient-metadata policy is the only exception. Each exception
needs a negative test showing that a different attribute name, owning class, or
value does not match it.

## Metadata-only and selective attribute reads

Use [metadata-access.md](metadata-access.md) as the completion contract. Instrument
the reader in tests so assertions cover bytes/blocks touched, not only returned R
values. Verify:

- container information requires only the fixed header/trailer and directory;
- native root type, length, class summary, and schema are available without leaf
  allocation;
- selecting `class`, `dim`, `levels`, `row.names`, or a custom supported
  attribute reads only its descriptor and referenced blocks;
- a large `names` attribute reads its own blocks but not the vector payload;
- data-frame schema inspection allocates no columns;
- missing and duplicate attribute requests have defined behavior;
- generic fallback is labeled synopsis-only and refuses exact attributes when
  `allow_full = FALSE`;
- `allow_full = TRUE` reports that it performed full deserialization;
- metadata inspection never invokes persistence hooks or package behavior;
- corrupt directories, offsets, references, checksums, and excessive metadata
  fail before payload or R-object allocation.

Add golden native and generic fixtures for the metadata API. Cross-OS CI must
produce identical logical inspection results.

## Whole-root fallback coverage

Automatic mode must round-trip every representative object handled by base R
serialization, even when no dedicated native codec exists. Add forced-generic and
automatic-mode tests for:

- closures with captured environments;
- environments with shared and cyclic references;
- pairlists, calls, expressions, formulas, and symbols;
- promises where they can be constructed and tested safely;
- S3 objects with unsupported classes or attributes;
- S4 and S7 objects when available in the test environment;
- unsupported matrices, arrays, complex/raw vectors, and ALTREP classes;
- weak references and external pointers under base R's documented limitations;
- a list mixing native-friendly vectors, a closure, and a shared environment.
- forced generic behavior when a serialization `refhook` is supplied, and a clear
  argument error for `refhook` with strict native mode.

External pointers used as values or unknown/semantic attributes must select the
whole-root generic codec in automatic mode and fail in strict native mode. Add a
separate native test for each registered transient external-pointer attribute.
For `data.table::.internal.selfref`, verify that the attribute is absent from the
native payload/restored object, `class()` remains
`c("data.table", "data.frame")`, `inherits(result, "data.table")` is true, all
columns and supported table metadata survive, and representative `data.table`
operations remain usable when the package is available. The test must guard
against accidental coercion to a plain data frame. Also test that a differently
named external-pointer attribute on the same object does not match the exception
and instead triggers whole-root fallback.

Use [sexp-coverage.md](sexp-coverage.md) as the exhaustive registry: every
constructible public type needs an automatic/forced-generic/strict-native
expectation, while retired, reserved, pseudo, and GC-only types need decoder
rejection tests where they can appear as corrupt tags.

Verify that automatic mode selects one generic payload for the complete root and
that strict native mode returns the distinguished unsupported-value error without
leaving a destination or temporary file. A nested unsupported object must never
be silently converted, removed, or serialized as an independent fallback island,
except for a registered transient attribute whose omission is part of the native
class contract.

`expect_identical()` remains authoritative for ordinary values. Reference objects
need semantic graph assertions: evaluate restored closures, inspect bodies and
formals, verify environment bindings/enclosures, and verify that repeated
references inside the restored root point to the same restored object. External
resources may require `refhook` or may retain only the behavior promised by base
R serialization; tests must document that limitation rather than claim resource
portability.

During development, use replaceable fixtures for the explicitly identified
R-serialization payload. Freeze them as compatibility fixtures only for the
0.1.0 release. Test that generic payload decoding preserves R serialization
version/source-encoding behavior and rejects corrupt block or container metadata
before returning an object.

## Type-specific correctness matrix

### Logical

- false, true, and missing states;
- long runs and alternating values;
- invalid reserved two-bit state on decode.

### Integer

- `NA_INTEGER`, minimum and maximum ordinary integers;
- sequential, negative, small-range, repeated, and random values;
- every retained physical transform and raw fallback.

### Double

- `NA_real_`, ordinary NaN, positive/negative infinity;
- positive and negative zero;
- subnormal values and unusual NaN payload bits;
- shuffled/compressed and raw blocks.

### Character

- `NA_character_`, empty strings, repeated strings, and unique strings;
- ASCII, UTF-8, Latin-1, bytes, and native encodings;
- mixed encodings in one vector;
- short and very long strings;
- dictionary and non-dictionary blocks;
- malformed lengths, offsets, encoding tags, and dictionary indices.

### Factor

- unordered and ordered factors;
- empty, unused, duplicated where constructible, and non-ASCII levels;
- missing codes;
- codes outside the level range in malformed files.

### List

- empty and heterogeneous supported lists;
- nested named and unnamed lists;
- boundary nesting depth;
- supported sharing/back-references and rejected cycles according to policy;
- unsupported children and attributes.

### Data frame

- zero rows, zero columns, and both zero;
- one and many columns;
- mixed supported columns;
- automatic, compact, integer, and character row names as supported;
- duplicate and empty column names where R permits them;
- malformed unequal column lengths and inconsistent row counts.

## Malformed-input and resource-limit tests

Construct invalid files directly with pure Rust helpers or small fixtures. Cover:

- bad magic, versions, codec IDs, tags, flags, and closing magic;
- truncated headers, block bodies, attributes, indexes, and footer;
- declared sizes larger than remaining input;
- integer overflow in lengths, offsets, and allocation calculations;
- overlapping, unordered, or out-of-range block spans;
- decompression bombs and decoded-size mismatches;
- checksum mismatches;
- excessive nesting, element counts, attributes, blocks, or total bytes;
- excessive object-directory entries, synopsis bytes, or metadata string sizes;
- trailing data according to the format's explicit policy.

The decoder must return an R error, never panic or abort R. Add fuzz targets for
the pure Rust container and codec once their APIs stabilize. Seed fuzzing with
every golden fixture and malformed regression case.

## Cross-OS compatibility tests

Treat [portability.md](portability.md) as a release gate. CI must read committed
native, generic-XDR, compressed, raw, and character-encoding fixtures on
Windows, macOS, and Linux. At least one workflow must exchange an artifact written
on one OS and read it on another.

Pure Rust tests must decode canonical little-endian bytes independently of host
layout, reject byte-swapped controls, exercise `u64`-to-host/R length overflow,
and prove scalar/SIMD transform equivalence. R tests must cover UTF-8 and
non-UTF-8/C locales where available, including native-source-to-UTF-8 conversion
and exact `CE_BYTES` preservation.

The generic codec must be inspected or fixture-tested as R version-3 XDR. A
native-word-order R stream is a format error even when writer and reader happen to
share an OS and CPU.

## Benchmark goals

Measure:

- end-to-end write latency and throughput;
- end-to-end read latency and throughput;
- encoded bytes and compression ratio;
- peak resident memory or a documented allocation proxy;
- CPU utilization and scaling by thread count;
- small-object latency separately from large-object throughput.

Benchmark the public R API, including format traversal, R allocation, checksums,
compression, and file IO. Microbenchmarks for transforms and compressors are
useful for diagnosis but cannot support user-facing performance claims alone.

### Phase 0A harness contract

`tools/bench-helpers.R` provides three views:

- `benchmark_serialization_matrix()` returns paired read/write latency, stored
  file size and ratios, stored-byte and `object.size()` throughput, and
  read/write `bench::mark()` allocation-proxy matrices plus long-form details;
- `benchmark_serialization_suite()` repeats that contract for named small,
  medium, and throughput-oriented sizes;
- `benchmark_read_matrix()` and `benchmark_write_matrix()` retain focused
  operation views for interactive diagnosis.

Every timed case is written and read once for identity validation, followed by a
recorded number of untimed warmups. Warm reads repeat the same prepared file.
Cold reads require `cache_mode = "cold_hook"`, a user-supplied hook invoked
outside each one-iteration timed expression, and a non-empty label describing
what the hook actually guarantees. Never call a fresh file or best-effort cache
perturbation "cold" without that contract.

Write timing is the steady overwrite of an existing validated path. Stored-byte
throughput divides file bytes by median time; object throughput divides
`utils::object.size()` by median time. `mem_alloc` is an R-allocation proxy, not
peak RSS, and must be labeled as such. Long-form CSV output is for tabular
analysis; the uncompressed RDS result preserves complete machine, package,
backend, parameter, and matrix metadata.

## Competitor matrix

Compare against currently pinned versions of:

- default gzip-compressed `saveRDS()`/`readRDS()` as the compatibility baseline;
- `saveRDS(..., compress = FALSE)`/`readRDS()` as the uncompressed base-R
  performance baseline;
- `qs2::qs_save()`/`qs_read()` for general serialization;
- `qs2::qd_save()`/`qd_read()` as the closest supported-subset competitor;
- `fst::write_fst()`/`read_fst()` for data frames.

Benchmark both strict native mode on supported common values and forced generic
mode on representative rare-object graphs. Compare forced generic mode primarily
with `qs2` and base R because `qdata` and `fst` intentionally do not provide the
same object coverage.

For vector-only `fst` comparisons, use a one-column data frame and label the
additional table semantics. For `fst`, distinguish complete reads/writes from
selective row or column access.

The default diagnostic matrix is single-threaded: set `fst::threads_fst(1)` and
pass `nthreads = 1` to qs2/qdata explicitly. Enable `validate_checksum = TRUE`
for qs2/qdata reads when comparing with RDZ's mandatory CRC validation. A
checksum-disabled package-default result may be recorded only as an additional,
clearly labeled mode. Restore process-global competitor settings after a helper
returns.

Record for every run:

- package versions and source/build options;
- R and Rust versions;
- CPU, RAM, operating system, and filesystem;
- thread count and compressor threading behavior;
- compression preset and checksum validation setting;
- input seed, shape, and value distribution;
- number of warmups and measured iterations.

Use equal thread budgets. Do not allow RDZ and a nested compressor to use more
threads than the competitor. Compare checksum-enabled with checksum-enabled when
possible and state any unavoidable difference.

Compression levels are not comparable numbers across formats. Report at least:

1. fastest practical mode;
2. default/balanced mode;
3. approximately matched encoded size;
4. compact mode where relevant.

Present throughput against encoded size as a Pareto frontier rather than choosing
one favorable setting.

## Benchmark datasets

### Logical

- uniform random states;
- mostly false, mostly true, and mostly missing;
- long runs and alternating values.

### Integer

- uniform random `i32` values;
- sequential and slowly changing values;
- small-cardinality repeated values;
- sparse missing values and all missing values.

### Double

- random full-precision doubles;
- rounded measurements;
- decimal-like currency, sensor, latitude/longitude, and percentage values for
  ALP;
- mixed decimal-like and high-precision row groups for ALP/ALP-RD selection;
- monotonic/time-series-like values;
- repeated values;
- sparse missing and exceptional values.
- dense mixtures of `NA_real_`, distinct NaN payloads, infinities, subnormals,
  positive zero, and negative zero to verify ALP exception-bit preservation;
- exact multiples and lengths around the 1,024-value ALP vector boundary and RDZ
  block boundary;

### Character

- short high-cardinality identifiers;
- low-cardinality categorical strings;
- repeated long strings;
- unique long strings;
- mixed lengths, missingness, and encodings.

### Data frame

- numeric-heavy, character-heavy, and mixed tables;
- many narrow columns and a few wide columns;
- factors with low and high level counts;
- realistic public datasets plus controlled synthetic cases.

Include sizes that expose fixed overhead, cache behavior, parallel thresholds,
and sustained IO: small, medium, and larger-than-cache inputs.

## Benchmark execution

Use deterministic seeds and generate data outside timed expressions. Reuse the
same input for every format. Separate write and read measurements, ensure writes
have completed, and document whether reads are warm-cache or cold-cache. Cold
cache claims require a reproducible platform-specific procedure; otherwise label
results as warm-cache.

Run enough iterations to report median and dispersion. Randomize format execution
order when practical. Validate every decoded result outside the timed expression
at least once per case.

`benchmark_read_matrix()` in `tools/bench-helpers.R` is the canonical read-matrix
entry point. It returns object types as rows, formats as columns, and median
`bench::mark()` read milliseconds as cells, with long-form measurements and skip
reasons attached as attributes. Do not coerce an unsupported object merely to
populate a cell: use `NA` and record the reason. `tools/benchmark.R` provides the
reproducible `Rscript` entry point.

Commit benchmark code and dataset generators under `tools/`, but do not commit
local benchmark output. Summarize material results in pull requests with the
environment and exact command needed to reproduce them.

## Performance acceptance

Correctness and compatibility are hard gates. A new transform or parallel path is
retained only when it provides a documented win for part of the benchmark matrix
without unacceptable regressions elsewhere. Always preserve a raw or simple
reference encoding for incompressible data and cross-check optimized decoders
against it.

For every investigated physical encoding, record the source, experimental setup,
semantic findings, dependency impact, and adopt/defer/reject decision in
[encoding-research.md](encoding-research.md). An adopted encoding must receive an
explicit format ID, malformed-input coverage, and a deterministic fixture before
the containing codec version becomes stable.

Do not set universal speedup targets until the baseline suite is reproducible.
After baselines exist, define per-workload regression thresholds that account for
measurement noise and run stable performance checks separately from ordinary CI.

## Related documents

- [Architecture](architecture.md)
- [Performance design](performance.md)
- [Implementation roadmap](roadmap.md)
- [R SEXP coverage matrix](sexp-coverage.md)
- [Cross-OS portability](portability.md)
- [Metadata and selective attribute access](metadata-access.md)
- [Encoding and format research](encoding-research.md)
- [Competitor research](research.md)
