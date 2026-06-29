# rdz

`rdz` is an experiment in minimizing R object serialization latency. It
uses two codecs:

- A native codec for atomic vectors, strings, lists, matrices, and data frames.
  The speed preset stores values directly; the balanced preset adds selective
  bit-packing, dictionaries, sequences, XOR-delta byte transforms, and LZ4
  blocks. Sampled cardinality selects dictionary encoding for repeated strings
  and a batched flat layout for mostly unique strings.
- A direct, uncompressed R serialization stream for everything else.

The native reader memory-maps files on POSIX systems. The design intentionally
trades file size for latency and avoids compression work when speed is the
primary goal.

```r
write_rdz(object, "object.rdz")
object <- read_rdz("object.rdz")
```

`.rdz` is the conventional file extension. The on-disk version tags remain
`FASTRDS1` and `FASTRDS2`, so files created before the package rename remain
readable.

Use `codec = "native"` to require the fast native path, or `codec = "r"` to
force general R serialization. The default `"auto"` selects the native path
when it can preserve the object and otherwise falls back automatically.
ALTREP vectors deliberately use the R serialization path so compact sequences
and deferred representations are not materialized on disk.

Use `rdz_threads()` to report the maximum number of worker threads that rdz may
use for automatically parallelized operations. It reports online logical
processors, capped at rdz's internal maximum; small workloads may use fewer.
On Linux, the result also respects process CPU affinity and cgroup v1 or v2 CPU
quotas, including limits applied to containers and Kubernetes pods.

Use `explain_rdz()` to inspect the codec and exact strategy that would be
selected without retaining an output file:

```r
explain_rdz(object, preset = "balanced")
```

The result is a data frame with rows for the root, attributes, and list
elements. It reports types, lengths, selected strategies, and encoded byte
counts; the root byte count equals the complete file size. The function runs
the real serializer into a temporary stream, so it has approximately the same
scanning and compression cost as writing the object.

## Balanced encoding

The default `preset = "speed"` retains the original direct-storage format.
`preset = "balanced"` writes the versioned `FASTRDS2` format tag retained for
backward compatibility. Logical vectors
use two bits per value, including `NA`. Integer vectors needing at most eight
bits use a frame-of-reference representation with bit-packed offsets and a
reserved `NA` code only when needed. Numeric vectors are sampled before codec selection:
low-cardinality vectors use an exact bit-pattern dictionary with bit-packed
indexes. Balanced mode applies independent LZ4 blocks to the packed index stream
only when they reduce its complete size. Numeric arithmetic sequences use a
constant-delta representation only after a full bit-for-bit reconstruction
check. Other high-cardinality doubles are XORed with their predecessor and
byte-transposed in fixed 8,192-value blocks before LZ4 compression. A cheap
first-block control point and the complete payload must both save at least
12.5%; otherwise the vector remains direct and pays only a one-byte format tag.

High-cardinality character vectors use the flat layout. In balanced mode, the
flat payload is split into independent 64 KiB LZ4 blocks and string lengths use
run-length encoded varints when that representation is smaller. Compression is
selected only when the complete encoded layout, including block framing and
length metadata, is smaller than the raw layout. Incompressible strings remain
raw, and the speed preset retains its original bytes.

```r
write_rdz(object, "object.rdz", preset = "balanced")
```

For one million logical values cycling through `FALSE`, `TRUE`, and `NA`,
two-bit packing reduced the file from 4,000,026 to 250,027 bytes. In a
20-iteration focused benchmark, median write time improved from 1.60 to 1.36 ms
while read time increased from 0.81 to 0.93 ms.

For one million integers cycling through `1:100` and `NA`, offset packing
reduced the native vector payload from 4,000,026 to 875,033 bytes. In a focused
one-column data-frame benchmark, balanced mode read the packed file in 1.60 ms
versus 2.77 ms for `fst`; packing increased write time from 1.66 to 3.33 ms.
The adaptive eight-bit limit prevents this tradeoff from being selected for
wider integer ranges.

For one million values repeating `0`, `1`, and `NA`, balanced encoding reduced
the file from 8,000,026 to 1,099 bytes. In a 40-iteration focused comparison,
balanced rdz wrote it in 2.46 ms and read it in 2.18 ms; default qs2 took
2.90 ms and 3.45 ms. A random 64-value vector retained raw packed indexes:
balanced rdz wrote and read it in 6.50 ms and 3.08 ms versus 32.97 ms and
12.81 ms for default qs2.

For one million consecutive dates, constant-delta encoding reduced the file
from 8,000,067 bytes to 84 bytes. Median write time improved from 9.36 to 1.44
ms and read time from 1.81 to 0.96 ms. The corresponding POSIXct file was 137
bytes. Selection is exact rather than heuristic: a near-sequence with a late
deviation remains direct, but balanced mode must scan it fully before rejecting
the sequence codec.

For one million high-cardinality doubles, XOR-delta byte transposition reduced
uniform random data to 0.61x, a noisy trend to 0.59x, and a random walk to
0.79x of direct storage. Marginally compressible normal data and random bit
patterns remain direct. On the mixed atomic-list qs2 benchmark, balanced
rdz produced a 17.40 MiB file versus 30.52 MiB for speed mode and 13.87 MiB
for default qs2; it wrote in 93.1 ms and read in 28.9 ms versus 251.5 ms and
47.7 ms for default qs2.

For the 500,000-row unique-string data frame in the fst benchmark, balanced
encoding reduced the file from 17.64 MiB to 6.66 MiB, versus 8.47 MiB for fst.
The added compression is a size/latency tradeoff. In the OpenMP comparison,
speed-mode rdz wrote in 26.44 ms versus fst's fastest 32.18 ms and read in
47.95 ms versus fst's fastest 49.18 ms.

## Benchmark

Run the reproducible comparison against `qs2` and its `qdata` format with:

```sh
Rscript inst/benchmarks/benchmark.R
```

For the data-frame comparison against `fst`, run:

```sh
Rscript inst/benchmarks/data-frames-fst.R
```

The script requests all available fst threads by default and records both the
requested and effective counts. Set `RDZ_FST_THREADS=1` for a single-threaded
control; availability depends on how fst was built.

Its methodology and latest results are documented in
`inst/benchmarks/data-frames-fst-results.md`.

With an OpenMP-enabled fst build on six cores, compressed fst improved by 1.24x
for writes and 1.44x for reads geometrically when each workload could use its
best thread count. The effect ranged from negligible for unique strings to
1.81x writes and 2.83x reads for temporal data. Uncompressed fst did not scale
materially. Against each workload's fastest fst mode and thread count, rdz
remained 2.00x faster for writes and 1.73x for reads geometrically.

Dictionary indexes are emitted through a 64 KiB buffer. For a one-million-row
column cycling through 100 strings, buffering reduced median write time from
58.4 to 13.0 ms without changing the 1,001,973-byte file; `fst` wrote the same
workload in 23.5 ms.

The broader 27-object correctness and performance matrix is available in
`inst/benchmarks/many-objects.R`; its latest methodology and results are in
`inst/benchmarks/many-objects-results.md`.

The benchmark includes base R's default `saveRDS()` and `readRDS()`, default
`qs2`, `qdata`, and a throughput-oriented `qs2` configuration
(`compress_level = -1000`, byte shuffle disabled). Set `RDZ_BENCH_N` and
`RDZ_BENCH_ITERATIONS` to control its size.

On R 4.5.2, qs2 0.2.2, and an x86-64 macOS machine, a 30.5 MiB
numeric/integer/logical list produced these medians:

| format | write | read | file size |
|---|---:|---:|---:|
| rdz speed | 13.6 ms | 11.2 ms | 30.52 MiB |
| rdz balanced | 93.1 ms | 28.9 ms | 17.40 MiB |
| qs2 default | 251.5 ms | 47.7 ms | 13.87 MiB |
| qs2 throughput | 69.2 ms | 18.6 ms | 30.29 MiB |
| qdata default | 249.1 ms | 45.4 ms | 13.87 MiB |

These are latency results, not a universal claim: compression can win when
storage bandwidth is constrained, and results depend on object shape and
hardware.

## Format limitations

- Native files use the writer's byte order and C numeric widths. The reader
  rejects files from incompatible architectures.
- The format currently has no checksum and is not intended as an archival
  format.
- Native encoding does not preserve shared list-node identity. Recursive
  objects and unsupported R types use the general R serialization codec.
