# Encoding and Format Research Directions

## Purpose

This document is the candidate register for algorithms and layout techniques that
may improve RDZ. It complements [research.md](research.md), which analyzes direct
R competitors. The purpose is not to reproduce Arrow, Parquet, ORC, or a database
storage format. RDZ should borrow well-tested leaf encodings and streaming ideas
while retaining its own R semantics, object traversal, and compatibility policy.

Every candidate starts as research. It becomes part of RDZ only after it:

1. preserves the relevant R semantics exactly;
2. beats the simple reference encoding on a documented workload;
3. has bounded decode memory and safe malformed-input behavior;
4. has a scalar, portable path on all supported platforms;
5. has acceptable Rust-version, licensing, CRAN vendoring, and maintenance cost;
6. receives an explicit physical encoding ID and compatibility tests.

Lossy encodings are out of scope. Algorithms must preserve integer values,
floating-point bits, missing values, string bytes/encodings, and object metadata
as defined by [architecture.md](architecture.md).
They must also satisfy the wire and cross-OS invariants in
[portability.md](portability.md); a faster host-specific file representation is
not an adoption candidate.

## How to record a research decision

Each experiment or proposal should record:

- source format, implementation, or paper and exact version/commit;
- applicable R types and representative datasets;
- physical layout and required metadata;
- encode/decode complexity and whether decoding has serial dependencies;
- SIMD, multithreading, and cache behavior;
- encoded size, write throughput, read throughput, and peak memory;
- random-access and independent-block consequences;
- NA, NaN, character-encoding, attribute, and portability implications;
- available Rust crates or code and their dependency/licensing impact;
- result: **adopt**, **experiment further**, **defer**, or **reject**.

Do not select an algorithm from compression ratio alone. RDZ's primary goal is
fast end-to-end R serialization, so R traversal, packing, allocation, copying,
checksumming, and IO remain part of the decision.

## Source families

### Apache Arrow: physical buffer layouts

[Arrow's columnar format](https://arrow.apache.org/docs/format/Columnar.html)
defines contiguous value buffers, validity bitmaps, offset-plus-data variable
binary arrays, dictionary arrays, run-end encoding, and record batches. These are
valuable layout references because they separate logical type metadata from
cache-friendly buffers.

Research directions for RDZ:

- offsets plus concatenated bytes for character blocks;
- dictionary values plus packed indices for repeated character vectors;
- run-end encoding for long runs in logical, integer, and factor vectors;
- aligned, contiguous block buffers that can be transformed in bulk;
- record-batch-like grouping for scheduling data-frame columns.

Do not adopt Arrow's validity model automatically. R logical vectors have three
value states, and R doubles distinguish `NA_real_` from ordinary NaN bit patterns.
A separate validity bitmap may help compression, but it must be compared with
two-bit logicals and exact raw floating-point bits. Arrow is an in-memory and IPC
interchange model, not an R object-graph format.

### Apache Parquet: page-level adaptive encodings

The [Parquet encoding specification](https://parquet.apache.org/docs/file-format/data-pages/encodings/)
defines dictionary encoding, hybrid RLE/bit packing, delta binary packing,
delta-length byte arrays, prefix/delta strings, and byte-stream split. Parquet's
[file layout](https://parquet.apache.org/docs/file-format/) uses independently
addressable column chunks/pages and final metadata for single-pass writing.

Research directions for RDZ:

- hybrid RLE/bit packing for logical states, factor codes, and small-range
  integers;
- frame-of-reference/minimum-delta plus miniblock bit widths for integer vectors;
- delta-encoded string lengths followed by concatenated bytes;
- prefix compression for sorted or prefix-heavy strings;
- byte-stream split for doubles before a general compressor;
- per-block encoding identifiers and raw fallback for incompressible blocks;
- footer indexes for future selective data-frame reads.
- a fixed trailer and checksummed object/attribute/block directory for required
  metadata/schema and selective attribute reads.

Parquet assumes a table schema, definition/repetition levels, and columnar query
access. RDZ should evaluate its leaf encodings without importing those container
semantics. In particular, a Parquet encoding may improve size but lose on full R
allocation or reconstruction speed.

### Apache ORC: adaptive integer and string policies

The [ORC specification](https://orc.apache.org/specification/ORCv2/) describes
independently compressed chunks, RLEv2 integer sub-encodings, and direct versus
dictionary string encoding. RLEv2 selects among short-repeat, direct bit-packed,
patched-base, and delta forms according to local data.

Research directions for RDZ:

- a small, deterministic per-block selector rather than a single integer codec;
- patched frame-of-reference for blocks with a narrow range and a few outliers;
- direct-versus-dictionary decisions for character blocks;
- store raw data when compression expands a block;
- independent compression chunks that limit corruption and decompression scope.

ORC's selector complexity is a warning as well as an opportunity. Measure whether
the extra scan and branches pay for themselves at RDZ block sizes. Start with a
small candidate set and add modes only for demonstrated workload gaps.

### Blosc2 and Zstandard: filters and independent frames

The [Blosc2 chunk format](https://blosc.org/c-blosc2/format/chunk_format.html)
supports chunks composed of blocks and an ordered filter pipeline including byte
shuffle, bit shuffle, and delta transforms. Its
[super-chunk model](https://blosc.org/c-blosc2/reference/schunk.html) collects
independently stored chunks. The
[Zstandard frame format](https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md)
supports bounded-memory streaming, independent concatenated frames, raw blocks,
and optional checksums.

Research directions for RDZ:

- explicit transform pipeline metadata separate from the compression codec;
- byte shuffle and bit shuffle for fixed-width vectors;
- independent frames per RDZ block for parallel decompression;
- raw-block fallback when compressed bytes are not smaller;
- buffer pools and bounded queues sized by blocks rather than objects;
- per-block integrity plus footer integrity.

RDZ should initially implement only the small set of filter combinations selected
by presets. An unrestricted filter graph would expand the compatibility and test
surface without helping ordinary serialization.

### DuckDB, FSST, and adaptive lightweight compression

DuckDB documents its use of constant, RLE, bit-packing, frame-of-reference,
dictionary, FSST, ALP, Chimp, and Patas encodings in its
[storage documentation](https://duckdb.org/docs/stable/internals/storage.html#compression)
and [lightweight-compression overview](https://duckdb.org/2022/10/28/lightweight-compression).
This is a useful example of choosing codecs per segment rather than forcing all
values through a single general compressor.

Research directions for RDZ:

- constant encoding as a near-zero-cost special case for every atomic type;
- sample-driven selection that limits full candidate evaluation;
- FSST for high-cardinality strings sharing common substrings such as paths,
  URLs, and identifiers;
- dictionary plus FSST for repeated strings with repeated substrings;
- ALP for decimal-like floating-point measurements;
- Chimp/Patas-style XOR encodings for locally similar numeric sequences.

FSST has table-construction overhead and metadata cost, so it should target large
character blocks after ordinary dictionary detection. Advanced floating-point
codecs can have data dependencies that restrict SIMD or block-level parallelism;
evaluate decode throughput, not just size.

### Floating-point research

The [ALP paper](https://ir.cwi.nl/pub/33334/33334.pdf) describes sampled,
vectorized adaptive lossless floating-point compression. The
[Chimp paper](https://www.vldb.org/pvldb/vol15/p3058-liakos.pdf) studies the
Gorilla XOR family and improves its treatment of leading/trailing zero patterns.

Candidate families:

- raw exact `u64` bits;
- eight-byte shuffle plus LZ4/Zstandard;
- Parquet byte-stream split plus LZ4/Zstandard;
- decimal-to-integer transforms with exact exceptions, as in ALP;
- XOR with previous value, with Gorilla/Chimp/Patas-style metadata;
- constant and run-length modes.

All candidates must preserve the original 64 bits, including R's NA payload,
ordinary NaN payloads, infinities, subnormals, and negative zero. Test random
full-precision data as an essential negative control: many specialized numeric
codecs should reject it quickly and use raw or shuffled storage.

#### ALP assessment for RDZ

[ALP](https://github.com/cwida/ALP) is a strong Phase 3 candidate. Its decimal
scheme maps suitable doubles to `i64`, frame-of-reference encodes and bit-packs
them, and stores values that do not reproduce exactly in an exception stream.
ALP-RD handles high-precision values by dictionary-encoding their common leading
bits while retaining the remaining bits. The reference design samples row groups
and operates on vectors of 1,024 values, which fits RDZ's independently decodable
block pipeline and ordered worker pool.

R-specific requirements are stricter than merely round-tripping ordinary finite
doubles:

- compare and restore exact `f64::to_bits()` values;
- force `NA_real_`, every NaN payload, infinities, negative zero, and any value
  that does not reproduce bit-for-bit through the decimal transform into the
  exact exception path;
- store exception values as canonical little-endian `u64` bits, not host doubles;
- verify that scalar and SIMD paths produce the same decoded bits;
- reject malformed exception positions, counts, dictionaries, widths, and
  vector lengths before allocation or unchecked access;
- retain raw `u64` and shuffled/raw-plus-compressor fallbacks when ALP expands or
  sampling cost is not recovered.

Do not persist the C++ reference implementation's structs or assume that any
third-party crate's in-memory representation is a stable file format. RDZ needs
its own versioned ALP block record containing the scheme, vector length,
factor/exponent or ALP-RD parameters, frame-of-reference base, bit widths,
dictionaries, exception positions, and stream lengths using explicit portable
fields.

Implementation options to prototype, not yet adopt as a stable dependency:

1. Port the small required primitives to ordinary safe Rust behind RDZ's codec
   traits, using the MIT C++ implementation and paper as references.
2. Evaluate the Apache-2.0 [`alp` crate](https://docs.rs/alp/latest/alp/) as a
   primitive library. Its current `0.0.1` version and partial documentation make
   its API and maintenance maturity a review item, and it is not a complete RDZ
   wire format.
3. Benchmark the MIT C++ reference through a narrow bridge only as an oracle and
   performance comparison. Shipping it would add C++ build/link/vendor work to a
   Rust R package and should require a material measured advantage.
4. Evaluate the recent pure-Rust
   [`entropack-alp`](https://docs.rs/crate/entropack-alp/latest) implementation as
   a test oracle. It documents bit-exact special-value handling and validation,
   but explicitly uses its own non-reference wire layout and is too new to freeze
   into RDZ without source audit, cross-platform fixtures, fuzzing, and benchmark
   evidence.

The preferred initial experiment is a self-contained safe-Rust ALP/ALP-RD block
prototype with an explicit raw fallback. Keep it only if it improves the
end-to-end throughput/size Pareto frontier on R numeric workloads after sampling,
R allocation, checksums, optional general compression, and multithread scheduling
are included.

### Structural framing: Protocol Buffers and CBOR

The [Protocol Buffers wire format](https://protobuf.dev/programming-guides/encoding/)
demonstrates compact field tags, wire types, packed repeated values, and
length-delimited records. [CBOR](https://www.rfc-editor.org/rfc/rfc8949.html)
demonstrates explicit semantic tags plus definite- and indefinite-length
containers.

Research directions for RDZ:

- type/field tags that allow unknown optional metadata to be skipped;
- packed homogeneous vectors instead of one tag per scalar;
- self-delimiting child records where their size is available cheaply;
- count-delimited or explicit-end composite records when precomputing nested byte
  lengths would force buffering;
- canonical encodings for deterministic fixtures and strict malformed-input
  rejection.

RDZ should not adopt Protocol Buffers or CBOR as its payload: neither directly
models R's typed NA values, per-string encodings, attributes, or vector blocks.
Their framing lessons are useful independently of their data models.

### R serialization: semantic compatibility

R's [serialization format documentation](https://cran.r-project.org/doc/manuals/r-devel/R-ints.html#Serialization-Formats)
is the reference for object/reference tables, versioning, ALTREP hooks, and source
native-encoding metadata. RDZ's dedicated native codec is intentionally narrower,
while the overall public API retains full base-R coverage through one whole-root
R serialization stream. Study these mechanisms before implementing native lists,
references, general attributes, or ALTREP.

Research directions for RDZ:

- a direct `R_Serialize()`/`R_Unserialize()` callback bridge to bounded RDZ blocks;
- explicit payload codec/version metadata for the generic stream;
- automatic whole-root selection without independent nested fallback islands;
- object IDs and back-reference records for supported shared/cyclic structures;
- source-native character encoding and cross-locale reconstruction;
- explicit behavior for ALTREP values: value materialization first, custom lazy
  representation only later;
- compatibility fixtures that document where native RDZ semantics differ from
  R serialization.

## Research lines by RDZ type

### Logical vectors

The implemented reference is two interleaved bits per R logical value. It proves
the vertical slice but is not the target performance design. fst's logical codec
already packs 32 R logicals into one 64-bit word with fixed masks and optionally
applies LZ4. RDZ should treat that as a comparison baseline rather than reproduce
it.

#### 2026-08-09 SIMD bitplane finding

R's in-memory states are `0`, `1`, and `NA_LOGICAL` (`INT_MIN`). This permits one
SIMD classification pass to compare wide `i32` lanes against `1` and `INT_MIN`,
extract a TRUE mask and an NA mask, validate all lanes, and accumulate exact
block statistics. The proposed canonical intermediate is therefore two separate
bitplanes, not fst-style interleaved two-bit words:

- a TRUE plane where one bit represents `TRUE`;
- an NA plane where one bit represents `NA`;
- `FALSE` is implied when neither bit is set;
- the two bits set at one position is invalid.

A disposable x86-64 AVX2 probe classified, validated, allocated, and emitted both
planes for one million values in 0.30--0.31 ms, or 3.2--3.4 billion values per
second. Balanced, sparse, long-run, and no-NA inputs had equivalent throughput.
For comparison, the complete current RDZ writer measured 2.38--4.66 ms and fst
with its secondary compression disabled measured 1.27--1.35 ms. This is a kernel
result, not an end-to-end claim, but it supplies enough headroom to justify a new
codec experiment before optimizing the interleaved reference.

The implementation now emits the same canonical bytes through
runtime-dispatched kernels:

1. portable branchless scalar/SWAR fallback;
2. x86-64 AVX2 comparison plus mask extraction;
3. AArch64 NEON comparison plus narrowing/mask extraction;
4. AVX-512 only if it produces a further measured win without raising the
   package's minimum CPU requirement.

Rust's per-function target features and runtime feature detection allow optimized
kernels without making AVX2 a file or package requirement. SIMD affects how bytes
are produced, never their canonical little-endian representation.

#### Implemented RDZ tri-state block family

Classification produces the two dense planes and exact statistics in one
pass: counts of all three states, presence of NA, and transition/run count. Because
the domain has only three states, selection can use exact analytical sizes rather
than BtrBlocks-style sampling or trial-compressing every candidate.

The speed path evaluates these physical modes per block:

1. **Constant:** one of FALSE, TRUE, or NA in metadata and no payload.
2. **Dense planes:** choose a modal default state and store bitplanes only for the
   other states that occur. Ordinary non-missing logicals need one bit per value;
   all-three-state blocks need two.
3. **Sparse patches:** modal default plus sorted positions for the other states.
   Compare plain `u16` positions in blocks of at most 65,536 values with
   delta-coded Stream VByte/SIMD-BP128. Prefer the simplest representation whose
   complete encode and decode win is demonstrated; do not embed a general bitmap
   library merely for logical vectors.
4. **Run-end:** state values plus run ends for blocks with few transitions. Compare
   this with a Parquet-style RLE/bit-pack hybrid, retaining only the faster and
   simpler mode.
5. **Short periodic:** for high-transition blocks only, verify exact repetition
   with a period of 2 through 64 and store the period in the reference two-bit
   representation. This captures patterns that defeat ordinary RLE without the
   dependency and trial cost of a general compressor.
6. **Dense planes plus LZ4:** try only when exact statistics predict enough repeated
   bytes to recover the compression cost. Raw dense planes remain mandatory.
7. **Five-trit dense packing:** five ternary states per byte (`3^5 = 243`) gives
   1.6 bits/value, close to the balanced ternary entropy bound of about 1.585.
   Prototype lookup/SIMD packing for a compact preset, but do not use it in the
   speed preset unless it beats bitplanes end to end.

The bake-off selected 65,536-value blocks. A 1,048,576-value experiment reduced
directory overhead but required wider sparse positions and slowed sparse/run
writes; 65,536 permits `u16` positions and turns most long-run blocks into
constant records. The already materialized bitplanes are at most one quarter the
R input size, so dense, sparse, and run bytes are derived from them instead of
rescanning the four-byte R vector. Short-period verification is attempted only
when the transition count exceeds half the block, preventing repeated scans of
long-run candidates.

This direction combines established ideas rather than adopting another format:

- [Arrow](https://arrow.apache.org/docs/format/Columnar.html) demonstrates
  independent value and validity bitmaps and omission of unnecessary validity;
- [Parquet](https://parquet.apache.org/docs/file-format/data-pages/encodings/)
  provides the RLE/bit-pack hybrid reference;
- [BtrBlocks](https://doi.org/10.1145/3589263) and
  [Vortex](https://docs.vortex.dev/concepts/arrays) demonstrate adaptive,
  statistics-guided constant, sparse, run-end, and bit-packed encodings;
- [FastLanes](https://www.vldb.org/pvldb/vol16/p2132-afroozeh.pdf) provides a
  modern SIMD-friendly layout reference;
- [Stream VByte](https://arxiv.org/abs/1709.08990) and
  [SIMD-BP128/FastPFOR](https://github.com/fast-pack/FastPFOR) are candidates for
  sparse position streams;
- [Roaring](https://arxiv.org/abs/1709.07821) motivates selecting array, bitmap,
  or run representations by local density, but its full container format is
  likely unnecessary at RDZ logical block scale.

#### Adoption gate

The new family is adopted before 0.1.0 only when all of the following hold:

- byte-identical output from scalar, AVX2, and NEON kernels;
- full validation of invalid in-memory states and malformed plane combinations;
- faster complete writes than fst with one thread on balanced, no-NA, sparse,
  mostly-missing, alternating, and long-run suites—not merely a faster kernel;
- no material regression from the current native read path;
- smaller files than the fixed two-bit reference for no-NA, constant, sparse,
  and run-heavy inputs, with bounded expansion impossible;
- block selection, CRC32, atomic file replacement, and R allocation included in
  the reported timings.

Status on 2026-08-09: scalar/AVX2 classification equivalence, malformed-record
validation, bounded expansion, complete-path CRC/atomic timings, and the file
size goals pass locally. Reads win four of six expanded cases. Complete writes
win random, no-NA, and mostly-missing cases, are about 0.15 ms behind fst for
sparse input, and remain about 0.29 and 0.62 ms behind for long-run and
alternating input. The family is retained pre-0.1 for its clear Pareto gain, but
the all-write criterion and AArch64 byte-equivalence CI check remain open; do not
describe the adoption gate as fully passed.

### Integer vectors

Baseline: exact little-endian `i32`, optionally byte-shuffled.

Experiments:

1. Constant and RLE.
2. Frame-of-reference plus packed residuals.
3. Delta plus zigzag and miniblock bit widths.
4. Patched frame-of-reference for rare outliers.
5. General LZ4/Zstandard on raw and shuffled bytes.

Decision questions:

- Can a sample or one-pass statistic select a codec without trying all modes?
- Does delta decoding's prefix dependency limit per-core throughput?
- Is transform time recovered after including direct file IO and R allocation?

### Numeric vectors

Baseline: exact little-endian `u64` bits.

Experiments:

1. Eight-byte shuffle.
2. Byte-stream split.
3. Constant/RLE.
4. ALP on decimal-like values with an exact exception path.
5. XOR-family encoding for locally similar sequences.
6. Raw fallback for high-entropy full-precision values.

Decision questions:

- Which candidates preserve arbitrary NaN payloads without special cases leaking
  into the logical format?
- Which decode fastest into contiguous R memory?
- Can sampling reject unsuitable advanced codecs cheaply?

### Character vectors

Baseline: element count, NA/encoding metadata, lengths or offsets, and
concatenated bytes.

Experiments:

1. Raw lengths versus delta-coded lengths.
2. Whole-string dictionary plus bit-packed indices.
3. Direct versus dictionary selection by sampled cardinality and byte coverage.
4. Prefix/delta strings for sorted or path-like values.
5. FSST after dictionary selection for large high-cardinality blocks.
6. Separate compression of metadata and bytes.

Decision questions:

- Does an encoding preserve mixed R encoding tags and byte strings exactly?
- Is dictionary construction dominated by R-to-owned-buffer packing?
- Should dictionaries be block-local, vector-local, or hybrid?
- How are oversized individual strings represented without breaking block limits?

#### 2026-09-27 CHARSXP-identity dictionary experiment

**Idea.** R's global CHARSXP cache already deduplicates strings in memory: a
STRSXP is pointers to one CHARSXP per distinct (bytes, encoding). The writer
keys a dictionary on the CHARSXP address (one hash probe on 8 bytes per
element; values validated and copied only on first sight). The reader creates
each distinct CHARSXP once and reuses its pointer for repeats, skipping the
per-element hash-and-compare in `mkCharLenCE`. `qs2`, `qdata`, `fst`, and base
R all call `mkCharLenCE` once per element.

**Layout.** Encodings 8 (entries) and 9 (indices, `u32` base plus 1/2/4-byte
offsets per 65,536-element block), specified in
[container-format.md](container-format.md). One forward pass on read.

**Setup.** `tools/bench-strings.R`, 1,000,000 strings, 1% `NA`, exact
distinct counts, short (~13 B) and long (~110 B, random hex) values, random
and sorted order; one thread; Apple M1, R 4.6.1, `qs2` 0.2.2 (level 3,
checksum validated), `fst` 0.9.8 (compress 50); seven interleaved rounds
with shuffled format order; release build. RDZ is uncompressed. Cold reads
drop the source and run `gc()` first, so every distinct CHARSXP is created.

**Results** (median ms; `auto` = dictionary below 75% estimated distinct):

| Case | rdz plain cold | rdz auto cold | best other cold | rdz auto write | fst write | rdz auto MiB | qdata MiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0.01% short | 34.4 | **8.8** | 41.1 fst | **10.1** | 43.0 | **0.96** | 1.61 |
| 1% short | 41.9 | **10.4** | 45.6 fst | **9.5** | 40.6 | **2.08** | 2.86 |
| 10% short | 91.4 | **25.6** | 93.3 fst | **20.6** | 43.6 | 5.41 | **4.73** |
| 50% short | 152 | **102** | 148 fst | 74.9 | **55.4** | 12.2 | **5.11** |
| 75% short | 174 | **137** | 172 fst | 102 | **59.7** | 16.5 | **5.15** |
| 100% short | 170 | 174 | **168** fst | 83.0 | **60.4** | 17.0 | **5.17** |
| 0.01% long | 169 | **8.9** | 158 qdata | **7.4** | 102 | **0.97** | 2.91 |
| 1% long | 184 | **12.3** | 191 fst | **10.3** | 148 | **2.93** | 34.2 |
| 10% long | 378 | **53.4** | 378 fst | **29.8** | 165 | **13.9** | 48.7 |
| 50% long | 424 | **209** | 416 fst | **111** | 183 | 54.5 | **50.2** |
| 10% short sorted | 53.5 | **19.8** | 53.1 qdata | **20.3** | 44.0 | 4.29 | **0.56** |

Warm reads (strings already cached) follow the same pattern. Cold reads of
long strings at 75–100% distinct allocate ~130 MB of CHARSXPs per read and
are GC-bound for every format: medians varied up to 2x between identical
encodings, so those rows were judged on warm reads, where `auto` matched
plain within 1% at 100% and the dictionary won 22% at 75%.

**Findings.**

- The dictionary reads 3.6–4.7x faster than the best competitor for short
  strings up to 10% distinct and 7–18x for long strings; writes are 2–14x
  faster than `fst`, the fastest competing writer.
- A vector-wide dictionary dominates a block-local one, which loses its
  benefit once the distinct values exceed one block (10% short: 80 vs 25 ms
  cold). **Reject block-local.**
- The read crossover is about 75–80% distinct; above about 50% the identity
  map costs more to build than it saves on writes. `auto` samples 16,384
  fixed-seed random positions and applies bias-corrected Chao1, which stays
  within tolerance for sorted and shuffled order; a first-chunk rule was
  rejected because early chunks look unique in large vectors regardless of
  global repetition.
- At high cardinality RDZ is 2–3x larger than zstd-compressed `qdata`: there
  is nothing to deduplicate, and only the compression layer can close it.
- Remaining write gap versus `fst` at 50–100% distinct short strings (55–60 vs
  75–102 ms): candidates are pre-sizing the identity map from the Chao1
  estimate and fewer R API calls per new value.

**Decision.** Adopt encodings 8/9 and the `auto` policy for native character
data, subject to the default-policy change; reject block-local dictionaries.

### Factors

Baseline: levels once plus exact codes.

Experiments:

1. Minimum-width packed codes with an explicit missing code.
2. RLE/bit-pack hybrid.
3. Run-end encoding for sorted factors.
4. Raw integer fallback for very high level counts.

Factor research should reuse integer and character primitives rather than define
unrelated codecs.

### Lists and references

Baseline: ordered recursive records with explicit limits.

Experiments and design research:

1. Count-delimited children versus explicit end markers.
2. Stable object IDs and back-references for supported composite sharing.
3. Cycle detection and early rejection where reconstruction is unsupported.
4. Scheduling leaf blocks without buffering complete child objects.

Compression of structural metadata is secondary to predictable decoding and
safe recursion.

### Data frames

Baseline: classed list semantics with independently block-encoded columns.

Experiments and design research:

1. Schedule columns before splitting within columns, then balance with block
   tasks for narrow tables.
2. Footer index of column and block offsets.
3. Shared character dictionaries only if cross-column reuse repays coordination
   and harms neither independent decoding nor parallelism.
4. Optional future record-batch/row-group boundaries for very large tables.

Do not make arbitrary row or column data access a prerequisite for the first
native release. Metadata/schema inspection and selective supported-attribute
reads are prerequisites under [metadata-access.md](metadata-access.md); preserve
enough offset metadata that broader selective reads can be added without
rewriting leaf encodings.

## Format decisions of 2026-10-05 (container version 3)

Taken with the move to a C implementation (plan-c.md §3, §10) and recorded in
container-format.md.

**Checksum: XXH3-64, seed 0, in eight-byte fields — adopt.** Source: xxHash
0.8.x (XXH3), as vendored by zufast 0.1.0 (`zuf_hash64()`, `zuf_hasher_*`) and,
for the Rust oracle until it is retired, the `xxhash-rust` 0.8.19 crate (BSL-1.0).
The two agree bit for bit on every checksum field of the files rdz writes
(header, directory header, directory, block; checked 2026-10-05 against
`zufast::fast_hash()`). Measured on Apple arm64, R 4.6.1, one thread, 256 MiB of
random bytes: XXH3-64 18.6 GB/s; zlib's portable CRC32 (through `digest`)
0.77 GB/s. crc32fast's hardware path is faster than zlib's but needs a kernel per
architecture, which a C implementation would have to write or take from zlib;
zufast already carries XXH3 with run-time SIMD dispatch, and `qs2` uses the same
hash. CRC32's guaranteed detection of short burst errors is not a property a
storage format needs over a 64-bit hash, and the wider field lowers the chance of
an undetected corrupt block from 2^-32 to 2^-64. Cost: four more bytes in the
file header's checksum slot (which absorbed the old reserved word), eight more in
each block header, directory header and trailer, and eight more per block entry,
about 16 bytes per MiB block. Result: **adopt**; container version 3.

**Compression: zstd, vendored, as compression ID 1 — adopt the slot; LZ4 on
evidence only.** Source: Zstandard's single-file amalgamation at a pinned
release, vendored under `src/vendor/zstd/` with the family's manifest and
verification tooling, as `fst` and `zstdlite` do. The speed presets of
performance.md need zstd's negative and low levels; zukomp registers only the
DEFLATE family and has no zstd satellite scheduled. Block bytes are zstd frames
either way, so a later switch to a shared registration is not a format change.
Implemented at plan-c Stage D (zstd 1.5.7, vendored as one translation unit
without its own threads; tools/vendor/). Levels measured on 160 MB of doubles,
160 MB of integers and 5e6 short strings, one thread (Apple arm64): level 1
writes in 850/970/570 ms to 48.8/66.4/15.3 MB; level 3 takes 1.1 to 1.5 times
as long for sizes within 4% either way; level 6 takes 2.3 to 3.3 times as long
for files 5 to 7% smaller; level -1 saves about 5% of the time for files 1 to
35% larger. Result: **adopt**
level 1 as the `balanced` default and level 6 as `compact`; `speed` stores raw.
Per-type use beyond the generic codec remains gated on the benchmark matrix.

**The generic payload is streamed — adopt.** The C writer serializes through an
`R_outpstream` straight into blocks and reads through an `R_inpstream` from them,
so the whole-payload raw vector never exists. The bytes on disk are those of
`serialize(x, NULL, version = 3, xdr = TRUE)`, so this is not a format change.

## Candidate register

| Candidate | Applies to | Initial status | Roadmap gate |
|---|---|---|---|
| Two-bit packing | logical | adopt as reference | Phase 1 |
| RLE/bit-pack hybrid | logical, integer, factor | experiment | Phases 1, 2, 5 |
| Constant encoding | all atomic vectors | experiment | Per type |
| Byte shuffle | integer, numeric | experiment | Phases 2, 3 |
| Frame-of-reference/bit packing | integer, factor | experiment | Phases 2, 5 |
| Delta miniblocks | integer, string lengths | experiment | Phases 2, 4 |
| Byte-stream split | numeric | experiment | Phase 3 |
| ALP/ALP-RD with exact exceptions | numeric | experiment after raw/shuffle reference; define RDZ-owned wire record | Phase 3/tuning |
| Gorilla/Chimp/Patas family | numeric sequences | defer until reference codec | Phase 3/tuning |
| Offsets plus concatenated bytes | character | adopt as reference candidate | Phase 4 |
| Whole-string dictionary | character, factor levels | experiment | Phases 4, 5 |
| Prefix/delta strings | character | experiment | Phase 4 |
| FSST | character | defer until dictionary baseline | Phase 4/tuning |
| Zstandard per block (compression ID 1) | all leaf blocks | adopt (2026-10-05): generic codec, levels 1 and 6 | plan-c Stage D; per type thereafter |
| LZ4 per block | all leaf blocks | evidence only | tuning |
| Filter pipeline metadata | fixed-width blocks | adopt minimal form | Phase 0 |
| Footer block/column index | container, data frame | adopt minimal form | Phases 0, 7 |
| Whole-root R serialization XDR stream | rare/complex objects | adopt for coverage | Phase 0/tuning |
| Reference/back-reference table | lists | research required | Phase 6 |

“Adopt” in this table means adopt the architectural slot or simple reference
representation, not freeze its exact bytes before the roadmap completion gate.

## Dependency and implementation research

Before adding a codec crate or native library, document:

- exact crate and transitive dependency versions;
- Rust 1.88 compatibility and supported targets;
- scalar fallback and runtime SIMD detection;
- panic/unsafe behavior on malformed input;
- license, copyright attribution, and CRAN vendoring size;
- native static libraries and required linker flags;
- maintenance activity and format stability;
- whether the encoded bytes are standardized or implementation-version-dependent.

Prefer small, independently testable pure-Rust building blocks when their
performance is competitive. Do not reimplement a complex published codec merely
to avoid a dependency unless the reduced scope is explicit and thoroughly tested.

## Research execution policy

Research is just-in-time and time-boxed:

1. Phase 0 settles framing, blocks, and raw fallback. Character feasibility is a
   just-in-time gate at the start of Phase 4, before its record is frozen.
2. Each leaf-type phase implements a simple correct reference codec first.
3. Candidate microbenchmarks identify promising transforms.
4. Only promising candidates enter end-to-end R benchmarks.
5. Adopted candidates receive encoding IDs, malformed-input tests, and fixtures.
6. Rejected results remain briefly documented so the same experiment is not
   repeated without new evidence.

Do not release 0.1.0 while a high-impact alternative could force incompatible
metadata or block-layout changes. Conversely, research must not indefinitely
delay a correct reference codec: reserve extension points, use the simpler
experimental representation before 0.1.0, and revisit deferred algorithms before
the 0.1.0 format freeze.

## Related documents

- [Architecture](architecture.md)
- [Performance design](performance.md)
- [Implementation roadmap](roadmap.md)
- [Validation and benchmarking](validation.md)
- [R SEXP coverage matrix](sexp-coverage.md)
- [Cross-OS portability](portability.md)
- [Metadata and selective attribute access](metadata-access.md)
- [Direct R competitor research](research.md)
