# rdz roadmap

Updated: 2026-07-04

## Product direction

rdz currently targets low-latency serialization of common R data objects. The
speed preset prioritizes latency and format stability; the balanced preset may
spend additional work only for exact or measured size reductions.

Becoming a full `saveRDS()` replacement is an open product decision, not an
adopted goal. Portability and API-completeness work below must be explicitly
accepted before it becomes a release commitment.

Implementation notes below reference functions rather than line numbers; all
writer/reader entry points live in [src/rdz.c](../src/rdz.c) unless stated.

## Committed priorities

### 1. Deterministic CPU-limit testing

- [ ] Separate portable cgroup parsing and limit-combination logic from Linux
  filesystem and affinity probes.
- [ ] Test cgroup v1 and v2 membership, parent/child quotas, fractional CPU
  limits, malformed files, and missing limits with temporary fixtures.
- [ ] Test combinations of online CPUs, affinity, and cgroup limits.
- [ ] Add a Linux container assertion that `--cpus=1.5` yields two available
  rdz threads.

Acceptance: fixture tests run on every platform, and the real Linux container
test confirms the production probe.

**Implementation notes**

- Refactor [src/threading.c](../src/threading.c) so every function that opens a
  hardcoded path takes explicit roots instead:
  `rdz_read_cgroup_path(unified, path, size)` gains a `proc_root` parameter
  (production passes `"/proc"`), and `rdz_cgroup_v1_threads` /
  `rdz_cgroup_v2_threads` gain a `sysfs_root` (production `"/sys/fs/cgroup"`).
  The pure pieces — `rdz_quota_threads` (ceil of quota/period),
  `rdz_minimum_positive`, and the `/proc/self/cgroup` line parser — need no
  filesystem at all once the file content flows through a parameter or a
  `FILE*` opened by the caller.
- Expose one **test-only probe entry**: `C_rdz_threads_probe(proc_root,
  sysfs_root, online, affinity)` registered in [src/init.c](../src/init.c) but
  not exported in `NAMESPACE`; tests call it via `.Call` on the internal
  symbol (`rdz:::` wrapper in `tests/testthat/helper-threads.R`). This keeps
  the production `rdz_resolve_threads()` byte-identical while making every
  branch reachable on macOS/Windows CI.
- Fixture layout (created with `withr::local_tempdir()`):

  ```
  <root>/proc/self/cgroup                      # membership lines, incl. malformed
  <root>/sys/fs/cgroup/cpu.max                 # v2: "150000 100000", "max 100000"
  <root>/sys/fs/cgroup/a/b/cpu.max             # v2 parent/child, child laxer than parent
  <root>/sys/fs/cgroup/cpu/a/cpu.cfs_quota_us  # v1 quota/period pairs
  ```

  Cases to cover: fractional limits (`150000/100000` → 2 threads via ceil),
  `max` (no limit), zero/negative quota, missing period file, membership line
  without the `cpu` controller, path longer than `RDZ_CGROUP_PATH_SIZE`, and
  the walk-to-root minimum when a parent is stricter than the child.
- Combination tests assert `min(online, affinity, cgroup)` semantics and the
  `RDZ_MAX_THREADS` (64) and `>= 1` clamps in `rdz_resolve_threads`.
- Container job: a manual GitHub Actions job (extend
  [.github/workflows/native-checks.yml](../.github/workflows/native-checks.yml)
  or a new `container-limits.yml`) that runs
  `docker run --cpus=1.5 rocker/r-ver:4.5 Rscript -e 'stopifnot(rdz::rdz_threads() == 2L)'`
  with the built package mounted. This is the only test that exercises the
  real `/sys/fs/cgroup` probe end to end.

### 2. Reader robustness

- [ ] Add a mutation harness that flips bytes in valid speed, balanced, and R
  fallback files.
- [ ] Run the harness under ASAN and UBSAN from the manual native-checks
  workflow.
- [ ] Assert malformed files either decode correctly or raise an R error—never
  crash, hang, or read beyond their bounds.

Acceptance: the corpus completes under sanitizers without native failures.

**Implementation notes**

- Corpus generator (`tools/fuzz/make-corpus.R`): one file per encoding tag so
  every decode branch is reachable. Minimum set — speed atomics (each
  `node_type`), dictionary strings at index widths 1/2/4 (the existing test
  cases construct these), flat raw strings, R fallback (a closure), and for
  balanced: packed logicals, packed integers with and without the NA code,
  integer sequence, numeric dictionary raw and `+LZ4`, numeric sequence,
  XOR+LZ4 (multi-block, ≥ 8193 values), flat strings + LZ4 with both RLE and
  direct length metadata. Keep each file small (< 512 KiB) so the harness
  stays fast.
- Mutation strategy (`tools/fuzz/fuzz-decode.R`):
  1. **Exhaustive first-256-byte sweep**: for each offset, three mutants
     (`0x00`, `0xFF`, `byte XOR 0x80`). This saturates magic, codec,
     architecture, node headers, and encoding tags.
  2. **Seeded random body flips**: N (default 2000) single-byte mutations at
     uniform offsets per file, seed fixed and printed so failures reproduce.
  3. **Truncations**: every prefix length ≤ 64, then 64 random lengths.
- Invariant per mutant: `tryCatch(read_rdz(f), error = identity)` must return
  *something* — a value (payload byte flips legitimately decode to different
  data) or an R error. Crashes, leaks, and OOB reads are the sanitizers' job;
  hangs are caught by wrapping each batch in `setTimeLimit(cpu = 30)` plus a
  shell-level `timeout` around the Rscript invocation.
- Wiring: gate on `RDZ_FUZZ=1`; add a step to the manual sanitizer jobs in
  `native-checks.yml` after the testthat run. Never part of the CRAN suite.
- Known gap to cover explicitly (from the 2026-07-02 review): no current test
  mangles the **architecture bytes** (offsets 9–11: endian, `sizeof(int)`,
  `sizeof(double)`) — the exhaustive header sweep covers this, but also add a
  focused testthat case asserting the "incompatible architecture" message so
  the contract is pinned outside the fuzz harness.

### 3. Test organization

- [ ] Split the large round-trip suite by concern: atomic values, strings,
  balanced numeric codecs, ALTREP, data frames, explain output, and malformed
  input.
- [ ] Keep common helpers small and place fixtures beside the tests that use
  them.

Acceptance: focused test filters identify one subsystem without running the
entire serializer suite.

**Implementation notes**

- Move `roundtrip()` into `tests/testthat/helper-roundtrip.R` (testthat
  auto-sources `helper-*.R`; no `source()` calls needed).
- Mechanical mapping of the existing `test-roundtrip.R` blocks:

  | New file | Blocks (by current `test_that` description) |
  |---|---|
  | `test-atomic.R` | native atomic vectors; attributes/matrices/lists/data.frames; auto fallback; codec enforcement |
  | `test-strings.R` | dictionary index widths; flat LZ4 selection; encodings + block boundaries |
  | `test-balanced-logical.R` | two-bit packing; packing inside data frames |
  | `test-balanced-integer.R` | small-range offsets; NA code reservation; every bit width; wide-range direct; factors |
  | `test-balanced-numeric.R` | dictionary; bit widths; random indexes; high-cardinality direct; sequences; XOR (block boundaries, rejection cases) |
  | `test-altrep.R` | streamed speed layout; balanced constant-delta; short sequences; byte-identity with materialized; tibble columns |
  | `test-data-frames.R` | tibbles; grouped tibbles; data.table (keyed/unkeyed/nested) |
  | `test-explain.R` | plan strategies; fallback/forced codecs; nested paths |
  | `test-malformed.R` | every "malformed … fail cleanly" block; invalid/truncated files |
  | `test-threads.R` | worker-thread count (grows with priority 1) |

- Pure moves — no assertion changes in the same commit, so the diff is
  reviewable as relocation only. `devtools::test(filter = "balanced-numeric")`
  becomes the subsystem filter.
- The byte-offset-poking helpers in the malformed tests (header index
  arithmetic like `block_header <- 46L + ...`) should gain named constants in
  the helper file (`HEADER_SIZE <- 26L`, etc.) so a future format change
  breaks one definition, not thirty magic numbers.

## Decisions required before implementation

| Decision | Current behavior | Question to resolve |
|---|---|---|
| Cross-architecture portability | Files record and require matching architecture. | Remain a local-cache format, or define a canonical wire representation? |
| Raw vectors and connections | Public APIs accept file paths. | Is parity with `serialize()` and connection-based RDS workflows a product goal? |
| Fallback granularity | One unsupported node sends the whole object to R serialization. | Is mixed native/R encoding worth the format and reference-semantics complexity? |
| Integrity checking | Structural corruption is validated; payload checksums are absent. | Should balanced files optionally carry a checksum? |

Each accepted decision should get a short design document covering format
compatibility, failure behavior, tests, and benchmark impact. Historical review
arguments are available under [archive](archive/) but are not decisions.

The sketches below are seeds for those design documents — enough detail to
estimate cost and compatibility, not commitments.

### Design sketch: cross-architecture portability

Portability does not require paying for it on the hot path. The rds/XDR
approach is slow because XDR is big-endian-canonical and every modern machine
swaps; the modern approach (fst, qs2, Arrow, Parquet) is
**little-endian-canonical**: LE hosts keep today's raw `memcpy`, only
big-endian hosts (s390x, in practice) swap. R already guarantees 32-bit `int`
and IEEE-754 64-bit `double`, so endianness is the only live variable — and
the header already records it, making swap-on-read backward compatible with
every existing file. No new magic tags.

- Detect at compile time (`__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__`) into an
  `RDZ_HOST_BIG_ENDIAN` macro; on LE builds all swap code compiles away.
- Reader: centralize swaps in `reader_u16/u32/u64` and add element-width swaps
  after payload copies (`reader_payload` callers know the element size). The
  balanced codecs already read byte streams (bit-packed data is
  endian-neutral); only the u16/u32/u64 fields and raw f64/i32 payloads swap.
- Writer: mirror in `writer_u16/u32/u64` and a swapping variant of the payload
  `writer_write` (BE hosts only; they pay for their own rarity).
- R fallback: today `save_body` uses `R_pstream_binary_format` (host-endian,
  not portable, and R offers no swap-on-read for it). Add codec byte
  `RDZ_CODEC_R_XDR = 3` using `R_pstream_xdr_format` for **new** files; keep
  accepting legacy codec 2 when the architecture matches. Fallback is already
  the slow path, so XDR's swap cost lands where latency matters least. Old
  readers reject new fallback files with the existing "unknown rdz codec"
  error — clean forward incompatibility, NEWS note required.
- Relax `read_body`'s check: endian mismatch becomes a swap trigger, not an
  error; `sizeof` mismatches still reject (they cannot occur on supported
  platforms).
- Tests: check in two tiny hand-byte-swapped fixture files (one per format
  version) and assert they decode on LE CI; benchmark CI asserts zero LE
  regression. Optional qemu-s390x job if `pedrobtz/r-actions` grows one.

### Design sketch: raw vectors and connections

- The writer already funnels every byte through `writer_write(writer_t*, ...)`;
  generalize the sink: keep `FILE *file` and add `byte_buffer_t *memory` —
  exactly one is non-NULL, `writer_write` branches once. No call-site changes.
- `C_rdz_serialize(object, mode, preset)` → run the existing `save_body`
  against the memory sink, then copy into `Rf_allocVector(RAWSXP, size)`.
  `serialize_rdz()`/`unserialize_rdz()` become thin R wrappers alongside the
  existing four exports.
- `C_rdz_unserialize(raw)` → build `reader_t { data = RAW(x), size, fd = -1 }`
  and reuse `read_body` unchanged. **Pitfall:** `reader_payload` calls
  `pread(reader->fd, ...)` for payloads ≥ 64 KiB on POSIX; that branch must be
  guarded with `reader->fd >= 0` or the raw path fails on any large vector.
- Connections: phase one is R-level only —
  `writeBin(serialize_rdz(x), con)` / `unserialize_rdz(readBin(con, "raw", n))`
  — documented as buffering the whole object in memory. Native streaming
  decode is out of scope; `mmap` stays the file fast path.
- Tests: byte-identity between `serialize_rdz(x)` and the file written by
  `write_rdz(x)` for the whole corpus; a > 64 KiB numeric vector through the
  raw path (regression for the `pread` pitfall).

### Design sketch: fallback granularity

- New node type `NODE_FALLBACK = 8`, only in files written by versions that
  have it (old readers reject unknown node types cleanly — the existing
  `decode_node` default case). Wire layout:
  `u8 type=8 | u64 byte_length | <R serialization bytes>`.
- Encoding: when `encode_node` meets an unsupported child, serialize that
  subtree with `R_Serialize` into a `byte_buffer_t` first (length is unknown
  until done), then emit length + bytes. Memory cost is bounded by the
  fallback subtree, not the whole object. A seek-and-backpatch variant avoids
  the buffer but complicates the memory sink; start with the buffer.
- Support checking moves from the current whole-tree pre-scan
  (`native_supported` in `C_rdz_save`) to a per-node check during encoding.
  `codec = "native"` (strict) keeps its contract by erroring at the first
  fallback emission; `codec = "auto"` emits mixed files.
- Decoding: `case NODE_FALLBACK` wraps the existing `r_in_char`/`r_in_bytes`
  stream over a **length-bounded sub-reader** so a corrupt length cannot let
  `R_Unserialize` read past the node.
- Reference semantics must be documented precisely: sharing *within* one
  fallback subtree is preserved (R's serializer handles it); sharing *across*
  two fallback subtrees, or between native and fallback nodes, is duplicated.
  The cycle-detection stack still guards the native spine.
- `explain_rdz` gains per-node fallback rows ("R serialization",
  "unsupported subtree") — the reporter machinery already supports mixed rows.
- Benchmark case that motivates the decision: a data frame with one closure
  column currently forfeits native encoding for every other column.

### Design sketch: integrity checking

- xxHash64 (vendor the single-file `xxhash.h`, BSD-2, add provenance to
  [inst/COPYRIGHTS](../inst/COPYRIGHTS)) computed incrementally inside
  `writer_write` (streaming XXH64 state on the writer), trailer `u64` appended
  after the payload. No second pass on write.
- Signaling without new magic: set a high bit in the codec byte
  (`codec | 0x80`); old readers fail with "unknown rdz codec" — clean.
- Read side verifies the mmap'd range once before decode (single
  memory-bandwidth pass). That cost is why the default should be **off** for
  the speed preset; reasonable defaults are `checksum = TRUE` for balanced or
  an explicit `write_rdz(..., checksum =)` argument. Decide in the design doc.
- `read_rdz` error message must distinguish "checksum mismatch" (bit rot,
  truncated-then-padded) from structural errors, since users act differently
  on each.

## Performance candidates

Performance work follows profiling and reproducible benchmarks; none of these
items is committed solely because a competitor implements it.

1. Remove redundant support and encoding traversals where measurements show a
   material write-latency cost.
2. Profile repeated and mostly unique strings before changing their layouts.
3. Evaluate parallel decompression for independent balanced blocks.
4. Evaluate column-parallel encoding only after a bounded memory-sink design
   exists.
5. Consider wider integer packing for non-sequence IDs and counts.

Every optimization must preserve exact round trips, clean malformed-input
failure, existing speed-format bytes, and small-object latency.

**Implementation notes per candidate**

1. **Redundant traversals.** `C_rdz_save` walks the tree twice: the
   `native_supported` pre-scan, then `encode_node`. On R ≤ 4.5 the pre-scan
   additionally evaluates `attributes(object)` at *every node* (allocating a
   list each time) and the encoder materializes the same attributes again.
   Measure first: profile `write_rdz` on (a) a nested list of 10k small
   vectors and (b) a 1k-column data frame of factors, R 4.4 vs 4.6
   (`PKG_CFLAGS='-g -O2'`, Instruments on macOS / `perf record` on Linux). If
   material, the clean fix is folding support-checking into a single
   speculative encode pass — which is the fallback-granularity design; the two
   items should be decided together. A cheaper interim fix caches each node's
   attribute list from the pre-scan in a PROTECTed pairlist keyed by traversal
   order, but the protection bookkeeping may not be worth it versus doing the
   real fix once.
2. **Strings.** The only benchmark write losses are repeated strings
   (59.8 ms vs 52.4) and repeated-numeric; unique-string read is a near-tie.
   Profile before touching layout. Specific hypotheses to confirm or kill:
   dictionary probe cost on high-cardinality inputs (the pointer-hash dict in
   `dict_index` degrades to inserting every element before `strings_should_use_flat`
   rejects — check whether the sampling pre-pass mirrors the numeric codec's);
   `CHAR()`/`LENGTH()` per-element loop overhead in `lz4_blocks_build_strings`;
   read-side `mkCharLenCE` allocation (serial by design — R's string pool is
   not thread-safe, so parallel reads can only cover decompression, never
   CHARSXP creation). Write-side LZ4 of flat payloads uses independent 64 KiB
   blocks already and is a natural `rdz_parallel_run` candidate.
3. **Parallel decompression.** The sequential dependency is real but small:
   `reader_real_xor_lz4` chains one `previous` value across blocks, and block
   byte-lengths are only discovered by walking the stream. A parallel layout
   needs two additions per vector, both cheap: a block-size index
   (`u32 × block_count`) and a per-block XOR seed (`u64` = last raw value of
   the previous block). Emit as **new encoding tags**
   (`REAL_ENCODING_XOR_LZ4_INDEXED = 5`; a string-layout analogue) rather than
   changing existing ones — old readers reject unknown tags cleanly, and the
   existing malformed-input tests prove that contract. Gate emission on
   `block_count >= RDZ_PARALLEL_XOR_MIN_BLOCKS` (16, matching the encoder).
   Decode via the existing `rdz_parallel_run` with workers writing disjoint
   output ranges — no synchronization needed. Strings: parallelize block
   decompression into the contiguous payload buffer; CHARSXP construction
   stays on the main thread (see item 2).
4. **Column-parallel encoding.** Blocked on the memory-sink design from the
   raw-vector decision: encode independent `VECSXP` children into per-worker
   `byte_buffer_t` sinks (static partition, as `rdz_parallel_run` provides),
   then concatenate with sequential `fwrite`. Bound memory with a cap on
   total in-flight sink bytes (a fraction of the input's own size is a
   defensible default) and fall back to today's serial path beyond it. The
   reporter needs offset patching (each row's `encoded_bytes` is
   position-relative). Only justified for wide frames — gate on child count
   and estimated payload, and benchmark against the fst wide-numeric case,
   which is where the current lead is thinnest on threads.
5. **Wider integer packing.** `RDZ_INTEGER_MAX_PACKED_BITS` is 8
   ([src/integer_codec.h](../src/integer_codec.h)); IDs and counts commonly
   fit 9–16 bits. The bit-accumulator encode/decode loops are already
   width-generic; the changes are the constant, the `bits` validation range in
   `rdz_integer_decode`, and the exact size gate (already computed). Old
   readers reject wider files with the existing "invalid packed integer
   metadata" error — forward incompatibility confined to balanced files, NEWS
   note. Benchmark on uniform draws from 0..65535 before adopting; the win is
   2–4× on that band, but only if the extra scan doesn't hurt the vectors
   that end up direct.

## Future exploration: frontier techniques

Where rdz stands relative to the literature: the balanced codecs are solid
modern practice — frame-of-reference bit-packing is the PFOR idea (Zukowski,
Héman, Nes, Boncz, *Super-Scalar RAM-CPU Cache Compression*, ICDE 2006), the
XOR-delta + byte-transpose + LZ4 pipeline combines the Gorilla family
(Pelkonen et al., PVLDB 8(12), 2015) with Parquet's `BYTE_STREAM_SPLIT`, and
the exact-or-measured gating is *more* rigorous than the sampling heuristics
used by DuckDB-class systems. What rdz does **not** yet use is the 2020–2024
wave: adaptive lossless float compression (ALP), random-access string
compression (FSST), auto-vectorizable bit-packing layouts (FastLanes), and —
uniquely available to an R package — ALTREP decode-on-access reads.

Everything here is exploration, not commitment. Each item graduates only
through the existing discipline: exact round trips, measured savings against
the current codec on the published benchmark corpus, clean rejection of
malformed input, and no small-object latency regression. Lossy scientific
compressors (zfp, SZ) are excluded outright — bit-exactness is a hard
constraint — as are heavy general codecs (xz, brotli) that trade latency for
ratio.

| Technique | Target | Expected benefit | Cost/risk |
|---|---|---|---|
| ALP | balanced doubles | large: ratio and decode speed on real-world decimals | new codec unit; format tag |
| FSST | string payloads | 2–3× string ratio at GB/s decode; random access | vendor or reimplement; format tag |
| Lazy ALTREP reads | read latency | near-O(1) `read_rdz` for large vectors | high: lifetime + ALTREP complexity |
| FastLanes layout | packed integers/indexes | ~10× unpack throughput, wider widths cheap | interleaved layout = format change |
| Run-end encoding | sorted/keyed columns | near-total collapse of sorted keys | small; new encoding tag |
| Zstd block codec | all LZ4 call sites | better ratio at similar decode speed | dependency size; measure vs LZ4 |
| SIMD shuffle | XOR transpose stage | 4–8× on one pipeline stage | intrinsics vs CRAN portability |
| Delta-of-delta | timestamps | near-regular `POSIXct` compresses ~10× | small; new encoding tag |
| io_uring / madvise | file I/O | modest, cold-cache reads/writes | Linux-only paths |

### ALP — adaptive lossless floating-point compression

Afroozeh, Kuffo, Boncz, *ALP: Adaptive Lossless floating-Point Compression*,
SIGMOD 2024. The current state of the art for doubles, deployed in DuckDB.
Key insight: most real-world doubles are **decimals** (prices, measurements,
rounded ratios), so ALP finds an exponent pair `(e, f)` such that
`round(x * 10^e / 10^f)` is an integer that reconstructs `x` **bit-exactly**,
then bit-packs those integers with frame-of-reference; non-conforming values
are stored as patched exceptions. Vectors that aren't decimal-like fall back
to ALP-RD, which dictionary-encodes the high bits — closely related to what
rdz's XOR path exploits, but without the LZ4 pass.

- **Why it fits rdz**: the encode-side verification is per-value and exact —
  the same philosophy as rdz's bit-for-bit sequence check — and the fallback
  cascade mirrors the existing dictionary → sequence → XOR → direct chain. It
  would slot in as a `REAL_ENCODING_ALP` tag tried before the XOR path.
- **Expected benefit**: the paper reports roughly 2× better ratios than
  Chimp/Gorilla-class codecs on real datasets and decode speeds an order of
  magnitude higher than Gorilla, because decode is just bit-unpack +
  multiply — no branchy bit-level parsing, no general-purpose decompressor.
  For R data frames (decimal-heavy by nature) this should beat XOR+LZ4 on
  both axes for a large fraction of numeric columns.
- **Cost**: a new codec unit comparable in size to `numeric_codec.c`; the
  reference implementation is C++ (github.com/cwida/ALP), so expect a C
  reimplementation of the core rather than vendoring. Prototype gate:
  implement scalar ALP, run it against the benchmark corpus's numeric
  columns, and adopt only if it wins the existing gating against XOR+LZ4.

### FSST — fast random-access string compression

Boncz, Neumann, Leis, *FSST: Fast Static Symbol Table string compression*,
PVLDB 13(11), 2020. Builds a 255-symbol table (symbols up to 8 bytes) over a
sample, then encodes each string independently as symbol indexes: ~2–3×
compression on short strings, multi-GB/s decode, and — unlike LZ4 blocks —
**per-string random access** with no block decompression.

- **Why it fits rdz**: strings are rdz's only benchmark losses (repeated
  strings write, unique strings near-tie). FSST would apply to both layouts:
  dictionary payloads (compress the unique set) and flat payloads (replace or
  precede the 64 KiB LZ4 blocks). The existing "adopt only if the complete
  encoded layout is smaller" gate applies unchanged.
- **Random access synergy**: per-string decode is the enabler for lazy
  ALTREP string vectors (below) — LZ4 blocks force 64 KiB granularity,
  FSST decodes one element.
- **Cost**: MIT-licensed reference implementation (github.com/cwida/fsst) is
  compact C++; vendoring with provenance like LZ4 or reimplementing the
  decoder (trivial — a table lookup loop) plus the sampling table builder
  (the hard part) are both viable. Note `mkCharLenCE` remains the read-side
  floor for strings regardless of codec (R's string pool is serial).

### Lazy decode-on-access reads via ALTREP

R-specific frontier no competitor fully exploits: `read_rdz()` could return
ALTREP vectors backed by the mmap'd file that decode regions on first access
(Tierney's ALTREP framework, R ≥ 3.5; `fst` does a coarse version of this
for row-range access). A 1 GB numeric column would "read" in microseconds;
work happens only for data actually touched.

- **Expected benefit**: transformative for the headline latency metric —
  read cost becomes proportional to data *used*, not data *stored*. Direct
  (uncompressed) speed-preset payloads are the natural first target: an
  ALTREP double/integer vector whose `Dataptr` materializes via one `memcpy`
  from the mapping, and whose `Get_region` serves bounded reads with **zero**
  materialization — the exact mirror of rdz's existing write-side ALTREP
  streaming.
- **Risks (why this is exploration)**: the mapping must outlive every
  returned vector (a finalizer-owned mmap handle shared by all ALTREP objects
  from one file); file mutation/deletion during the vector's lifetime becomes
  user-visible (document as undefined, like `fst`); serialization of the lazy
  vector must materialize; and ALTREP method tables are a large correctness
  surface under gctorture. Gate behind `read_rdz(..., lazy = TRUE)` and keep
  eager reads the default until the semantics are proven.

### FastLanes — auto-vectorizable bit-packing layout

Afroozeh & Boncz, *The FastLanes Compression Layout: Decoding >100 Billion
Integers per Second with Scalar Code*, PVLDB 16(9), 2023. Reorders packed
values into a 1024-bit interleaved layout so plain scalar C auto-vectorizes
to full SIMD width on any ISA — **no intrinsics**, which sidesteps the CRAN
portability problem that rules out hand-written AVX2/NEON dispatch.

- **Where it applies**: `rdz_integer_decode`'s bit accumulator loop and the
  numeric dictionary index unpacking — both currently scalar-sequential
  (~1 value/cycle vs ~16+ with a vectorized layout). It would also make the
  wider-integer-packing candidate (performance item 5) nearly free to decode.
- **Cost**: the interleaved order is a different wire layout, so this is a
  new encoding tag, and encode-side transposition adds write cost — measure
  against rdz's write-latency priority. Precursor literature for context:
  SIMD-BP128 (Lemire & Boytsov, *Decoding billions of integers per second
  through vectorization*, Software: Practice & Experience 45(1), 2015).

### Run-end encoding for sorted and grouped columns

Arrow's run-end encoding (REE) / Parquet's `RLE_DICTIONARY`: store
`(run_end, value)` pairs instead of per-element indexes. rdz's dictionary
paths bit-pack one index per element even when the column is sorted — but
sorted columns are structurally common in R: `data.table` **keys** (sorted by
definition, and rdz already preserves key metadata), grouped tibbles, and
ordered factors. A keyed grouping column collapses from n packed indexes to
a handful of runs.

- **Cost**: small — one new encoding tag per affected codec and a run-length
  scan that the existing sampling pass can trigger cheaply (sample detects
  long runs → attempt REE → adopt under the existing exact size gate).

### Zstd as an alternative block codec

Collet & Kucherawy, RFC 8878 (2021). Zstd at levels 1–3 (or negative levels)
approaches LZ4 decode speed with visibly better ratios; qs2 is zstd-based.
rdz's block architecture makes this a contained experiment: `lz4_blocks_*`
already isolates the codec behind a compress/decompress-one-block interface,
so a `RDZ_BLOCK_CODEC_ZSTD` variant per affected tag is mostly plumbing.

- **Decision criterion**: adopt only where the existing measured-saving gates
  select it *and* decode latency does not regress — on rdz's latency-first
  objective, LZ4 may simply keep winning; the experiment settles it. Cost:
  zstd's amalgamated source is ~10× LZ4's footprint (vendor weight, compile
  time, rchk surface).

### Smaller, cheaper explorations

- **SIMD byte transpose**: the XOR path's shuffle stage is scalar
  (`rdz_real_xor_encode_block`); Blosc demonstrated SIMD shuffle long ago
  (Alted, *Why modern CPUs are starving and what can be done about it*,
  Computing in Science & Engineering 12(2), 2010). A portable improvement
  without intrinsics: process the transpose in cache-blocked tiles the
  compiler can auto-vectorize; verify with the existing byte-identity tests.
- **Delta-of-delta for timestamps**: Gorilla's timestamp scheme. rdz's
  constant-delta sequence covers perfectly regular `POSIXct`; real logs are
  *nearly* regular. Zigzag + Simple-8b on second-order deltas (Anh & Moffat,
  *Index compression using 64-bit words*, Software: Practice & Experience
  40(2), 2010) captures that band. New `REAL_ENCODING_DELTA2` tag under the
  usual exact-reconstruction gate.
- **I/O tuning**: `posix_fadvise`/`madvise(MADV_SEQUENTIAL)` on the read
  mapping (one line, measure cold-cache), `fallocate` before large writes,
  and — Linux-only, larger lift — io_uring batched writes (Axboe, 2019).
  Expect modest wins; cheap to test, easy to discard.
- **xxHash3** over xxHash64 if the integrity-checking decision is accepted:
  same vendor, ~2× throughput on large inputs, no format implications since
  the checksum field is new either way.
- **Pcodec (pco)** (github.com/pcodec/pcodec): a 2023– Rust-ecosystem
  numerical codec with strong published ratios; not directly vendorable (Rust)
  but its automatic mode-selection design is worth reading alongside ALP when
  designing the numeric codec cascade.

### Suggested exploration order

1. **ALP** — doubles dominate R data; biggest expected ratio+speed win.
2. **FSST** — attacks the only benchmark losses; enables string random access.
3. **Run-end encoding** — smallest cost, targets keyed/sorted data rdz
   already understands.
4. **Lazy ALTREP reads** — highest ceiling for the latency claim, highest
   risk; prototype on speed-preset direct payloads only.
5. **FastLanes / zstd / SIMD shuffle / delta-of-delta / I/O** — as profiling
   indicates, under the standing rule: measured wins or it doesn't ship.

## Go beyond: features from other dynamic-language ecosystems

The previous section is about *how bytes are encoded*; this one is about
*what a serializer can do for its users*. Python, Julia, and the JavaScript
runtimes have grown serialization features that no R format (rds, qs2, fst)
offers today. Each idea below names its origin, what it would look like in
rdz, and which already-planned infrastructure it reuses. Same standing rule:
exploration, not commitment, and nothing that compromises the no-knobs
latency-first identity.

| Feature | Origin | rdz shape | Reuses |
|---|---|---|---|
| Selective reads | safetensors, Parquet footer, fst row ranges | `read_rdz(file, select=)` | block/offset index from parallel decode |
| Safe untrusted loading | safetensors vs pickle | `read_rdz(file, trusted = FALSE)` | existing decode validation |
| File metadata without decode | safetensors header, Parquet KV, npz | `rdz_metadata(file)` | header extension |
| Multi-object containers | Arrow IPC stream, Zarr, HDF5, shelve | `rdz_archive()` append + named lazy read | footer index, memory sink |
| Out-of-band buffers / zero-copy IPC | pickle protocol 5 (PEP 574), Ray/Plasma | skeleton + buffer table for mirai/future | memory sink from raw-vector decision |
| Trained compression dictionaries | zstd `--train` (RocksDB practice) | shared dictionary for many small files | contingent on zstd exploration |
| Idempotent writes / pipeline hashing | joblib.Memory, content addressing | skip rewrite on identical hash; fast `rdz_hash()` | streaming hash from integrity decision |
| Custom class encoders | Nippy freeze/thaw, Kryo registration | open question — likely **no** | — |

### Selective reads: decode only what is asked for

safetensors (Hugging Face) stores a JSON header of `name → offset` so one
tensor loads without touching the rest; Parquet's footer metadata enables
column projection; fst reads row ranges. rdz decodes whole objects only.

- **rdz shape**: `read_rdz(file, select = c("price", "date"))` for data-frame
  columns and named list elements, plus `rdz_ls(file)` returning the
  `explain_rdz`-style node table *from the file* without decoding payloads.
- **What it needs**: a footer with per-node byte offsets — the same offset
  index the parallel-decompression candidate needs per vector, promoted one
  level to the node tree. Written after the payload (single pass preserved),
  found via a trailing offset, ignored by old readers only if gated behind a
  new codec/flag byte.
- **Why it is worth it**: the latency story becomes *O(data you want)*, not
  *O(file)* — one column of a 10 GB frame in milliseconds, which neither qs2
  nor rds can do and fst only does by row range. Pairs naturally with lazy
  ALTREP reads (same index, two consumption modes).

### Safe loading of untrusted files

The defining feature of safetensors is what it *refuses* to do: loading can
never execute code, because the format holds only typed buffers — a direct
response to pickle's arbitrary-code-execution problem. R's `readRDS` is far
safer than pickle, but unserializing attacker-controlled rds bytes still
rebuilds arbitrary objects — environments, classed objects whose methods run
on first touch, promises — and CVE-grade R unserialization bugs exist
(CVE-2024-27322).

- **rdz shape**: `read_rdz(file, trusted = FALSE)` decodes **native nodes
  only** — plain validated data, no R unserialize — and errors on any
  R-fallback codec or `NODE_FALLBACK` node. The native decoder already
  validates every tag, index, and length against the mutation corpus, so the
  hard part is done; the feature is one refusal branch plus documentation.
- **Why it is worth it**: rdz becomes the only R format with a documented
  "this call cannot construct active objects" mode — genuinely valuable for
  services deserializing user uploads, and nearly free to build.

### File metadata readable without decoding

safetensors carries a user metadata dict in its header; Parquet has key-value
metadata; `npz` archives are inspectable. R formats store nothing but the
object — even discovering what an rds contains requires full deserialization.

- **rdz shape**: `write_rdz(..., metadata = list(...))` accepting a small
  named character list, stored after the header; `rdz_metadata(file)` reads
  it in microseconds. Reserve keys for provenance rdz writes itself:
  package version, R version, creation time.
- **Cost**: trivial — a length-prefixed block behind a flag bit; old readers
  reject flagged files cleanly. Pipelines (and future-you) get provenance for
  free; support tickets get `rdz_metadata()` before "please send the file".

### Multi-object appendable containers

Arrow IPC streams append record batches; Zarr and HDF5 store named datasets
in one file; Python's `shelve` gives dict-like persistent storage. The
many-objects benchmark shows rdz users store thousands of small objects —
today as thousands of files, paying filesystem overhead per object.

- **rdz shape**: `rdz_archive_open(path)` with `$write(name, object)`
  (append-only) and `$read(name)` (lazy, via a footer name→offset index
  rewritten on close). Each entry is a complete rdz stream, so the entry
  format needs nothing new — the container is framing plus the index.
- **Synergies**: entry reads reuse the raw-vector reader; the trained
  zstd dictionary idea (below) applies across entries; `targets`-style
  pipelines get one file per stage instead of one per object.

### Out-of-band buffers and zero-copy process transfer

Pickle protocol 5 (PEP 574, Python 3.8) splits serialization into a small
object-graph stream plus a list of out-of-band buffer references, so Dask and
Ray route multi-GB arrays over shared memory or RDMA without copying them
into the pickle bytes. Ray's Plasma store shares immutable objects across
processes via mmap. JavaScript's structured clone has "transferables" —
zero-copy ownership transfer between threads.

- **rdz shape**: a C-level variant of the serializer that emits the node
  tree with large payloads replaced by `(buffer_id, size)` references plus a
  buffer table the caller maps into `memfd`/`shm_open` segments. The consumer
  (`mirai`/`future`/`callr` workers on the same host) maps the segments and
  rebuilds vectors — ideally as ALTREP views, making worker dispatch
  *O(metadata)*.
- **Honest cost**: this is the largest item in this section — shared-memory
  lifetime management across R processes is hard, and it only pays off inside
  a parallel framework that adopts it. The right first step is a design
  conversation with the mirai author, not code. Listed because it is the
  feature that would most change what "fast serialization" means in R:
  the fastest serialization is the copy you never make.

### Trained compression dictionaries for small objects

zstd can train a dictionary on sample data and reuse it across many small
payloads (standard practice in RocksDB and Cloudflare's edge caches) — small
inputs compress 2–5× better with a dictionary than without, because the
dictionary supplies the context a 2 KB payload lacks. Contingent on the zstd
exploration in the previous section; if adopted, `rdz_archive` entries and
many-small-files workloads are exactly the shape dictionaries serve.

### Idempotent writes and pipeline hashing

joblib memoizes to disk keyed on argument hashes; content-addressed stores
skip work when bytes already exist. The `targets` package — R's standard
pipeline tool — hashes every stored object on every pipeline pass.

- **rdz shape**: two small features. (1) If the integrity-checking decision
  lands, `write_rdz` already streams a hash while encoding; comparing it
  against an existing target file's trailer and skipping the rewrite makes
  writes idempotent for free (one stat + one trailer read). (2) Expose
  `rdz_hash(object)` — the encode pass without the write, returning the
  hash — giving pipeline tools an object hash at serialization speed, which
  for `targets` users would be markedly faster than their current
  digest-over-serialize path.

### Custom class encoders — noted and (probably) declined

Nippy (Clojure) and Kryo (JVM) let packages register custom freeze/thaw
handlers, and one could imagine `sf` or `units` registering native rdz
encoders. Recorded here to make the decision explicit rather than by
omission: the likely answer is **no** — a registration API makes the wire
format's meaning depend on session state, breaks the "any rdz reader decodes
any rdz file" property, and turns third-party bugs into rdz file corruption.
The `data.table` special case works because it is *in-tree, tested, and
versioned with the format*. Revisit only if a second in-tree case (sf?)
reveals a genuinely general pattern.

### Suggested order

1. **Metadata** — trivial cost, immediate pipeline value.
2. **Safe loading** — one refusal branch; a unique, honest security feature.
3. **Selective reads** — biggest user-visible capability; shares the offset
   index with parallel decode, so design them together.
4. **Archive container** — serves a measured workload (many small objects).
5. **Idempotent writes / `rdz_hash`** — after the integrity decision.
6. **Zero-copy IPC** — start as a design conversation with parallel-framework
   authors; build only with a committed consumer.

## Go beyond: features from other dynamic-language ecosystems

The frontier section is about *how bytes are encoded*; this one is about
*what a serializer can do for its users*. Python, Julia, and the JavaScript
runtimes have grown serialization features that no R format (rds, qs2, fst)
offers today. Each idea below names its origin, sketches the R API, and lists
what the C side requires — grounded in the existing `writer_t`/`reader_t`
machinery so effort is estimable. Same standing rule: exploration, not
commitment, and nothing that compromises the no-knobs latency-first identity.

A shared prerequisite: several features flag their presence in the header.
Reserve the codec byte's high nibble as flags now (`RDZ_CODEC_MASK 0x0f`;
`0x80` checksum — matching the integrity sketch — `0x40` metadata, `0x20`
node index). Old readers reject any flagged byte with the existing "unknown
rdz codec" error, so every feature below is cleanly forward-incompatible and
backward-transparent.

| Feature | Origin | rdz shape | Reuses |
|---|---|---|---|
| Selective reads | safetensors, Parquet footer, fst row ranges | `read_rdz(file, select=)` | reporter machinery; block/offset index |
| Safe untrusted loading | safetensors vs pickle | `read_rdz(file, trusted = FALSE)` | existing decode validation |
| File metadata without decode | safetensors header, Parquet KV, npz | `rdz_metadata(file)` | header flag nibble |
| Multi-object containers | Arrow IPC stream, Zarr, HDF5, shelve | `rdz_archive()` append + named lazy read | footer index, sub-readers |
| Out-of-band buffers / zero-copy IPC | pickle protocol 5 (PEP 574), Ray/Plasma | skeleton + buffer table for mirai/future | memory sink from raw-vector decision |
| Trained compression dictionaries | zstd `--train` (RocksDB practice) | shared dictionary for many small files | contingent on zstd exploration |
| Idempotent writes / pipeline hashing | joblib.Memory, content addressing | `rdz_hash()`; skip rewrite on identical hash | streaming hash from integrity decision |
| Custom class encoders | Nippy freeze/thaw, Kryo registration | open question — likely **no** | — |

### Selective reads: decode only what is asked for

safetensors (Hugging Face) stores a `name → offset` header so one tensor
loads without touching the rest; Parquet's footer enables column projection;
fst reads row ranges. rdz decodes whole objects only.

**R API**

```r
write_rdz(x, f, index = TRUE)          # opt-in: writes the node index footer

rdz_ls(f)                              # explain_rdz-shaped data frame read
#>   path      relation  name   type      length  offset  encoded_bytes
#>   $         root      <NA>   list          12      12          9.6e7
#>   $[[1]]    element   price  double     1e+06     311          8.0e6
#>   ...                                  # from the footer alone — no decode

read_rdz(f, select = c("price", "date"))   # data frame with two columns
read_rdz(f, select = 1:3)                  # positional, for unnamed lists
```

Semantics: `select` applies to top-level elements of a list/data-frame root;
error if the file has no index, the root is not a list, or a name is absent.
Root attributes (`names`, `class`, `row.names`) are always decoded, then
`names` is subset to match — so the result is a valid data frame, not a bag
of columns.

**C implementation**

- The index already almost exists: `reporter_t` rows carry parent, depth,
  relation, name, type, length, and byte counts — `explain_rdz` computes
  exactly this. Add a `uint64_t offset` field to `report_row_t` (captured
  from `writer->position` at `reporter_add` time; `report_start` in
  `encode_node` is already that value). `write_rdz(index = TRUE)` runs the
  writer with a reporter attached (today only explain does), costing one
  array append per node.
- Footer wire format: serialized reporter rows (varint-length name bytes,
  fixed-width numeric fields), then `u64 footer_size`, flagged by `0x20` in
  the codec byte. Written after the payload, so single-pass writing is
  preserved.
- `C_rdz_ls(path)`: mmap, parse header, seek to `size - 8`, read
  `footer_size`, parse rows, emit via the existing `reporter_dataframe` —
  zero payload decoding.
- `C_rdz_read(path, select_sexp)`: decode the root node header and its
  attributes (conveniently, `encode_node` writes attributes *before*
  children, so the attribute region is contiguous after the node header),
  then for each selected child set `reader->pos` to its footer offset and
  call `decode_node`. Assemble a `VECSXP`, subset the names attribute,
  install `class`/`row.names`.
- **The footer is untrusted input**: validate every offset against
  `reader->size`, require offsets strictly inside the payload region, and
  keep the depth guard — a malicious index must not become an OOB seek or a
  decode loop. Add footer-mutation cases to the fuzz corpus.
- Synergy: this is the node-level version of the block-offset index the
  parallel-decompression candidate needs at vector level — design the two
  footers together so they share layout and validation code.

### Safe loading of untrusted files

The defining feature of safetensors is what it *refuses* to do: loading can
never execute code. R's `readRDS` is far safer than pickle, but unserializing
attacker-controlled bytes still rebuilds arbitrary objects — environments,
classed objects whose methods run on first touch — and CVE-grade R
unserialization bugs exist (CVE-2024-27322).

**R API**

```r
read_rdz(f)                   # default trusted = TRUE, current behavior
read_rdz(f, trusted = FALSE)  # native nodes only; errors on any
                              # R-serialized payload with a message that
                              # names the reason and the remedy
```

With `trusted = FALSE` the `data.table::setDT()` self-ref rebuild is also
skipped (no package code runs on untrusted data); the object arrives as a
plain classed data frame and the doc tells users to call `setDT()` themselves
if they trust it.

**C implementation**

- `read_context_t` gains `int trusted`; threaded through from a new third
  argument to `C_rdz_read`.
- Two refusal points, ~20 lines total: in `read_body`, if
  `codec != RDZ_CODEC_NATIVE && !trusted` →
  `Rf_error("file contains R-serialized data; pass trusted = TRUE only for sources you trust")`;
  and in `decode_node`, the future `NODE_FALLBACK` case checks the same flag.
- The guarantee this documents: the native decoder only ever calls
  `Rf_allocVector`, `mkCharLenCE`, and `Rf_setAttrib` with tag-validated
  input — no evaluation, no method dispatch, no promises. The mutation
  corpus (committed priority 2) is what makes this claim testable rather
  than aspirational; say so in the man page.

### File metadata readable without decoding

safetensors carries a user metadata dict in its header; Parquet has key-value
metadata; `npz` archives are inspectable. R formats store nothing but the
object — even discovering what an rds contains requires full deserialization.

**R API**

```r
write_rdz(x, f, metadata = c(pipeline = "etl-v3", commit = "9c1f0d2"))

rdz_metadata(f)
#>      pipeline        commit   rdz:version  rdz:r-version
#>      "etl-v3"     "9c1f0d2"       "0.0.1"        "4.5.2"
```

User keys are a named character vector; names must be unique, non-empty, and
must not start with the reserved `rdz:` prefix, under which the writer
records provenance (package version, R version, creation time) automatically
whenever metadata is requested. Total block capped at 64 KiB — metadata, not
payload.

**C implementation**

- Wire: flag `0x40` in the codec byte; block sits between the architecture
  bytes and the first node: `u32 count`, then per entry
  `varint keylen | key utf8 | varint vallen | value utf8`. The existing
  `byte_buffer_varint`/`buffer_varint` pair already implements the varint
  codec.
- Writer: `save_body` emits the block before dispatching to `encode_node` /
  `R_Serialize`; validation (uniqueness, prefix, cap) happens in the R
  wrapper where vctrs-style errors are cheap.
- `C_rdz_metadata(path)`: reuse `C_rdz_read`'s open/mmap path, parse header
  + block, build a named `STRSXP`, return — never touches the payload.
  Files without the flag return `NULL`.
- `explain_rdz` and the fuzz corpus gain the flagged variant; decode
  validation mirrors the string-length checks (varint bounds, UTF-8 length
  against block size).

### Multi-object appendable containers

Arrow IPC streams append record batches; Zarr and HDF5 store named datasets
in one file; Python's `shelve` gives dict-like persistence. The many-objects
benchmark shows rdz users store thousands of small objects — today as
thousands of files, paying filesystem overhead per object.

**R API**

```r
a <- rdz_archive("cache.rdza")             # create or open for append
rdz_archive_write(a, "training", df, preset = "balanced")
rdz_archive_write(a, "model_fit", fit)     # any object; fallback rules apply
rdz_archive_ls(a)                          # name, bytes, codec, written_at
fit <- rdz_archive_read(a, "model_fit")    # decodes one entry only
rdz_archive_close(a)                       # also on gc via finalizer
```

The handle is an external pointer owning an open `FILE*`; a finalizer closes
it. Single-writer, many-reader; no locking beyond advisory documentation
(same contract as fst/qs2 files today).

**C implementation**

- Container wire: magic `RDZARCH1`, then appended entries — each a complete,
  unmodified rdz stream (the entry format needs *nothing new*) — then a
  footer: `u32 count`, per entry `varint name | u64 offset | u64 size`,
  `u64 footer_size`, trailing magic. Append = seek to the old footer start,
  write the entry, write the rebuilt footer. A torn append corrupts only the
  footer; the trailing magic detects it, and an optional
  `rdz_archive_repair()` can rescan for entry magics.
- Writing an entry reuses `save_body` against the archive's `FILE*` — the
  writer already tracks only relative `position`, so no changes there.
- Reading an entry builds a sub-`reader_t` over the archive mapping:
  `{ data = map + offset, size = entry_size, fd = -1 }`. **Pitfall**: the
  `fd` must be `-1` (mmap-only path) — `reader_payload`'s `pread` branch
  computes file offsets from `reader->pos`, which is entry-relative; with a
  real fd it would read from the wrong archive position. Alternatively give
  `reader_t` a `base` offset; the `fd = -1` route is simpler and loses only
  the large-payload `pread` optimization, which the mmap path covers anyway.
- Entry names live only in the footer, so renames and tombstones (delete =
  drop from footer, space reclaimed by an explicit `rdz_archive_compact()`)
  are footer-only operations.

### Out-of-band buffers and zero-copy process transfer

Pickle protocol 5 (PEP 574, Python 3.8) splits serialization into a small
object-graph stream plus out-of-band buffer references, so Dask and Ray route
multi-GB arrays over shared memory without copying them into the pickle
bytes. Ray's Plasma store shares immutable objects across processes via mmap.
JavaScript's structured clone has "transferables" — zero-copy ownership
transfer between threads.

**R API**

```r
s <- rdz_serialize_oob(x, threshold = 65536L)
#> s$skeleton  raw vector: node tree with large payloads replaced by refs
#> s$buffers   data frame: id, size, shm_name — one shared segment per buffer

# in a worker process (mirai/future/callr):
y <- rdz_unserialize_oob(s$skeleton, s$buffers)          # maps, zero-copy
y <- rdz_unserialize_oob(s$skeleton, s$buffers, lazy = FALSE)  # eager copy
```

**C implementation**

- Writer: `writer_t` gains an optional out-of-band hook
  `int (*oob)(void *ctx, const void *data, size_t size, uint32_t *id)`. In
  `encode_node`, payloads ≥ threshold call the hook — which copies into a
  fresh segment from `memfd_create` (Linux) / `shm_open` (POSIX) /
  `CreateFileMapping` (Windows) — and emit a new payload encoding tag
  `PAYLOAD_OOB { u32 id, u64 size }` instead of bytes. Everything else in
  the stream is unchanged, so the skeleton is a valid (small) rdz stream.
- Eager reader: resolve each ref by mapping the named segment and `memcpy`
  into the freshly allocated vector — already a win (one copy instead of
  serialize + transport + unserialize).
- Zero-copy reader: an ALTREP class whose `Dataptr` points into a
  `MAP_PRIVATE` mapping of the segment. `MAP_PRIVATE` is the elegant part:
  the OS gives copy-on-write *page* semantics, so R-level mutation is safe
  without ALTREP duplication tricks. Needs a finalizer-owned mapping handle
  shared by all vectors from one skeleton, a `Serialized_state` method that
  materializes, and gctorture-grade testing — the same correctness surface
  as the lazy-ALTREP-reads exploration, deliberately shared with it.
- **Honest cost**: the largest item in this section, and it only pays off
  inside a parallel framework that adopts it. The right first step is a
  design conversation with the mirai author, not code. Listed because it is
  the feature that would most change what "fast serialization" means in R:
  the fastest serialization is the copy you never make.

### Trained compression dictionaries for small objects

zstd trains a dictionary on sample data and reuses it across many small
payloads (standard practice in RocksDB and Cloudflare's caches) — small
inputs compress 2–5× better with a dictionary, because the dictionary
supplies the context a 2 KB payload lacks.

**R API**

```r
d <- rdz_train_dictionary(sample_objects, size = 112640L)   # raw vector
write_rdz(x, f, dictionary = d)
read_rdz(f)                     # error: "requires dictionary xxh64:9c1f…"
read_rdz(f, dictionary = d)
a <- rdz_archive("c.rdza", dictionary = d)   # stored once, applies to entries
```

**C implementation**

- Contingent on the zstd exploration: training is `ZDICT_trainFromBuffer`
  over the concatenated serialized samples; block compression switches to
  `ZSTD_compress_usingCDict` / `_usingDDict`.
- Wire: a header flag carrying the dictionary's xxh64 so mismatches fail
  with a *named* error before any decode; archives store the dictionary
  itself as a hidden footer entry, making archive files self-contained.
- Keep it out of the default path entirely — this is a power feature for
  the many-small-objects workload, gated behind explicit arguments.

### Idempotent writes and pipeline hashing

joblib memoizes to disk keyed on argument hashes; content-addressed stores
skip work when bytes already exist. The `targets` package — R's standard
pipeline tool — hashes every stored object on every pipeline pass, today by
digesting a full serialization.

**R API**

```r
rdz_hash(x)                            # "xxh3:9c1f0d2e…" — encode-speed,
                                       # no I/O, no allocation of the stream
write_rdz(x, f, mode = "if-changed")   # skip the write when content matches
write_rdz(x, f, mode = "atomic")       # temp file + rename, crash-safe
```

**C implementation**

- `rdz_hash`: a third writer sink — after `FILE*` and the memory buffer — a
  *hashing* sink where `writer_write` only updates a streaming XXH3 state
  and `position`. `save_body` runs unchanged; the entry point returns the
  hex digest. This reuses the streaming-hash plumbing the integrity-checking
  decision installs; without that decision it can still exist standalone.
- `mode = "if-changed"`: encode to `<file>.tmp` while hashing (one pass);
  read the existing file's checksum trailer (integrity feature) or, absent a
  trailer, hash the existing file; on match `unlink(tmp)`, else
  `rename(tmp, file)`.
- `mode = "atomic"` falls out for free: `rename(2)` is atomic on POSIX
  (Windows needs `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`), which upgrades
  every write to crash-safe — arguably worth making the default someday;
  measure the temp-file cost first on small objects.
- The `targets` angle is worth a conversation with its maintainer once
  `rdz_hash` exists: format + hash in one encode pass would be markedly
  faster than their digest-over-serialize path.

### Custom class encoders — noted and (probably) declined

Nippy (Clojure) and Kryo (JVM) let packages register custom freeze/thaw
handlers, and one could imagine `sf` or `units` registering native rdz
encoders. Recorded to make the decision explicit rather than by omission:
the likely answer is **no**, for two reasons. Product: a registration API
makes the wire format's meaning depend on session state and breaks "any rdz
reader decodes any rdz file". Engineering: decode-side dispatch would call
arbitrary R code mid-decode while `reader_t` holds the mapping across a
potential longjmp — survivable via `R_UnwindProtect`, but user callbacks
could re-enter `read_rdz` or allocate unboundedly, and the
`trusted = FALSE` guarantee dies the moment third-party code runs during
decode. The `data.table` special case works because it is *in-tree, tested,
and versioned with the format*. Revisit only if a second in-tree case (sf?)
reveals a genuinely general pattern.

### Suggested order

1. **Metadata** — trivial cost, immediate pipeline value, exercises the
   header-flag mechanism every later feature needs.
2. **Safe loading** — ~20 lines of C; a unique, honest security feature.
3. **Selective reads** — biggest user-visible capability; design its footer
   together with the parallel-decode block index.
4. **Archive container** — serves a measured workload (many small objects);
   entry format requires nothing new.
5. **Idempotent writes / `rdz_hash`** — after (or alongside) the integrity
   decision; atomic writes fall out for free.
6. **Dictionaries** — only if zstd graduates from the frontier section.
7. **Zero-copy IPC** — start as a design conversation with parallel-framework
   authors; build only with a committed consumer.

## Release readiness

- [ ] Keep R 4.1, release, devel, Windows, macOS, and Linux checks green.
- [ ] Run the manual sanitizer, valgrind, LTO, gctorture, and rchk workflows for
  release candidates.
- [ ] Run the CRAN extra-checks and refresh `cran-comments.md`.
- [ ] Publish benchmark results with complete runtime and hardware metadata.
- [ ] Document the accepted portability and fallback contract prominently.

**Implementation notes**

- Benchmark publication: keep the standard set by
  [inst/benchmarks/data-frames-fst-results.md](../inst/benchmarks/data-frames-fst-results.md)
  (hardware, thread counts, library versions, seeded iterations, strict
  `identical()` validation). Pin seeds and sizes in the scripts so runs are
  comparable across releases; record `sessionInfo()` in each results file.
- Consider a lightweight regression tripwire before 1.0: store one baseline
  timings JSON per benchmark in-repo and have the manual benchmark workflow
  flag >10 % speed-preset latency regressions. The "fast" claim needs an
  automated guard, not a quarterly manual run.

Current verification and immediate work live in [status.md](status.md).
Completed user-visible changes live in [NEWS](../NEWS.md).
