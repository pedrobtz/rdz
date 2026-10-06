# RDZ Performance Design

> **Historical.** This document records performance design and measurements from the Rust implementation (crc32fast, its block sizes and presets). The presets now differ only in the zstd level (`speed`: none, `balanced`: 1, `compact`: 6), and `speed` never writes the decimal double encoding. Current measurements are in `tools/benchmark.R` (see `.github/CONTRIBUTING.md`). Where it disagrees with the code, the code and [architecture.md](architecture.md) are right.

## Performance objective

RDZ covers every object handled by base R serialization while targeting much
faster native encoding for common vectors, lists, and data frames. The primary
competitors are `qs2`, `qdata`, and base R serialization. `fst` is the data-frame
performance target for complete writes and reads, but its selective row/column
access solves a broader storage problem and must be benchmarked and described
separately.

Cross-OS readability is invariant, not a benchmark preset. RDZ does not trade it
away for host-native integer order, struct dumps, or native-order R serialization.
Measure the portable representation defined by
[portability.md](portability.md); optimize conversion with bulk/SIMD operations
without changing file bytes.

Performance means a Pareto balance of:

- write throughput;
- read throughput;
- encoded size;
- peak temporary memory;
- scaling across threads;
- exact R round-trip fidelity.

A speed claim is not valid if it compares materially different checksumming,
compression ratios, object semantics, or thread counts. See
[validation.md](validation.md) for the benchmark protocol and
[research.md](research.md) for competitor details. Candidate algorithms from
Arrow, Parquet, ORC, Blosc2, DuckDB, and published numeric/string work are tracked
with adoption gates in [encoding-research.md](encoding-research.md).
The native/generic handling expectation for each R runtime type is defined in
[sexp-coverage.md](sexp-coverage.md); benchmarks must report which payload codec
was selected rather than combining the paths.

## Block-oriented vectors

Use Arrow-style contiguous typed buffers combined with independently compressed
blocks. Blocks belong to leaf vectors rather than arbitrary ranges of the whole
object:

```text
data.frame
|- integer column
|  |- block 0
|  |- block 1
|  `- block 2
|- character column
|  |- block 0
|  `- block 1
`- factor column
   |- levels
   |- code block 0
   `- code block 1
```

A block record needs enough information to decode independently:

```text
element range or element count
physical encoding ID
compression codec ID
uncompressed byte count
compressed byte count
optional block checksum
compressed payload
```

Independent blocks provide bounded memory, parallel compression and
decompression, early corruption detection, codec selection per block, and a path
to future partial reads.

Start by benchmarking uncompressed target sizes from 256 KiB through 1 MiB. The
chosen policy should produce at least two to four blocks per worker for large
vectors, without splitting small vectors merely to create tasks. Character blocks
should target bytes while retaining element boundaries.

## Bounded encode pipeline

```text
R producer thread             compression workers            ordered writer
-----------------             -------------------            --------------
inspect SEXP
copy/pack owned block ------> transform
produce next block            compress
                              checksum
                        ----> completed block -------------> reorder by sequence
                                                            write block
```

The R producer is the only stage that touches SEXPs or invokes the R API. A worker
receives an owned buffer and immutable metadata. A dedicated writer may write on
another thread because it owns only Rust buffers and the file handle.

Assign monotonically increasing sequence numbers before work submission. The
writer restores deterministic stream order with a bounded reorder buffer. Queue
capacity should be a small multiple of worker count so the pipeline overlaps R
packing, compression, and disk writes without accumulating the object in memory.

All stages need cooperative cancellation and first-error propagation. On error,
stop accepting work, join workers, close the temporary file, and remove it.

For uncompressed or small vectors, bypass the pipeline and synchronously write
borrowed contiguous data when safe. Copying merely to obtain parallelism can make
the fast path slower.

## Bounded decode pipeline

```text
ordered reader -> decompression workers -> ordered owned blocks -> R consumer
```

The reader validates block headers before allocating compressed buffers. Workers
decompress and reverse physical transforms into owned Rust buffers. The R thread
allocates output vectors and copies completed blocks into their validated ranges.
Do not let workers write into R-owned memory.

The final R object necessarily consumes memory; the design goal is to avoid a
second complete encoded or decoded object. Peak temporary memory should scale
approximately with `worker_count * block_size * bounded_queue_factor`.

## Type-specific physical encodings

### Logical

- Reference decoder: two bits per value for false, true, missing, and reserved.
- Implemented speed path: fused scalar/AVX2/NEON tri-state classification and
  exact selection among constant, modal bitplanes, sparse patches, run ends, and
  short periodic patterns in 65,536-value blocks.
- Writes use one bounded buffered stream with tracked offsets; readers validate
  sequential blocks into one reusable encoded buffer and expand dense planes
  with AVX2 where available.
- General compression remains evidence-gated because specialized records already
  win tested file sizes; evaluate LZ4 only against the remaining complete-path
  write gaps.

### Integer

- Baseline: little-endian `i32`.
- Candidates: byte shuffle, frame-of-reference plus bit packing, delta plus
  zigzag, and run-length encoding.
- Select using cheap block statistics; never run every candidate on ordinary
  speed presets.

### Double

- Baseline: exact little-endian `u64` bits.
- First candidate: eight-byte shuffle before compression.
- Later candidates: ALP/ALP-RD for decimal-like or common-prefix blocks and XOR
  or delta encodings for suitable ordered series.
- Transform selection must never canonicalize NaNs or lose negative zero.
- ALP selection includes its sampling, exact-exception, and metadata costs; use it
  only when its complete stored block beats the raw/shuffled alternatives under
  the active preset.

### Character

- Baseline: offsets or lengths, encoding/NA metadata, and concatenated bytes.
- Candidate: dictionary plus packed indices for repeated or low-cardinality data.
- Dictionary identity is bytes plus encoding. CHARSXP pointer identity may be used
  as an in-process optimization while building the dictionary, never persisted.
- Compress offsets/indices separately from string bytes when benchmarks justify
  it.

### Factor

- Write levels once.
- Pack codes to the minimum safe bit width while reserving missing values.
- Compress code blocks only when it improves the selected preset.

### Lists and data frames

Structural records stay ordered and lightweight. Schedule large leaf vectors as
block work. Data-frame columns naturally provide coarse independent work, while
blocks within a wide column provide sufficient parallelism for narrow tables.

## Compression presets

The format records the actual transform and codec for every block. User presets
select policies, not implicit format meanings. An initial policy family is:

| Preset | Physical transforms | Compression |
|---|---|---|
| `speed` | mandatory packing and cheap transforms | raw (LZ4 only on evidence) |
| `balanced` | sampled shuffle/dictionary/packing | Zstandard level 1 |
| `compact` | broader transform selection | Zstandard level 6 |

Implemented at plan-c Stage D as `options(rdz.preset = )`, with
`options(rdz.threads = )` for the pool (default 1); the levels are those of
encoding-research.md's 2026-10-05 measurements.

Exact codecs and levels are benchmark decisions. Keep the block format open to
mixed codecs so incompressible blocks can remain raw and highly compressible
blocks can use a stronger codec.

## Threading policy

Parallelism is useful only when the work amortizes copies, scheduling, and buffer
management. Introduce configurable thresholds based on uncompressed bytes and
block count. A one-thread path must remain first-class and deterministic.

Respect the requested thread limit and CRAN constraints. Avoid uncontrolled
nested parallelism when the compressor also creates threads. Prefer one RDZ-owned
pool whose workers call single-threaded block compressors.

Tune the pool after integer and double codecs exist. Logical vectors validate
block plumbing but their two-bit encoding may be too cheap to represent realistic
worker scheduling costs.

## IO and container finalization

Normal writes go directly to a buffered temporary file. `encode_to_vec()` remains
useful for pure tests, fixtures, and a low-level raw API, but must not be the
production file path.

Keep payload generation sequential so the same codec can target any `Write`.
Place final length, checksum, and indexes in a footer where possible. A seekable
file wrapper may patch a front header, but the core encoder must not require
seeking.

The writer records block and optional column offsets while writing. A footer index
is required from the first native codec for metadata/schema inspection and
selective supported-attribute reads. It also leaves room for later selective
data-frame reads.

## Metadata-only and attribute reads

Implement [metadata-access.md](metadata-access.md) with a fixed trailer and a
checksummed object/attribute/block directory. A metadata-only reader seeks to the
trailer and reads only the bounded directory. Selected attribute values fetch only
their referenced blocks. Large attributes remain proportional to their own size,
but unrelated vector and column blocks must not be touched.

Track actual bytes read and decompressed in benchmarks, not latency alone. For a
large vector, `class`/`dim` inspection should be effectively independent of the
vector payload size. For a wide data frame, schema inspection must not allocate
column vectors. Keep the directory compact enough that ordinary full reads do not
pay a material regression.

Generic XDR fallback supports a bounded root synopsis only. Exact arbitrary
attribute access requires full R unserialization and must be reported as such,
not hidden behind a metadata API.

## Generic R-serialization fallback

The generic codec is a performance path, not merely a correctness escape hatch.
It should stream one whole-root XDR `R_Serialize()` byte stream into bounded RDZ
blocks, compress those blocks in the same worker pipeline, and feed decompressed
bytes to `R_Unserialize()` during reads. It must not first allocate the complete R
serialization as a raw vector.

This design follows the useful part of `qs2`: retain R's graph and rare-object
semantics while improving compression, checksums, and disk IO. Type-specific RDZ
transforms apply only to native payloads, but the generic stream may still use
raw-versus-compressed block selection and a benchmarked byte-shuffle policy.

Automatic mode may waste work if native encoding discovers an unsupported value
late. Measure optimistic native attempt plus restart against a capability prepass
on mixed object graphs. Prefer the optimistic path for common supported values
unless profiles show that a reusable capability/encoding plan pays for its extra
traversal.

Report native and generic results separately. Native benchmarks establish RDZ's
specialized performance; forced-generic benchmarks ensure closures, environments,
and other fallback values do not suffer avoidable allocation or IO regressions.

## Checksums

Compute checksums over the exact stored representation so validation does not
require allocating the uncompressed object. Per-block checksums improve failure
locality and parallel calculation; a footer checksum protects structural
metadata.

The generic fallback and native container use `crc32fast` 1.5.0 for IEEE CRC32. It
selects an optimized implementation for the current CPU at runtime, supports
incremental block updates, and retains a portable scalar fallback. Validation
borrows the payload and returns only success or failure; it must never allocate a
second complete payload. The dependency, its transitive `cfg-if` crate, licenses,
and sources are included in the CRAN vendor archive.

The first native container should apply the same principle to its structural and
optional per-block checksums: update the checksum while bytes already pass through
the writer/reader, and do not add a separate whole-payload memory pass merely for
validation.

Checksumming must be included consistently in competitor benchmarks. CRC32 and
XXH-style hashes detect accidental corruption; they are not cryptographic
authentication.

## Optimization order

Optimize in this order:

1. Remove whole-payload allocations with direct IO.
2. Use contiguous bulk reads/writes and compact mandatory representations.
3. Establish block framing and reusable buffer pools.
4. Add cheap type-specific transforms.
5. Add the bounded parallel pipeline.
6. Tune block sizes, thresholds, and compression policies on the benchmark suite.
7. Investigate ALTREP, random access, and advanced codecs only after end-to-end
   profiles identify them as valuable.

Every optimization must retain a simpler reference path or fixture-based
cross-check so performance work cannot silently change format semantics.

Do not add a physical encoding solely because it is successful in another
format. Record its experiment and decision in
[encoding-research.md](encoding-research.md), then retain it only after the
end-to-end R benchmark and semantic gates in [validation.md](validation.md).

## Related documents

- [Current-state assessment](current-state.md)
- [Architecture](architecture.md)
- [Implementation roadmap](roadmap.md)
- [Validation and benchmarking](validation.md)
- [R SEXP coverage matrix](sexp-coverage.md)
- [Cross-OS portability](portability.md)
- [Metadata and selective attribute access](metadata-access.md)
- [Encoding and format research](encoding-research.md)
- [Direct R competitor research](research.md)
