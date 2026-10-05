# RDZ Current-State Assessment

## Checkpoint 2026-10-05: plan-c Stage G

Character vectors (optionally named) and factors (plain and ordered) are
native (container-format.md, "Native character and factor representation"):
strings as bytes plus encoding tag in the Stage A records, packed on the R
thread and compressed by the pipeline's workers; factor codes as integer
records with the levels as a child object; selective `levels` and `class`
reads. The default dictionary policy is now `auto`. Native-encoded non-ASCII
strings, strings over 1 MiB, factors with other attributes or out-of-range
codes stay generic.

Evidence: every encoding tag and NA under every policy, strings spanning
blocks, the oversized-string fallback, factors with unused, empty and `NA`
levels, ordered factors, and a file read back byte for byte with its tags in
a C-locale R process; fuzzing decoded character roots and factor levels (10M
runs, no finding).

`tools/bench-character.R` (2e6 values, ms and MB; one thread / eight; same
machine and qs2 as above):

| data | native w/r, 1 | native w/r, 8 | native MB | generic w/r, 1 | generic MB | qs2 w/r, 1 | qs2 MB |
|---|---|---|---:|---|---:|---|---:|
| chr high cardinality | 80/127 | 48/116 | 0.62 | 119/242 | 0.58 | 120/189 | 0.74 |
| chr low cardinality | 15/12 | 14/11 | 1.29 | 114/172 | 1.78 | 132/125 | 2.05 |
| chr repeated long | 17/12 | 12/11 | 0.50 | 160/401 | 4.23 | 189/333 | 3.58 |
| chr unique long | 146/450 | 103/440 | 2.03 | 143/576 | 2.05 | 164/513 | 2.05 |
| chr mixed encodings | 16/14 | 13/13 | 0.72 | 146/228 | 3.96 | 149/163 | 3.19 |
| fct low cardinality | 11/8 | 8/8 | 0.48 | 25/16 | 1.19 | 24/10 | 1.04 |
| fct high cardinality | 18/8 | 10/8 | 5.68 | 26/21 | 5.11 | 17/9 | 4.19 |

Unique strings are bound by R's string cache on read (`mkCharLenCE()` per
element), which the deferred-string ALTREP of roadmap's later opportunities
would remove. High-cardinality factor codes (17 bits) are larger than qs2's;
a narrower code layout is a tuning item.

Stage H (lists and data frames) is next.

## Checkpoint 2026-10-05: plan-c Stage F

Integer and double vectors (with optional `names`) are native
(`src/core/rdz_numeric.c`, `rdz_vector.c`; container-format.md, "Native
integer and double representation"), and every native write and read now goes
through the block pipeline: the R thread copies a block's values into a slot,
workers encode, compress and checksum it, and finished blocks are written in
order; reading decompresses in workers and decodes on the R thread into the
result. Interrupts are checked between blocks under `R_UnwindProtect()`.
Logical blocks stay uncompressed (the Rust bytes); integer and double blocks
are compressed by the preset. ALTREP vectors (`1:n`) and classed vectors stay
generic.

Evidence: the R suite and the C harness (ASan and UBSan) round-trip every
record kind at every block boundary, at 1, 4 and 8 threads, bit for bit
(NaN payloads, `NA`, `-0`), with identical bytes across thread counts; every
single-byte change of a small file is detected; fuzzing with integer and
double seeds found nothing.

`tools/bench-numeric.R` (1e7 values, ms and MB; one thread / eight; R 4.6.1,
Apple arm64, 8 cores; qs2 0.2.2 at its default level, checksums validated):

| data | native w/r, 1 thread | native w/r, 8 | native MB | generic w/r, 1 | generic MB | qs2 w/r, 1 | qs2 w/r, 8 | qs2 MB |
|---|---|---|---:|---|---:|---|---|---:|
| int random | 47/20 | 22/20 | 23.9 | 86/72 | 29.9 | 83/23 | 32/15 | 24.0 |
| int full range | 52/6 | 22/7 | 38.2 | 56/46 | 38.2 | 47/24 | 36/23 | 38.2 |
| int sequential | 25/9 | 6/9 | 0.0 | 96/71 | 25.2 | 14/6 | 4/3 | 0.2 |
| int low cardinality | 31/18 | 8/18 | 2.4 | 121/81 | 6.3 | 115/49 | 24/13 | 5.9 |
| int 5% missing | 65/20 | 24/20 | 23.9 | 114/75 | 28.8 | 197/49 | 48/23 | 29.0 |
| int all missing | 27/1 | 6/1 | 0.0 | 36/53 | 0.0 | 13/4 | 4/3 | 0.0 |
| dbl random | 80/37 | 37/22 | 67.3 | 142/132 | 73.1 | 116/61 | 64/42 | 67.2 |
| dbl rounded | 230/71 | 51/15 | 22.1 | 229/128 | 18.9 | 263/87 | 54/23 | 18.4 |
| dbl currency | 112/52 | 34/20 | 50.6 | 195/131 | 63.2 | 422/100 | 92/38 | 42.4 |
| dbl monotonic | 64/28 | 27/18 | 41.6 | 140/129 | 65.6 | 192/87 | 49/40 | 49.7 |
| dbl repeated | 180/79 | 38/16 | 10.7 | 210/138 | 10.4 | 203/83 | 41/20 | 9.0 |
| dbl all missing | 8/2 | 4/2 | 0.0 | 49/81 | 0.0 | 26/9 | 7/6 | 0.0 |

Roadmap Phases 2 and 3's exits: every retained integer record beats raw on a
documented part of the suite (runs on runs and all-missing, delta on
sequences, frame of reference on random, low-cardinality and missing); the
eight-thread pipeline scales writes 2 to 5 times with small vectors
unaffected (one block, no workers started). Open: zstd at level 1 is the
write cost on rounded and repeated doubles (one thread), and qs2 still makes
the smaller files on those and on currency, where ALP (deferred) is the
lever.

Stage G (character and factor) is next.

## Checkpoint 2026-10-05: plan-c Stage E

The native logical codec is C (`src/core/rdz_logical.c`, `rdz_string.c`,
`rdz_native.c`, `src/adapter/rdz_native_r.c`): the tri-state classifier
(scalar reference; AVX2 on x86-64 by run-time dispatch; NEON on AArch64), the
five written record kinds and all six decoders, the character records and
every dictionary policy for `names`, the native writer and reader, and
selective `names` reads. `write_rdz()`, `read_rdz()` and `rdz_attributes()`
no longer call Rust; the Rust implementation remains only as the oracle the
tests compare against.

- Every one of the 21 native reference fixtures is reproduced byte for byte,
  with the SIMD and with the scalar classifier (C harness and R suite), and
  the Rust reader reads every file the C writer writes. A planted change to
  the dictionary width or to record selection is caught.
- Eligibility, fallback and messages are the Rust adapter's; strict mode now
  raises `rdz_unsupported_error`.
- The R floor stays 4.1: attribute counting uses `R_getAttribCount()` from R
  4.6 and `ATTRIB()` before it, as the Rust shim did; nothing else needs more
  than 4.1.
- Roadmap Phase 1's six distributions (`tools/bench-logical.R`; 1e7 values,
  one thread, median of 5 after a `gc()`, NEON, Apple arm64; ms):

  | data | C write | Rust write | qs2 write | C read | Rust read | qs2 read | rdz MB | qs2 MB |
  |---|---:|---:|---:|---:|---:|---:|---:|---:|
  | random | 5 | 5 | 73 | 7 | 10 | 27 | 2.40 | 3.55 |
  | sparse | 5 | 6 | 19 | 2 | 1 | 8 | 0.31 | 0.37 |
  | mostly TRUE | 5 | 6 | 18 | 2 | 2 | 6 | 0.30 | 0.44 |
  | mostly NA | 5 | 6 | 21 | 2 | 2 | 8 | 0.40 | 0.59 |
  | runs | 4 | 3 | 13 | 1 | 1 | 5 | 0.02 | 0.00 |
  | alternating | 4 | 7 | 7 | 1 | 1 | 11 | 0.02 | 0.00 |

  The first port was 1.5x slower than Rust on writes; three changes closed it:
  classifier counts kept in kernel-local state (stores through the planes'
  byte pointers had forced reloads), the sparse encoder scanning planes 64
  bits at a time, and a 1 MiB stdio buffer on output files (one write call
  per MiB rather than per small block, which also helps every writer).

Stage F (integer and double) is next.

## Checkpoint 2026-10-05: plan-c Stage D

The block pipeline and compression are in place:

- `src/core/rdz_pipeline.c`: a ring of 2 x threads slots and an rdz-owned
  pthread pool (winpthreads on Windows). The R thread fills and submits slots
  and consumes finished ones strictly in sequence order, so memory is bounded
  by the slots and the bytes do not depend on the thread count. Workers start
  on the second block, so a one-block object never pays for them (the
  small-input bypass); with one thread every job runs inline. Teardown joins
  the workers and is safe from an unwind cleanup.
- `src/core/rdz_codec.c`: zstd 1.5.7, vendored as one translation unit without
  its own threads or dictionary builder (`tools/vendor/{manifest.tsv,fetch,
  record,verify}`, `inst/COPYRIGHTS`, a `cph` entry), every symbol hidden. A
  block is compressed only when that makes it smaller; decoding requires one
  frame of exactly the stored bytes that decodes to exactly the decoded length.
- `options(rdz.preset = )` (speed raw, balanced zstd 1, compact zstd 6) and
  `options(rdz.threads = )`, default 1.
- Evidence: the C harness writes and reads 3 MiB in 4 KiB blocks through 1, 2
  and 8 threads under ASan and UBSan with identical files, raw and zstd blocks
  both present; the R suite does the same through `write_rdz()`; fuzzing with
  compressed seeds found nothing; `tools/vendor/verify` passes and fails on an
  edited file or a missing define.
- Equal-budget scaling (`tools/bench-pipeline.R`, Apple arm64, 8 cores, median
  of 5 after a `gc()`; write/read ms, balanced preset; qs2 0.2.2 at its default
  level with checksum validation):

  | data | threads | rdz | qs2 |
  |---|---:|---:|---:|
  | 2e7 doubles (48.8 / 52.2 MB) | 1 | 866 / 422 | 1125 / 303 |
  |  | 2 | 359 / 202 | 585 / 300 |
  |  | 4 | 239 / 202 | 450 / 130 |
  |  | 8 | 266 / 202 | 298 / 79 |
  | 4e7 integers (66.4 / 64.7 MB) | 1 | 991 / 490 | 1295 / 328 |
  |  | 4 | 320 / 288 | 526 / 139 |
  |  | 8 | 357 / 287 | 351 / 95 |
  | 5e6 strings (15.3 / 14.7 MB) | 1 | 592 / 1021 | 701 / 788 |
  |  | 8 | 349 / 954 | 331 / 730 |

  Writes scale and match or beat qs2. Reads stop at about 200 ms for 160 MB:
  `R_Unserialize()`'s single-threaded XDR conversion is the floor (the speed
  preset reads in 213 ms), which the native codecs of Stages E to H remove.
  Small objects show no regression: 1e3 and 1e5 doubles and a 100-element list
  write and read within a millisecond or two under every preset and thread
  count.

Stage E (the logical codec, ported) is next.

## Checkpoint 2026-10-05: plan-c Stage C

The generic codec is C and streamed (`src/adapter/rdz_generic.c`):
`R_Serialize()` writes through an `R_outpstream` that fills a 1 MiB block and
hands each full block to the container writer; `R_Unserialize()` reads through
an `R_inpstream` that pulls blocks in order, each verified before it is used.
Both run under `R_UnwindProtect()`, so an error or an interrupt (checked between
blocks) removes the temporary file or closes the reader at once; the external
pointer's finalizer is the backstop. A block that fails its checksum is raised
as the classed `rdz_format_error` from inside the stream.

- `write_rdz()` writes every object the native logical codec does not take
  through it (`mode = "r"` always), in every build; `read_rdz()` reads every
  generic file through it. Native logical files are still written and read by
  the Rust oracle until Stage E; a build without Rust writes logicals
  generically, and `mode = "native"` is an error there.
- The synopsis's root length comes from C (`rdz_c_root_length()`).
- The streamed writer's files are byte for byte the Rust writer's under the
  same R version (checked against the reference corpus).
- Memory: a forced-generic write allocates almost nothing on the R heap, where
  the Rust path allocated the whole payload; the C side holds one 1 MiB block.
  Measured with `tools/bench-generic.R` (median of 5, one thread, R 4.6.1,
  Apple arm64, qs2 0.2.2 at its defaults with checksum validation):

  | data | format | write ms | read ms | R heap per write, MB | file MB |
  |---|---|---:|---:|---:|---:|
  | data frame, 1e6 rows | rdz (C, streamed) | 151 | 211 | 0.19 | 31.5 |
  |  | rdz (Rust, whole payload) | 147 | 207 | 31.49 | 31.5 |
  |  | qs2 | 94 | 113 | 0.03 | 0.08 |
  |  | saveRDS, uncompressed | 299 | 363 | 0.03 | 31.5 |
  | 100 numeric vectors of 1e5 | rdz (C, streamed) | 161 | 161 | 0.02 | 76.3 |
  |  | rdz (Rust, whole payload) | 209 | 207 | 76.31 | 76.3 |
  |  | qs2 | 131 | 64 | 0.01 | 1.2 |
  |  | saveRDS, uncompressed | 148 | 164 | 0.01 | 76.3 |
  | character, 1e6 | rdz (C, streamed) | 95 | 223 | 0.02 | 17.2 |
  |  | rdz (Rust, whole payload) | 102 | 206 | 17.18 | 17.2 |
  |  | qs2 | 122 | 146 | 0.01 | 0.4 |
  |  | saveRDS, uncompressed | 256 | 370 | 0.01 | 17.2 |

  qs2's lead on these is compression (its files are 40 to 400 times smaller):
  rdz has none until Stage D. Without it, the streamed generic path matches the
  whole-payload one on speed and removes its allocation.

Stage D (the pipeline and compression) is next.

## Checkpoint 2026-10-05: plan-c Stage B

The C skeleton and the R-free container are in place beside the Rust oracle:

- `src/core/` (C99, no R header): the wire constants and error kinds
  (`rdz_format.h`); the seven records as zubin layout specifications, proved
  against their offsets by `rdz_records_check()` (`rdz_records.c`); files with
  64-bit offsets, UTF-8 paths on Windows, and same-directory temporary files
  renamed over the destination with its permissions kept (`rdz_io.c`); and the
  container reader and writer (`rdz_container.c`). The reader is the Rust
  reader's validation check for check, with its messages, plus one bound the
  Rust reader lacked: a logical root's block range is checked against the
  block table before it is indexed. Checksums are zufast's XXH3.
- `src/rdz_r.c` and `src/rdz_init.c`: `rdz_info()` now reads through the C
  reader; failures are classed conditions inheriting `rdz_error`. Writing and
  reading still go through Rust.
- The build: `configure` builds the Rust oracle only with R 4.5 or newer and
  cargo and rustc 1.88 (`RDZ_RUST=0` turns it off); without it the C
  implementation builds alone, Rust-dependent tests skip, and the examples
  (gated on `rdz:::rdz_has_rust()` until Stage C) do not run.
- The R floor is R 4.1: the family's floor (zubin, zufast), and all the C
  implementation needs so far. It is confirmed or raised at Stage E, where the
  C adapter iterates attributes (the Rust shim used `R_getAttribCount()`, an R
  4.6 API, with an `ATTRIB()` fallback).
- Evidence: `tools/run-c-tests` (2,422 checks under ASan and UBSan) reads all 33
  reference fixtures, reproduces every generic fixture byte for byte with the C
  writer, and rejects every single-byte change and every truncation of two
  fixtures; the libFuzzer target ran ten minutes on the reader with no finding
  after its canary crashed; `rdz_info()` matches the Rust build's for every
  fixture, in builds with and without Rust; the C writer's files are read by
  the Rust reader.
- CI: `hardening` (the harness under gcc and clang, fuzzing 10 minutes per
  push and an hour nightly), `native-checks` (sanitizers, valgrind, LTO,
  gctorture, blocking rchk), and `arch` (i386, musl, s390x; weekly and on
  `full-ci`), beside `R-CMD-check`.

Stage C (the generic codec, streamed) is next.

## Checkpoint 2026-10-05: plan-c Stage A

rdz is being re-implemented in C on zubin and zufast (plan-c.md, adopted
2026-10-05). Stage A is done:

- The three format decisions of plan-c.md section 3 are taken and written
  where the format is specified: container version 3 with XXH3-64 checksums in
  eight-byte fields (container-format.md), zstd vendored as compression ID 1
  (reserved; not written or accepted yet), and the generic payload streamed
  through `R_Serialize()`/`R_Unserialize()` with unchanged bytes
  (encoding-research.md, "Format decisions of 2026-10-05").
- The Rust implementation writes and reads version 3. Its XXH3 fields agree
  bit for bit with `zufast::fast_hash()`.
- The character dictionary (encodings 8 and 9) is landed in Rust (#3), so the
  character record is fixed by the oracle.
- The reference corpus is in `tests/testthat/fixtures/rust/`: 33 files, from
  the commit recorded in `manifest.tsv`, covering native logical lengths 0, 1,
  65,535, 65,536, 65,537 and multiblock; every logical record kind (3 to 7);
  names under every dictionary policy, in every string encoding, and over two
  string blocks; generic payloads at 1 MiB minus one, exactly, plus one and
  multiblock; a rare object; and automatic fallback for each common type.
  `tools/make-rust-fixtures.R` regenerates it from the specs in
  `helper-rust-fixtures.R`; `test-rust-fixtures.R` checks its SHA-256 sums,
  that every fixture reads back `identical()` to its spec, and that
  `rdz_info()` matches what the Rust build reported. Encoding 1, the legacy
  two-bit logical record, has no writer and so no fixture; whether the C reader
  keeps it is decided at Stage E.
- The Rust tree is tagged `rust-reference-pre-c`.

Stage B (the C skeleton and the R-free container) is next. The assessment
below describes the Rust implementation as of 2026-08-09 and is kept for its
evidence.

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
