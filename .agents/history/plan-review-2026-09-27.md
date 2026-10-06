# Plan and Roadmap Review — 2026-09-27

> **Historical.** This document records a review of the plans as they stood on 2026-09-27, before the C port was adopted. Where it disagrees with the code, the code and [architecture.md](../architecture.md) are right.

Goal restated: the fastest, most complete serializer in the R ecosystem, in
Rust. Beat `qs2`/`qdata` on general objects and `fst` on data frames, while
covering every object base R can serialize.

This review assesses whether the current plan gets there, what to change, and
how to run investigation, testing, and benchmarking from here on. It is a
proposal; [roadmap.md](../roadmap.md) and the other guidance documents should be
rewritten once its decisions are accepted.

## 1. Verdict

The foundations are sound and unusually careful: a seekable, checksummed
container with a real directory; whole-root fallback with no fallback islands;
an honest benchmark harness; direct Rust IO with atomic replacement; no
`unwrap()` at the boundary. Keep all of that.

The sequencing is the problem. After eight weeks the native codec covers one
type, and it is the least valuable one. Every data frame, every integer vector,
every string still goes through `serialize()` into a raw vector and then to
disk uncompressed. The four things that decide the competition — a typed
structure-plus-columns layout, per-block compression, a parallel pipeline, and
a streaming generic bridge — are all scheduled after five more per-type
"vertical slices", each with a completion gate heavier than the one that took
the whole of August. On the current plan there is no data-frame number at all
until Phase 7.

The fix is to turn the roadmap sideways: build a thin, correct, uncompressed
native path through *all* types and data frames first, then add compression,
threads, and the streaming bridge as horizontal layers, and only then spend
time on per-type transforms — chosen by profiling the real workload rather than
by type order.

## 2. What to keep

- Container v2: header, block headers, directory, trailer, CRC32, limits,
  canonical little-endian fields, XDR generic stream. It already has the object
  and attribute tables the horizontal plan needs.
- Whole-root fallback; never embed fallback islands.
- Attributes are part of the value; never strip silently. (The mechanism
  changes below; the principle does not.)
- The `data.table::.internal.selfref` registry as the one omission exception.
- The optimistic single-call `rdz_try_write_native()` shape.
- The benchmark harness contract (paired read/write, checksum-on competitors,
  one-thread default, warm/cold labelling, metadata capture).
- The logical codec as implemented. It is finished; stop investing in the
  remaining long-run/alternating write gap until profiling of real data frames
  says it matters (it will not).
- Pre-0.1 freedom to change bytes.

## 3. What to change, ranked by leverage

### 3.1 Sequence horizontally, not by type

Replace "Phase N = type N, fully finished" with milestones that each cut
through every type. The first milestone is a walking skeleton: raw
little-endian blocks for every atomic type, lists, and a general attribute map,
single-threaded, uncompressed, but with the directory populated and every
round-trip test in place. From that point every later layer (compression,
threads, transforms, selective reads) is measured on the real target — data
frames — instead of on logical vectors.

### 3.2 A general native attribute map replaces per-type allowlists

The current policy allows `names` on vectors, `levels`/`class` on factors,
`names`/`row.names`/`class` on data frames, and sends everything else to the
generic codec. That makes `Date`, `POSIXct`, `difftime`, `integer64`, `ts`,
matrices, arrays, `dimnames`, `tzone`, `units`, `data.table` keys/indices,
`tibble` classes, and any S3 vector with a custom attribute fall back — i.e.
most real data-frame columns.

Proposed rule: **an object is natively eligible when its storage type is native
and every attribute value is itself natively eligible (recursively).** Then:

- a data frame is a list with three attributes, all native — no data-frame
  codec is needed for *semantics*; the data-frame "codec" becomes a physical
  layout decision (column scheduling, column descriptors in the directory);
- a factor is an integer vector with `levels` (character) and `class`
  (character); the narrow-code-width representation is an *encoding* choice
  for that integer vector, not a separate type;
- matrices, `Date`, `POSIXct`, `difftime`, `integer64`, `nanotime`-free S4-less
  classes all work with zero special cases;
- compact row names `c(NA_integer_, -n)` are an ordinary integer vector and
  round-trip exactly.

This is safe because reference semantics cannot arise among natively eligible
values: environments, external pointers, weak references, symbols, promises,
closures, and pairlists all force whole-root fallback, and base R serialization
itself does not preserve identity of shared ordinary vectors. That also
removes the "reference/back-reference table for lists" research item: the
native codec needs no object table for graph reasons, only directory IDs.

Preserve the `OBJECT` and S4 bits as flags on the object descriptor; an S4 bit
on a native-typed value is fine to carry (an S4 object extending `numeric`
with only vector attributes is eligible), an `OBJSXP` is not.

### 3.3 ALTREP must not cause whole-root fallback

`1:n`, `seq_len()`, and `rev(seq)` columns are common in data frames. Today an
ALTREP anywhere forces the entire root through `serialize()`. Policy: read
ALTREP vectors through `INTEGER_GET_REGION`/`REAL_GET_REGION` (or
`DATAPTR_OR_NULL` when non-null) block by block; special-case compact integer
and real sequences as a (start, step, n) encoding since that is free and
exact; otherwise materialize per block. Deferred-string ALTREP (from
`as.character(1:n)`) materializes element-wise through `STRING_ELT`, which is
what every competitor does.

### 3.4 Workers may write into R memory; they may not call R

The rule "do not let workers write into R-owned memory" forces one extra copy
of every decoded block on every read. The property that actually matters is
that no R API is touched off the R thread. The R thread allocates the
destination vector; Rust splits `&mut [f64]`/`&mut [i32]` into disjoint block
slices with `chunks_mut`; `std::thread::scope` workers decompress and decode
straight into them. R's GC does not move memory and the vector is protected for
the duration. `fst` and `qs2` both decode directly into column memory. Adopt
this for reads; on writes, workers read from `&[i32]` borrowed R slices the
same way (copy only when a transform needs an owned buffer).

### 3.5 Streaming generic bridge moves from Phase 8 to the second milestone

`R_Serialize()` with an `R_outpstream_st` whose `OutBytes` callback feeds the
block writer, and `R_Unserialize()` with an `R_inpstream_st` pulling from the
block reader, is about 150 lines of C plus a Rust ring buffer. It removes the
whole-payload raw allocation on both sides, lets the generic stream use the
same compression and thread pipeline, and is the only way to match `qs2` on
closures, environments, S4, and everything else that will never be native. It
is small, independent of every type codec, and unblocks honest comparisons, so
it should not wait.

### 3.6 Compression is a layer, not a per-type experiment

No size claim is possible without it, and above page-cache sizes, bytes *are*
time. Add two codecs behind the existing `compression_id` field:

- LZ4 via `lz4_flex` (pure Rust, safe, within a few percent of C LZ4);
- Zstandard via `zstd-sys`/`zstd-safe` (C, vendored; `cc` is already in the
  vendor set). `ruzstd` is decode-only/slow-encode and not a substitute.

Plus one transform: byte shuffle (transpose) for 4- and 8-byte elements with
a sampled decision (compress a 32 KiB sample both ways with LZ4, keep the
smaller). Raw fallback whenever the stored size is not smaller. Presets map to
policies: `speed` = shuffle+LZ4 or raw, `balanced` = shuffle+zstd 1–3,
`compact` = zstd 9+ with wider transform search. Block size policy: 256 KiB to
1 MiB per block, at least two blocks per worker for large vectors.

### 3.7 Threads are a layer too

One RDZ-owned pool built on `std::thread::scope` (no dependency), bounded
queue, sequence-numbered blocks, ordered writer, `read_at`/`seek_read` for
concurrent block reads, cooperative cancellation with first-error
propagation. Default thread count: `getOption("rdz.threads")`, otherwise 1
when `_R_CHECK_LIMIT_CORES_` is set, otherwise half the physical cores capped
at 8. Compressors run single-threaded inside workers; never nest.

### 3.8 Strings are the real fight

Every R serializer is bottlenecked by `mkCharLenCE()` and the global CHARSXP
cache on read, and by per-element `STRING_ELT` traversal on write. Techniques
to apply, in order:

1. Write: pointer-identity dictionary. Hash the CHARSXP *pointer*, not the
   bytes, while packing a block; repeats cost a hash-map probe and no string
   comparison. Emit a dictionary block when distinct/total is below a
   threshold, otherwise offsets plus bytes.
2. Read: dictionary blocks create each distinct CHARSXP once and reuse the
   SEXP for every repeat with `SET_STRING_ELT` — this skips the cache lookup
   entirely for repeats and is where low-cardinality columns beat `fst` and
   `qdata` by a wide margin.
3. Read: exploit `IS_ASCII`-style knowledge from the writer (store an ASCII
   flag per block) so the reader can pass `CE_NATIVE`/ASCII without
   validation.
4. Later: FSST for high-cardinality blocks, and an ALTREP lazy string vector
   that materializes CHARSXPs on first access (a genuine feature competitors
   lack; keep it behind an option because ALTREP strings surprise some code).

### 3.9 Feature targets that decide "most features"

| Feature | qs2 | qdata | fst | RDZ target |
|---|---|---|---|---|
| Every base-R-serializable object | yes | no (drops to NULL) | no | yes (fallback) |
| Exact attributes, any class | yes | partial | fixed set | yes (native map + fallback) |
| Reference sharing, envs, closures | yes | no | no | yes (fallback) |
| Data-frame column projection | no | no | yes | yes (M6) |
| Row-range reads | no | no | yes | yes (M6, per block) |
| Schema/metadata without reading data | no | no | yes | yes (directory) |
| Selective attribute reads | no | no | partial | yes |
| Per-block checksums on data | whole file | whole file | metadata only | yes |
| Atomic replace, permissions preserved | no | no | no | yes |
| Cross-endian portable | no | no | yes | yes |
| Compression presets, shuffle | yes | yes | yes | yes (M3) |
| Multithreaded read and write | yes | yes | yes | yes (M4) |
| Serialize to/from raw vector | yes | yes | no | yes (cheap; add) |
| Long vectors (> 2^31−1) | yes | yes | no | yes (u64 lengths already) |
| ALTREP compact sequences kept compact | yes (R does) | no | no | yes |
| Lazy ALTREP string/numeric columns | no | no | no | later, optional |
| `data.table` class + keys preserved | yes | partial | keys | yes |

## 4. Proposed roadmap

Sizes are relative (S ≈ days, M ≈ one to two weeks, L ≈ two to four weeks
of focused work). Each milestone ends with a recorded benchmark run and a doc
update; nothing else is a gate.

### M0 — Instrumentation and baseline (S)

- `RDZ_TRACE=1` prints per-phase wall time (traverse, pack, transform,
  compress, checksum, write, R allocate, decode) to stderr from Rust; this is
  the primary investigation tool and costs nothing when off.
- Profiling recipe in `tools/`: `xctrace`/Instruments on macOS and `perf` on
  Linux with `CARGO_PROFILE_RELEASE_DEBUG=1`.
- Add Tier 3 workloads (below) and the small-object Tier 4 to the harness;
  add `arrow::write_feather()`/`read_feather()` as a memcpy-speed reference
  for data frames (not a competitor claim; a ceiling).
- Peak RSS via a subprocess `/usr/bin/time -l` (macOS) / `-v` (Linux) runner.
- Record the baseline for this review (see §7).

### M1 — Native skeleton for everything (L)

- Types: NULL, logical (done), integer, double, character, raw, complex,
  list; general attribute map (§3.2); ALTREP policy (§3.3); long lengths.
- Encodings: raw LE `i32`/`u64` bits/`u8`, string offsets+bytes with
  per-element encoding tags, list child records, compact-sequence record.
- Directory: object entries for every node including attribute values;
  data-frame column descriptors derivable from list children plus `names`.
- Reads decode directly into R vectors (§3.4, single-thread for now).
- `rdz_schema()` returns the recursive schema; `rdz_attributes()` works for
  any object path.
- Tests: generated round-trip matrix (§5.2) and the dataset-corpus differential
  test. Exit: `mode = "native"` succeeds on every corpus object without
  reference semantics; `mode = "auto"` is identical to base R on all of them;
  Tier 2/3 numbers recorded against `qdata` and uncompressed `fst`.

### M2 — Streaming generic bridge (S–M)

`R_Serialize`/`R_Unserialize` through blocks (§3.5); remove the R-level
`serialize()`/`unserialize()` and move the synopsis into the bridge. Exit:
forced-generic within noise of `qs2` at `compress_level = 0`-equivalent on the
corpus, and no whole-payload allocation on either side.

### M3 — Compression layer (M)

§3.6. Exit: `balanced` within ±10 % of `qs2`/`qdata` size on the Tier 3
suite; `speed` still faster than `fst` at its default on full read+write.

### M4 — Parallel pipeline (M)

§3.7. Exit: thread-scaling matrix at 1/2/4/8 recorded; beats `fst` full
read and full write at equal threads on at least four of five Tier 3
families; peak RSS bounded by `workers × block × queue factor` plus the
result.

### M5 — Strings and factors (M)

§3.8 items 1–3; narrow factor codes; `character_high_cardinality` and
`character_low_cardinality` families become the gate. Exit: string-heavy
Tier 3 family beats `fst` and `qdata`.

### M6 — Selective reads and overlays (M)

Column projection and row-range reads for data frames from the directory;
attribute reads by object path; `data.table` overlay (`.internal.selfref`
omission, keys/indices preserved); `rdz_serialize()`/`rdz_unserialize()` to
raw. Exit: metadata benchmarks report bytes touched; a 1 000-column frame's
schema read touches only the directory.

### M7 — Evidence-driven transforms (ongoing, S each)

Frame-of-reference/bit-packing and delta for integers, byte-stream split and
ALP for doubles, run-end for sorted factors, FSST for strings, dictionary
across columns. Each is a one-day experiment against the Tier 3 suite with an
adopt/defer/reject entry in the decisions log; adopted ones get an encoding
ID, malformed-input tests, and a fixture.

### M8 — Hardening and 0.1.0 freeze (M)

`cargo-fuzz` targets for the container and each decoder (separate dev crate,
not vendored); byte-mutation tests over every fixture; cross-OS artifact
exchange in CI; reader limits reviewed; opt-in `sync = TRUE`; CRAN packaging
with `zstd` copyright in `inst/COPYRIGHTS`; compatibility fixtures frozen.

## 5. Granularity: how to investigate, test, and benchmark

### 5.1 Investigate

Every performance idea follows the same four steps, time-boxed to one day
unless the first step already shows a large gap:

1. **Hypothesis with a number.** "Byte shuffle + LZ4 will cut `numeric_rounded`
   stored bytes by ≥ 40 % at ≤ 15 % write-time cost."
2. **Kernel measurement (Tier 0).** A pure-Rust timing over owned buffers,
   run with `cargo test --release -- --ignored --nocapture` timers or a small
   `tools/rust-bench` binary. Decides whether the idea deserves integration.
3. **Component measurement (Tier 1).** Container write/read of owned buffers
   without R, isolating framing, checksum, and IO.
4. **End-to-end (Tier 2/3).** The public R API. Only this level supports a
   claim, because it includes R allocation, `mkChar`, `.Call`, and the file
   system.

Record every experiment — including rejected ones — in one decisions log
(merge `research.md` and `encoding-research.md` into it) so the same question
is not reopened.

Use `RDZ_TRACE` before optimizing anything. The phase timers say whether time
is in R traversal, packing, compression, CRC, IO, or R allocation; guessing
has already cost the logical slice weeks of SIMD work on a type that is a
rounding error in real tables.

### 5.2 Test

Layers stay as they are (pure Rust, Savvy boundary, testthat), with four
additions that scale coverage without hand-writing a test per type:

1. **Generated round-trip matrix.** A `helper-objects.R` registry of object
   generators × sizes (0, 1, block−1, block, block+1, 3 blocks) × modes
   (`auto`, `native`, `r`) × attribute overlays (none, names, class, custom,
   dim/dimnames), asserted with `expect_identical()`. One test file walks the
   matrix; new types register a generator.
2. **Dataset-corpus differential test.** Every object from `data(package =
   .packages(all.available = TRUE))` plus a curated list of package datasets
   (`ggplot2`, `nycflights13`, `palmerpenguins`, `Lahman`, `babynames` when
   installed): `identical(read_rdz(write_rdz(x)), x)` in `auto`, and
   `rdz_info()$codec` recorded so coverage (native versus fallback share) is a
   tracked number, not a guess. This is the cheapest broad regression net
   available and directly measures §3.2.
3. **Byte-mutation tests.** For every golden fixture, flip each byte of the
   header/directory/trailer and truncate at every offset up to a bound; assert
   an R error and no panic. Generalizes the hand-written malformed cases.
4. **Fuzzing.** `cargo-fuzz` in a separate `fuzz/` crate (nightly, not
   vendored, not run by CRAN) seeded with the fixtures; run locally before
   each milestone and in a scheduled CI job.

Rules: a PR adds tests only for what it touches; the matrix and the corpus
catch the rest. Reference objects (environments, closures) use behaviour
assertions, as today. Cross-OS fixtures are exchanged as CI artifacts from
M8 on.

### 5.3 Benchmark

Five tiers, each with a purpose and a cadence:

| Tier | What | Purpose | Cadence |
|---|---|---|---|
| 0 | Rust kernel timers on owned buffers | choose transforms/codecs | ad hoc |
| 1 | Container write/read without R | isolate framing/CRC/IO | ad hoc |
| 2 | Per-type public API (`benchmark_serialization_matrix`) | per-type regression gate | per PR, for touched types |
| 3 | Workload suite (below) vs `fst`, `qs2`, `qdata`, base, feather | the headline; thread scaling; peak RSS | per milestone |
| 4 | Small-object latency (1, 100, 10 000 elements; nested lists) | wrapper and fixed overhead | per milestone |

Tier 3 workload families, deterministic seeds, 1e5 / 1e6 / 1e7 rows:

- `numeric_wide`: 200 double columns, mixed rounded/random/monotonic;
- `mixed_typical`: flights-like — ints, doubles, low-cardinality strings,
  factors, dates, POSIXct, logicals, 10 % NA;
- `string_heavy`: 60 % character columns, half high-cardinality IDs, half
  categorical;
- `narrow_many`: 500 low-cardinality integer columns (scheduling stress);
- `real_datasets`: `nycflights13::flights`, `ggplot2::diamonds`, a
  `data.table` with keys, a tibble.

Report per family: write ms, read ms, bytes, ratio, peak RSS, and threads
1/2/4/8; present speed versus size as a Pareto plot rather than one preset.
Keep the existing rules: checksum-on competitors, equal threads, warm cache
labelled, results outside the repository with the exact command in the PR.

## 6. Techniques catalogue and where they land

| Technique | Applies to | Milestone |
|---|---|---|
| Typed structure tree + grouped columnar payloads (qdata) | all | M1 |
| Decode straight into R memory from workers | all | M1/M4 |
| Streaming `R_Serialize` bridge (qs2) | fallback | M2 |
| LZ4 / zstd per block, raw fallback (fst, qs2, Blosc) | all | M3 |
| Byte shuffle with sampled decision (fst, qs2, Blosc) | int, double | M3 |
| Bounded ordered pipeline, `read_at` parallel reads (fst) | all | M4 |
| Pointer-identity string dictionary; CHARSXP reuse | character, factor | M5 |
| Narrow factor codes (fst) | factor | M5 |
| Column projection, row-range reads via block index (fst) | data frame | M6 |
| Compact-sequence record (R ALTREP) | int, double | M1 |
| Constant / run-end / dense-plane modes (done for logical) | logical | done |
| FOR + bit-packing, delta miniblocks (Parquet, ORC) | int | M7 |
| Byte-stream split, ALP/ALP-RD (Parquet, DuckDB) | double | M7 |
| FSST (DuckDB) | character | M7 |
| Lazy ALTREP columns | character, double | post-0.1 |
| `cargo-fuzz`, byte mutation | decoders | M8 |

## 7. Baseline recorded for this review

Machine: Apple M1, 8 cores, 16 GiB; R 4.6.1; Rust 1.88; `qs2` 0.2.2
(level 3, shuffle on, checksum validated); `fst` 0.9.8 / `fstcore` 0.10.0
(compress 50); one thread; warm cache; `bench::mark()` medians. Command:

```sh
RDZ_BENCH_SIZE=1000000 RDZ_BENCH_ITERATIONS=5 RDZ_BENCH_WARMUPS=1 \
RDZ_BENCH_MEMORY=false Rscript tools/benchmark.R
RDZ_BENCH_SIZE=100 RDZ_BENCH_ITERATIONS=50 RDZ_BENCH_WARMUPS=3 \
RDZ_BENCH_MEMORY=false Rscript tools/benchmark.R
```

### 1 000 000 elements/rows, representative suite, one thread

Median read (ms):

| object | rdz | qs2 | qdata | fst | base gz | base raw |
|---|---:|---:|---:|---:|---:|---:|
| logical (native) | 1.79 | 3.55 | 3.09 | 0.38 | 9.27 | 7.34 |
| integer | 8.29 | 3.97 | 3.95 | 6.81 | 22.47 | 7.07 |
| numeric | 14.15 | 9.91 | 9.43 | 2.93 | 42.03 | 9.93 |
| character | 109.2 | 71.3 | 41.2 | 43.8 | 255.1 | 232.2 |
| factor | 8.20 | 3.45 | 3.37 | 0.79 | 15.82 | 7.05 |
| list | 140.7 | 88.9 | 59.1 | — | 328.3 | 257.1 |
| data_frame | 144.7 | 94.2 | 61.8 | 54.9 | 344.0 | 264.0 |

Median write (ms):

| object | rdz | qs2 | qdata | fst | base gz | base raw |
|---|---:|---:|---:|---:|---:|---:|
| logical (native) | 0.96 | 9.53 | 9.48 | 1.03 | 218.9 | 8.01 |
| integer | 9.55 | 10.28 | 10.24 | 7.22 | 665.9 | 8.97 |
| numeric | 14.57 | 27.68 | 24.67 | 10.18 | 1315.5 | 11.96 |
| character | 71.6 | 87.2 | 56.7 | 40.0 | 509.2 | 178.2 |
| factor | 8.29 | 7.36 | 7.11 | 1.77 | 569.6 | 7.46 |
| list | 111.6 | 132.2 | 98.0 | — | 2706.6 | 203.7 |
| data_frame | 116.1 | 141.6 | 111.3 | 55.9 | 3276.3 | 202.5 |

File size (MiB):

| object | rdz | qs2 | qdata | fst | base gz |
|---|---:|---:|---:|---:|---:|
| logical | 0.240 | 0.220 | 0.219 | 0.242 | 0.273 |
| integer | 3.82 | 2.09 | 2.09 | 2.49 | 2.65 |
| numeric | 7.63 | 5.18 | 5.17 | 7.66 | 6.26 |
| character | 16.13 | 1.61 | 1.53 | 5.04 | 1.44 |
| factor | 3.82 | 0.82 | 0.82 | 0.96 | 1.26 |
| data_frame | 35.21 | 9.99 | 9.82 | 16.40 | 11.89 |

### 100 elements/rows (fixed overhead), medians in ms

| object | rdz read | qs2 read | rdz write | qs2 write | qdata write | fst write |
|---|---:|---:|---:|---:|---:|---:|
| integer | 0.046 | 0.042 | 0.332 | 0.089 | 0.084 | 0.142 |
| character | 0.057 | 0.047 | 0.343 | 0.088 | 0.084 | 0.143 |
| data_frame | 0.071 | 0.058 | 0.440 | 0.107 | 0.101 | 0.173 |

### What the baseline says

- **Data frames, the target, are 2.6× slower to read than `fst`, 2.3× slower
  than `qdata`, and 3.6× larger than `qdata`.** Every one of those gaps is a
  missing layer (typed columns, compression, threads), not a missing
  transform.
- **Character is the largest absolute cost everywhere** (109 ms of the
  145 ms data-frame read is the string column and its `mkChar` calls through
  `unserialize()`); §3.8 is where the data-frame race is won.
- **Small-object writes cost 0.33–0.44 ms against 0.08–0.10 ms for
  `qs2`/`qdata`.** Reads are within 20 %. The ~0.25 ms is R-level work in
  `write_rdz()`: the native attempt, `serialize()` of the payload, building
  and `serialize()`-ing the synopsis, and path checks. M2 removes it.
- **The native logical read (1.79 ms) does not hold the lead reported on
  2026-08-09 (0.99 ms vs `fst` 0.89 ms); `fst` measured 0.38 ms here.** The
  logical *write* still wins. Before touching the logical codec again, use
  `RDZ_TRACE` to see whether the read cost is fixed overhead (open, directory
  validation, names lookup) or decoding; a 1.4 ms gap on a 4 MiB result is
  almost certainly fixed overhead shared by every type.
- Uncompressed base R reads integers and doubles faster than RDZ (7.1 vs
  8.3 ms, 9.9 vs 14.2 ms) despite RDZ's direct IO, which confirms the R-side
  `unserialize()` of a raw vector plus CRC is not free and that the streaming
  bridge (M2) matters even before native codecs exist for those types.

## 7b. Is Rust the ceiling? Measured, 2026-09-27

The question "are we hitting a Rust-in-R limit?" was tested on the one native
path that already hands many R objects across the boundary: reading a
1 000 000-element `names` attribute (100 distinct ASCII strings plus `NA`).
Release builds, one thread, warm cache, `bench::mark()` medians:

| Variant | median |
|---|---:|
| Original: two `savvy::unwind_protect` per element, one `Vec<u8>` per string | 1 080 ms |
| One `unwind_protect` per block, `mkCharLenCE` from borrowed block bytes | **35 ms** |
| `qdata` reading the same strings as a character vector | 42–46 ms |
| `fst` reading the same strings as a one-column frame | 48 ms |
| Speed-of-light proxies: `levels(f)[f]` (cached CHARSXP + `SET_STRING_ELT`) / 8 MiB `memcpy` | 7.3 ms / 3.0 ms |

Conclusions:

- **There is no Rust ceiling.** The 30× gap was two of our own choices: a
  `setjmp`-based `R_UnwindProtect` context per R API call (savvy's
  `unwind_protect`, `set_na`, `set_elt` each create one), and one heap
  allocation per decoded string. With one protected region per block and
  borrowed bytes, the boundary runs at the `mkCharLenCE` floor, and RDZ is
  already the fastest of the three on this workload — *before* the
  dictionary/CHARSXP-reuse technique, whose floor is another ~5× lower
  (7.3 ms).
- **Rule for every boundary loop:** validate outside, then one
  `unwind_protect` around the whole batch with raw `savvy_ffi` calls inside.
  Never use per-element savvy setters in a hot path. Audit `rdz_read`'s
  names path and every future decoder for this pattern.
- **Numeric columns have no such headroom.** `fst` reads 1e6 doubles in
  2.9 ms, which is the `memcpy` speed-of-light measured in R (3.0 ms).
  Parity is the goal there; wins come from fewer copies, larger blocks, and
  parallel decode, not from a better kernel.
- **Where the wins are, in order:** strings and factors (5× available),
  fixed per-call overhead (0.25 ms on writes today), compression to shrink
  bytes moved, threads. Against `qs2` on closures/environments the target is
  parity: same `R_Serialize` + zstd architecture, and `R_Unserialize` is
  single-threaded for everyone.

Two methodology traps surfaced while measuring, both now recorded:

- `devtools::load_all()` defaults to `pkgbuild` debug mode, which builds the
  Rust crate with the `dev` profile. Three intermediate numbers taken that
  way were 3–6× inflated and were discarded. Every measurement must pass
  `debug = FALSE` (as `tools/benchmark.R` already does) or use `R CMD
  INSTALL`; the harness's `build_mode` field exists to catch this.
- `names(x) <- value` on a shared vector produced an ALTREP wrapper in one
  session, which strict native mode rejected as "an ALTREP logical vector".
  Ordinary R code creates ALTREP objects; §3.3 is not optional.

## 8. Housekeeping the review surfaced

- Guidance is ~4 000 lines for ~7 000 lines of code and every phase touches
  five documents. Consolidate to: `container-format.md` (normative spec),
  `architecture.md` (merged with `performance.md`), `roadmap.md`,
  `validation.md`, one decisions log (merged research documents), and a short
  dated `status.md` replacing `current-state.md`.
- `sexp-coverage.md` says it was audited against R 4.5.2; the toolchain is now
  R 4.6.1 (and `r_bridge.c` already handles the 4.6 `ATTRIB` change).
- `research.md` cites `qs2` 0.2.3; installed is 0.2.2. Pin the versions the
  benchmarks actually run.
- `review.md` at the repository root is the August review; move it under
  `.agents/` next to this one or delete it once its findings are all closed
  (they are).
- Add `lz4_flex`, `zstd-sys`/`zstd-safe` to the vendor archive and
  `inst/COPYRIGHTS` when M3 starts; check the archive size stays acceptable
  for CRAN.

## 9. Decisions needed before rewriting the roadmap

1. General native attribute map instead of per-type allowlists (§3.2).
2. ALTREP: materialize/region-read, never fall back (§3.3).
3. Workers write into R memory; only R API calls stay on the R thread (§3.4).
4. Zstandard via the C `zstd-sys` crate (vendored) alongside pure-Rust LZ4
   (§3.6), versus pure Rust only.
5. Default thread policy (§3.7).
6. Documentation consolidation (§8).

Once these are settled, rewrite `roadmap.md` around M0–M8, mark the logical
write-gap item as waived, and start M0.
