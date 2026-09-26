# RDZ Current-State Assessment

Assessment date: 2026-08-09

## Outcome

Phase 1 plus its first performance follow-up are implemented. RDZ now classifies
ordinary logical vectors with scalar/AVX2/NEON kernels and selects constant,
dense bitplane, sparse-patch, run-end, or short-period records per block. It
preserves optional eligible `names`, populates authoritative
object/attribute/block entries, and exposes `rdz_schema()` plus selective
`rdz_attributes()` reads. Unsupported logical overlays and every other root type
retain whole-root XDR fallback. There is not yet an integer codec, compression,
parallel pipeline, or streaming R callback bridge.

The type order remains unchanged. The logical bake-off selected 65,536-value
blocks and the adaptive family, but its strict "win every fst write case" gate
is not yet met for sparse, long-run, and alternating inputs. This is a focused
optimization item rather than a reason to copy fst's physical format. Streaming
generic fallback, general character feasibility, and the data.table
transient-pointer adapter remain required before 0.1.0.

## Implementation inventory

| Area | Current implementation | Target gap |
|---|---|---|
| Public API | `write_rdz()`, `read_rdz()`, `rdz_info()`, authoritative `rdz_schema()`, and selective `rdz_attributes()` | Add presets and extend schema/attribute access with each type |
| Object coverage | Native logical vectors with optional eligible names; broad whole-root XDR fallback for everything else | Add integer and subsequent common-type codecs |
| Container | Version 2 header, native logical/string blocks, populated logical directories, fixed trailer, checksums, and explicit limits | Extend type records without changing discovery or bounds |
| R/Rust boundary | The R thread makes one fused logical eligibility/encoding attempt; Savvy functions are thin and a narrow C shim uses public R APIs for attributes/encoding tags | Extend the same fused dispatch for each native slice |
| IO and memory | Buffered, position-tracked writes; sequential block reads with one reusable encoded buffer; logical decode targets the final R vector; generic R serialization still allocates one complete XDR raw vector | Before 0.1.0, stream R serialization callbacks through the existing block path |
| Integrity | Header, directory-header, complete-directory, and per-block IEEE CRC32 plus canonical span validation | Add fuzzing and cross-OS exchanged corruption fixtures |
| Compression | Adaptive logical constant, modal bitplane, sparse-patch, run-end, and short-period records; no general compressor | Evaluate selective LZ4 only if it closes a measured gap that the specialized modes do not |
| Parallelism | None | Bounded owned-buffer pipeline after integer/double blocks provide enough work |
| Metadata access | Native logical schema is authoritative and names are independently readable; generic inspection remains a bounded synopsis | Extend authoritative descriptors and attributes per native type |
| Benchmarking | Paired read/write latency, stored and object-size throughput, file size, `bench::mark()` allocation proxy, explicit warmups/cache labels, machine/backend metadata, multi-size suites, and representative/expanded cases | Run and interpret the full production-size suite for every performance change; add true peak RSS tooling if the allocation proxy is insufficient |
| Portability | Explicit little-endian fields, low-bit-first logical packing, exact UTF-8/Latin-1/bytes tags, portable ASCII native names, and XDR generic streams | Add cross-OS exchanged fixtures and define each subsequent type's representation |
| Tests | Logical empty/scalar/NA/multiblock/adaptive distributions/names/fallback/selective-read, malformed records, and deterministic golden-wire tests plus broad generic graph coverage | Add cross-OS exchanged fixtures, fuzzing, and integer-slice tests |
| CI and packaging | Windows/macOS/Linux R CMD check, pinned Rust/Savvy, vendored crates and licenses | Add pure Rust jobs, cross-job artifact exchange, and minimum-toolchain/release packaging gates |

## Performance evidence and limits

The current matrix is useful diagnostic evidence, not yet a release-quality
performance claim.

- The completed follow-up benchmark on 2026-08-09 used release code, one million
  values, one thread, three warmups, and 30 `bench::mark()` iterations. RDZ write
  medians versus fst defaults were 1.04/1.71 ms for random, 1.17/1.83 ms for
  no-NA, 1.42/1.27 ms for sparse, 1.44/1.67 ms for mostly missing, 1.00/0.71 ms
  for long runs, and 1.29/0.67 ms for alternating values. The new writer wins
  three of six cases and remains close on sparse, but the strict all-case write
  gate is still open for runs and high-frequency patterns.
- RDZ read medians in the same run were 0.99, 0.96, 0.57, 0.60, 0.62, and 0.66
  ms respectively. fst measured 0.89, 0.83, 1.13, 1.32, 1.03, and 0.84 ms. SIMD
  dense expansion is within roughly 0.1--0.13 ms of fst for random/no-NA input;
  specialized records win sparse, mostly-missing, run, and alternating reads.
- RDZ files were 0.240, 0.121, 0.030, 0.040, 0.0017, and 0.0017 MiB. They are
  smaller than fst in every tested distribution, including the periodic case,
  while retaining CRC32 and atomic replacement. These complete-path results
  justify the adaptive R-specific family and the 65,536-value block size. They
  do not justify claiming a universal write-speed win or adding LZ4 without a
  targeted experiment.
- Generic RDZ files contain the uncompressed R serialization payload plus block
  headers, a bounded synopsis/directory, and a fixed trailer. Phase 0A's exact
  20-byte overhead observation applies only to the retired container version 1.
- RDZ's buffered raw `unserialize()` path can outperform file-connection
  `readRDS()` for character vectors and composed objects. This is useful evidence
  for buffering strategy, but it is not evidence for a native codec.
  In the same focused run, RDZ measured 8.62 vs 23.43 ms for character, 11.18 vs
  30.55 ms for list, and 12.19 vs 29.47 ms for data frame.
- Compressed base R, qs2, qdata, and fst can be dramatically smaller. RDZ cannot
  make a size or matched-size speed claim until native packing and compression
  exist.
- The harness now covers paired read/write latency, two throughput definitions,
  file size, an explicitly labeled R-allocation proxy, multiple sizes, warmups,
  and a user-supplied labeled cold-cache hook. True peak RSS and matched-size
  compression modes remain future work because RDZ has no native compression.
- The default harness now pins a one-thread budget for fst/qs2/qdata and enables
  qs2/qdata checksum validation. Earlier checksum-disabled competitor results
  must not be used for current comparative claims.
- A Phase 0B smoke regression on 2026-08-08 used the representative 100,000-row
  suite, one thread, one warmup, five measured iterations, checksum-enabled
  competitors, and the allocation proxy. RDZ atomic writes were about 0.2--0.4
  ms slower than uncompressed base R for logical/integer/double, while numeric
  and composed reads and composed writes were faster in that run. Logical and
  integer reads still showed the expected validation/allocation tax. Treat this
  short run as a regression signal, not a competitive claim; the full multi-size
  suite remains the per-codec gate. Raw outputs were generated outside the
  repository as required by the benchmark-output policy.

## Architecture decisions retained

The following decisions remain sound and should not be reopened without contrary
implementation evidence:

- one whole-root R-serialization fallback rather than embedded fallback islands;
- native codecs for common leaf vectors, then lists and data frames;
- preservation of all attributes, with fallback for unsupported semantic
  attributes and only registered transient-pointer omissions;
- canonical little-endian native fields and XDR generic streams;
- direct Rust file IO with the R API confined to the R thread;
- independently decodable leaf blocks and a bounded ordered worker pipeline;
- a seekable closing directory from the first native format for metadata access;
- pre-0.1.0 freedom to replace the on-disk format.

## Sequencing changes

The previous Phase 0 combined framing, benchmarking, streaming fallback,
character research, compression, and the data.table adapter. That delayed the
first native feedback loop and scheduled some work before its reusable
infrastructure existed. The aligned sequence is now:

1. Keep the completed Phase 0A benchmark contract as the performance regression
   gate.
2. Keep the completed Phase 0B frame and its documented wire invariants stable
   enough to support the next slices; pre-0.1 incompatibility remains allowed.
3. Keep the completed adaptive logical slice as the reference for SIMD dispatch,
   per-block selection, metadata inspection, attributes, malformed input,
   fixtures, and end-to-end benchmarks. Track the remaining run/periodic write
   gap separately from Phase 2 correctness work.
4. Implement integer and double raw reference codecs before admitting adaptive
   transforms; add parallelism only when profiling shows enough block work.
5. Prototype exact R character bytes/encoding access, then implement character,
   factor, list, and data-frame codecs in the established order.
6. Before 0.1.0, replace the transitional generic allocation with streaming
   callbacks, complete data.table transient-pointer behavior, add compression
   presets selected by evidence, and pass cross-OS fixture/fuzz/release gates.

## Next milestone definition

Phase 2 adds the integer-vector vertical slice without redesigning codec
identification, block framing, directory bounds, trailer discovery, metadata
inspection, or whole-root fallback. Its first completion gate is an exact `i32`
baseline with `NA_INTEGER`, attributes and malformed-input coverage equivalent to
the logical slice. Candidate transforms enter the format only after the expanded
integer benchmark suite shows a complementary win without pathological regressions.

## Related documents

- [Roadmap](roadmap.md)
- [Architecture](architecture.md)
- [Container format](container-format.md)
- [Performance](performance.md)
- [Validation](validation.md)
- [SEXP coverage](sexp-coverage.md)
- [Portability](portability.md)
- [Metadata access](metadata-access.md)
