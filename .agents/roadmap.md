# RDZ Native Serialization Roadmap

> Since 2026-10-05 the sequence is [plan-c.md](plan-c.md), the re-implementation
> in C. Its stages map onto the phases below: the container and generic codec
> (Phase 0B, the streaming bridge of Phase 8) are Stages B and C; compression
> and the pipeline are Stage D; Phase 1 (logical) is Stage E; Phases 2 and 3
> (integer, double) Stage F; Phases 4 and 5 (character, factor) Stage G;
> Phases 6 and 7 (list, data frame) Stage H; Phase 8 (tuning, the 0.1.0
> freeze) Stage I. Each phase's deliverables and completion gate below still
> apply to its stage.

## Development method

Implement one complete vertical slice at a time. A type is not complete when it
can merely write bytes; it is complete when its format, direct file IO, decoding,
validation, R round trips, malformed-input behavior, fixtures, and benchmarks are
all in place.

The intended type order is:

1. logical
2. integer
3. numeric (double)
4. character
5. factor
6. list
7. data frame

Factor follows integer and character because it uses both. Lists introduce
recursion, and data frames are the final common composition of the supported leaf
types.

The public design covers every object supported by base R serialization. Use an
explicit experimental native mode during development; unsupported types must
error in that mode. Automatic mode uses native encoding only when the complete
root is supported and otherwise writes the complete root through the generic R
serialization codec. Do not embed separately serialized fallback fragments inside
native lists because that would break graph semantics across codec boundaries.

## Completion gate for every type

Each type-specific milestone must include:

- documented logical and physical representation;
- pure Rust encode/decode tests;
- public R API round trips using `expect_identical()`;
- automatic-mode coverage and strict-native rejection where the object is not
  natively supported;
- missing-value and supported-attribute coverage;
- proof that every non-supported attribute triggers whole-root fallback in
  automatic mode rather than being stripped;
- empty, scalar, block-boundary, and multiblock cases;
- malformed, truncated, oversized, and corrupt input cases;
- temporary-file cleanup checks;
- deterministic golden fixture once the representation is stable;
- metadata/schema inspection and selected supported-attribute reads that prove
  unrelated data blocks were not decoded;
- end-to-end read/write/size benchmark against applicable competitors;
- single-thread results and multithread results when the work is parallelized.

See [validation.md](validation.md) for the required matrices.

## Current checkpoint

The dated implementation assessment is maintained in
[current-state.md](current-state.md). As of the current checkpoint, RDZ has a
correct transitional generic container, optimized full-payload CRC32, broad
base-R round-trip coverage, CRAN-oriented Rust packaging, the Phase 0B block and
directory frame, bounded metadata inspection, and a reproducible benchmark
matrix. Phase 1 adds a supported logical codec, populated object/attribute
tables, and selective names access. It does not yet have an integer codec,
compression, a parallel pipeline, or a streaming generic bridge.

The benchmark evidence changes the immediate emphasis:

- the current RDZ payload is the uncompressed R serialization stream plus the
  version-2 block headers, bounded synopsis/directory, and closing trailer;
- base R without compression is faster for simple atomic payloads, showing the
  cost of the extra raw allocation, IO pass, and mandatory CRC scan;
- buffered raw unserialization is already competitive for some character and
  composed values, but RDZ remains materially larger than compressed formats;
- existing measurements are read-latency diagnostics, not yet competitive
  claims, because write time, peak memory, cold-cache behavior, and matched-size
  modes are incomplete.

## Phase 0: lock the measurement contract and build the native frame

Phase 0 is split so foundational work is visible and the first native vertical
slice is not blocked by unrelated pre-release optimizations.

### Completed foundation

- Pre-0.1.0 files are explicitly disposable and ordinary RDS files are not
  accepted as RDZ containers.
- The transitional file has fixed little-endian magic, version, flags, length,
  and IEEE CRC32 fields.
- `crc32fast` replaced the bit-at-a-time checksum, and validation no longer
  creates a second complete payload copy.
- Rust writes framed blocks directly to a same-directory temporary file, closes
  it, preserves an existing destination's permissions, and replaces the
  destination with cleanup on failure.
- Generic R serialization version 3 currently supplies broad semantic coverage,
  including closures, environments, cycles, shared references, attributes, and
  character encodings.
- Pure Rust header/checksum tests, R corruption tests, public round trips, a
  multi-OS R-CMD-check matrix, and vendored Rust dependencies are present.
- The read matrix covers RDZ, qs2, qdata, fst, default compressed base R, and
  uncompressed base R. Its default thread budget is one and checksum validation
  is enabled where the competitor exposes it.

### Phase 0A: finish the benchmark contract

Status: implemented on 2026-08-08. Keep this contract as a regression gate and
extend datasets or metrics when evidence requires it; Phase 2 is now active.

- Extend the matrix from read latency to paired read/write latency, throughput,
  file size, and an allocation or peak-memory measure.
- Benchmark at least small-latency, medium, and throughput-oriented sizes; record
  warmup policy and distinguish warm-cache from cold-cache runs.
- Record package versions, presets, checksum settings, thread budgets, CPU/OS,
  and object-generation parameters in machine-readable result metadata.
- Keep the default comparison single-threaded. Add a separate equal-budget
  scaling matrix only after RDZ has parallel work.
- Report fst atomic-vector results as the one-column `as.data.frame()` adapter,
  not as arbitrary-object fst support.
- Retain both compressed and uncompressed base-R baselines. Compare qs2/qdata
  with checksum validation enabled; checksum-disabled results may be shown only
  as a separately labeled package-default diagnostic.
- Add dataset families from [validation.md](validation.md) rather than relying on
  one representative value distribution per type.

### Phase 0B: define and implement the native frame skeleton

Status: implemented on 2026-08-08. The exact pre-release layout is recorded in
[container-format.md](container-format.md). Phase 2 is now the active milestone.

- Define container version, distinct native and R-serialization codec IDs,
  codec versions, tags, flags, block headers, and footer finalization rules.
- Define the fixed closing trailer and the minimal versioned
  object/attribute/block directory required by
  [metadata-access.md](metadata-access.md). Reserve a bounded, independently
  checksummed synopsis for generic fallback payloads.
- Specify every wire field's width, byte order, bit order, padding, and overflow
  behavior according to [portability.md](portability.md).
- Define `auto`, `native`, and forced-`r` behavior. Only the distinguished
  unsupported-native outcome may trigger automatic fallback.
- Add checked Rust reader/writer primitives, bounded trailer/directory discovery,
  direct temporary-file IO, and cleanup guards independent of R.
- Implement thin SEXP-aware dispatch and native eligibility checks on the R
  thread while keeping format, codec, and IO modules independent of Savvy.
- Decide the initial policy for unsupported attributes, reference sharing,
  cycles, and ALTREP values. Do not silently strip or fragment-fallback any value.
- Reserve encoding/compression IDs without freezing unmeasured algorithms. The
  first native blocks may be raw or obligatorily packed; adaptive compression is
  admitted only by benchmark evidence.
- Keep the current allocation-based whole-root R payload as a clearly tagged
  transitional generic codec while Phase 1 is developed. Make XDR selection
  explicit and do not freeze this payload layout as the 0.1.0 generic codec.
- Expose bounded container/codec information without decoding a payload. The
  authoritative native schema and selective attribute API are completed by the
  Phase 1 vertical slice.

### Work deliberately moved out of the Phase 1 entry gate

- The callback bridge that streams `R_Serialize()`/`R_Unserialize()` through
  bounded blocks is required before 0.1.0, but should reuse the block pipeline
  after it exists rather than block the first native logical codec.
- Exact character byte/encoding extraction is the first feasibility gate of
  Phase 4, before the character wire representation is frozen.
- The `data.table::.internal.selfref` transient-pointer registry belongs to the
  Phase 7 data-frame/class adapter, where its semantics can be tested end to end.
- Compression presets and multithreading follow correct raw/packed block paths;
  they are not prerequisites for proving dispatch and framing.

### Exit criteria

Phase 1 may start when the benchmark contract is reproducible, `auto`/`native`/`r`
selection is testable, the native frame can write and validate bounded raw or
packed blocks through direct Rust IO, and adding logical records does not require
redesigning codec identification, trailer discovery, or directory bounds.
Representative rare objects must continue to round-trip through one explicitly
tagged whole-root XDR payload. Streaming that generic payload and compressing it
remain mandatory 0.1.0 gates, but are no longer Phase 1 entry gates.

## Phase 1: logical vectors

Status: correctness vertical slice and first adaptive performance follow-up
implemented on 2026-08-09. The writer now uses SIMD-classified tri-state planes
and exact per-block selection among constant, dense, sparse, run, and short
periodic records; the old two-bit path remains a decoder/reference codec.

The implementation bake-off selected 65,536-value blocks. Complete writes now
beat fst defaults for random, no-NA, and mostly-missing inputs, remain close for
sparse input, and still lose for long-run and alternating inputs. Specialized
reads win four of six cases and file size wins all six. The strict all-case write
gate therefore remains a tracked optimization target rather than a completed
claim.

### Deliverables

- Complete the first vertical slice of `rdz_info()`, native schema inspection,
  and selective reads of supported logical-vector attributes without reading the
  logical data blocks.
- Class/attribute validation for ordinary logical vectors.
- Raw logical access that preserves `NA_LOGICAL`.
- Two-bit decoder/reference encoding with a reserved fourth state.
- Names support.
- Single and multiblock direct file writes and reads.
- Runtime-dispatched scalar, AVX2, and NEON bitplane classification, plus AVX2
  dense expansion and a portable scalar decoder.
- Constant, dense-plane, sparse-patch, run-end, and short-period modes.
- Evidence-gated LZ4 remains deliberately unimplemented: a future experiment
  must demonstrate a complete write/read benefit rather than merely shrinking
  dense bytes.
- Optional five-trit dense experiment for the compact preset.
- Compare the logical candidates listed in
  [encoding research](encoding-research.md#logical-vectors), retaining two-bit
  packing as the simple reference path.

### Purpose

Logical vectors prove the complete R-to-file-to-R path, block framing, footer,
checksums, and error cleanup. They are not representative evidence for overall
performance because two-bit packing makes their codec unusually cheap.

### Performance follow-up before Phase 2

Treat fst's word-at-a-time logical packer as the baseline, not the architecture.
The implemented follow-up exploits R's three exact `i32` states to fuse SIMD
classification, validation, bitplane generation, and selector statistics. Keep
the two-bit decoder as a reference. Before 0.1.0, close or explicitly waive the
remaining long-run/periodic write gap using complete-path evidence, and verify
canonical classifier bytes on both x86-64 and AArch64 CI.

## Phase 2: integer vectors

### Deliverables

- Exact `i32`/`NA_INTEGER` baseline.
- Byte-shuffle, bit-pack, delta, and run-length candidates behind explicit block
  encoding IDs.
- Cheap selection heuristic backed by benchmark datasets.
- Checked conversion of R lengths and file lengths.
- Evaluate the integer candidate set in
  [encoding research](encoding-research.md#integer-vectors) without requiring all
  modes to enter the stable format.

### Exit criteria

The baseline path is byte-stable and all retained transforms beat or complement
it on a documented part of the benchmark suite without pathological regressions.

## Phase 3: numeric (double) vectors

### Deliverables

- Bit-exact `f64` representation.
- Tests for R missing doubles, ordinary NaNs, infinities, negative zero, and
  unusual NaN payloads.
- Eight-byte shuffle experiment and raw fallback.
- Prototype ALP/ALP-RD as an independently decodable RDZ-owned block encoding
  after the raw/shuffle baseline. Preserve exceptional values through exact
  `u64` bit-pattern side channels and keep raw fallback.
- Initial bounded worker pool and ordered writer, now that integer and double
  blocks provide enough work for meaningful scheduling measurements.
- Configurable one-thread and multithread paths.
- Evaluate byte-stream split first, then benchmark ALP/ALP-RD and XOR-family
  codecs against the raw and shuffled reference paths on the
  [numeric matrix](encoding-research.md#numeric-vectors). ALP adoption requires
  the audit and portability gates in
  [encoding research](encoding-research.md#alp-assessment-for-rdz).

### Exit criteria

Parallel results demonstrate useful end-to-end scaling on sufficiently large
vectors without regressing small-vector latency or exceeding the memory bound.

## Phase 4: character vectors

### Deliverables

- Before freezing the character record, prototype exact extraction and
  reconstruction of bytes plus R encoding tags through public R APIs. Stop and
  revise the representation if mixed `CE_NATIVE`, `CE_UTF8`, `CE_LATIN1`, and
  `CE_BYTES` values cannot meet [portability.md](portability.md).
- R API adapter for NA, bytes, and encoding tags.
- Exact round trips for ASCII, UTF-8, Latin-1, bytes, and native encodings.
- Source-native encoding metadata.
- Cross-locale conversion tests: exact explicit-encoding/byte preservation and
  native-source-to-UTF-8 conversion when the target locale differs.
- Baseline offsets/lengths plus concatenated byte payload.
- Adaptive dictionary encoding for repeated and low-cardinality strings.
- Variable-byte block construction with stable element ranges.
- Parallel compression after all R strings have been packed into owned buffers.
- Evaluate dictionary, delta-length, prefix, and FSST candidates in the order and
  under the gates described in
  [character research](encoding-research.md#character-vectors).

### Exit criteria

Character identity and encoding tests pass across supported locales, high- and
low-cardinality benchmarks are covered, and memory remains bounded for long
strings and large vectors.

## Phase 5: factors

### Deliverables

- Factor detection before integer dispatch.
- Levels, codes, names, class, and ordered-state encoding.
- Code-range validation.
- Minimum-width or bit-packed codes with explicit missing representation.
- Tests for empty levels, unused levels, missing codes, ordered factors, and
  malformed codes.

## Phase 6: lists

### Deliverables

- Recursive records with optional names.
- Nesting and total-allocation limits.
- Empty, deeply nested, and heterogeneous supported lists.
- Defined reference/back-reference behavior and tests.
- Deterministic scheduling of large leaf vectors without moving SEXPs to workers.
- A distinguished unsupported-native result for unsupported children,
  attributes, sharing, or cycles; automatic mode restarts the complete root with
  R serialization, while strict native mode reports the error.

### Exit criteria

The native decoder cannot recurse or allocate without configured bounds,
supported sharing semantics round trip, and unsupported graph structures select
whole-root fallback in automatic writes or fail clearly in strict native mode
rather than hanging or overflowing.

## Phase 7: data frames

### Deliverables

- Data-frame detection before list dispatch.
- Column names, exact supported row names, class, row count, and columns.
- Equal-column-length validation.
- Zero-row, zero-column, duplicate-name, empty-name, and mixed-column tests.
- Parallel scheduling across columns and within large columns.
- Footer offsets sufficient for future column selection.
- Full data-frame benchmarks against `qdata`, `qs2`, and `fst`.
- Implement the class-and-attribute-keyed transient external-pointer registry
  with `data.table::.internal.selfref` as its first entry. Preserve the
  `c("data.table", "data.frame")` class and supported table metadata, prove usable
  restored behavior, and prove that non-matching external-pointer attributes
  trigger whole-root fallback rather than being dropped.

### Exit criteria

All supported data-frame shapes round trip identically. Performance reports
separate full reads/writes from `fst` selective-access capabilities and show the
speed/size/memory Pareto frontier rather than a single favorable preset.

## Phase 8: tuning and the 0.1.0 format freeze

### Deliverables

- Profile end-to-end R calls, packing, transforms, compression, checksums, IO, and
  R allocation separately.
- Tune block sizes, queue bounds, small-input thresholds, buffer reuse, and codec
  policies on the full benchmark matrix.
- Tune the already optimized structural and per-block checksum implementations.
- Run malformed-input fuzzing and cross-platform fixtures.
- Exchange generated `.rdz` artifacts between Windows, macOS, and Linux CI jobs;
  test synthetic big-endian and narrow-address-space decode failures.
- Freeze and verify the native and generic fixtures that will ship with 0.1.0.
- Replace the transitional allocation-based generic payload with the callback
  bridge that streams one whole-root XDR `R_Serialize()`/`R_Unserialize()` stream
  through bounded RDZ blocks. Remove the complete raw-vector allocation and
  separate whole-payload integrity pass from the production generic path.
- Benchmark forced generic fallback against `qs2` so rare-object coverage does
  not retain avoidable whole-payload allocations.
- Document supported types, attributes, portability, and format compatibility.
- Decide when native-first automatic writing becomes the public default while
  retaining forced native and forced R modes.

### 0.1.0 compatibility rule

All pre-0.1.0 formats may change incompatibly without a compatibility reader.
Do not release 0.1.0 until reader limits, unsupported-type behavior, character
portability, reference policy, and fixtures are complete. Starting with 0.1.0,
incompatible byte-format changes require a new codec version and existing 0.1.0
files must remain readable.

## Later opportunities

These are explicitly outside the first native release and should be prioritized
from profiles and user needs:

- matrices and arrays;
- Date, POSIXct, difftime, integer64, and other classed vectors;
- general native attribute maps (all attributes are already preserved publicly
  through whole-root fallback);
- raw and complex vectors;
- selective data-frame column or row-range reads;
- zero-copy or lazy ALTREP character/numeric reads;
- additional integer, floating-point, or string codecs;
- authenticated or cryptographic integrity;
- broader dedicated native coverage to reduce how often automatic mode falls back
  to R serialization;
- a dedicated environment/reference graph codec, distinct from ordinary native
  list records, if profiling and user demand justify its complexity.

## Related documents

- [Current-state assessment](current-state.md)
- [Architecture](architecture.md)
- [Performance design](performance.md)
- [Validation and benchmarking](validation.md)
- [R SEXP coverage matrix](sexp-coverage.md)
- [Cross-OS portability](portability.md)
- [Metadata and selective attribute access](metadata-access.md)
- [Encoding and format research](encoding-research.md)
- [Competitor research](research.md)
