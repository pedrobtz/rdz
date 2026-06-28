# Data-frame benchmark against fst

This benchmark compares `rdz` speed and balanced presets with `fst` 0.9.8
at compression levels 0 and 50. It uses `fstcore` 0.10.0, R 4.5.2, one
explicitly configured and recorded fst thread, five seeded iterations, and
strict `identical()` validation. Every rdz file is checked to ensure it used
the native codec rather than the R serialization fallback. The installed macOS
fst binary exposes only one effective thread on this host: requesting either
one or all available threads reports one. Multithreaded fst therefore remains
unmeasured here.

The table compares the fastest rdz and fst mode for each operation. A
speedup above one favors rdz. The size ratio compares rdz balanced with
fst compression level 50; a ratio below one favors rdz.

| case | write speedup | read speedup | size ratio |
|---|---:|---:|---:|
| mixed | 2.17x | 2.85x | 0.63x |
| repeated | 1.81x | 2.84x | 0.32x |
| temporal | 5.47x | 2.04x | 0.30x |
| unique strings | 1.16x | 0.99x | 0.79x |
| wide numeric | 4.22x | 1.70x | 1.00x |

Across the five workloads, rdz won all writes and four of five reads; the
unique-string read result was effectively tied at 51.49 ms versus 51.03 ms. The
geometric-mean speedups were 2.53x for writes and 1.95x for reads. The
geometric-mean balanced/default-fst size ratio was 0.54x.

fst applies type-specific block codecs: 16 KB blocks for fixed-width columns
and 2047-value blocks for strings, with two-bit logical packing, byte-shuffled
integer compression, compact factor indexes, and LZ4 or ZSTD over numeric and
flattened string payloads. rdz packs logicals and integer ranges requiring
at most eight bits and buffers dictionary index output. Balanced flat strings
use independent 64 KiB LZ4 blocks and run-length encoded varints for the length
vector, selecting the compressed layout only when its complete stored size is
smaller. XOR-delta byte transposition reduced the unique-string frame, which
contains a uniform numeric column, to 6.66 MiB versus 8.47 MiB for default fst.
This is a size/latency tradeoff: balanced rdz wrote it in 78.09 ms and read
it in 61.31 ms, while speed-mode rdz and the fastest fst modes took 29.18 ms
versus 33.72 ms to write and 51.49 ms versus 51.03 ms to read. Compressible
numeric dictionary indexes use the same block codec; this reduced the repeated
frame from 2.27 to 2.03 MiB. The temporal frame fell to 4.65 MiB versus 15.60
MiB for default fst.

These results are specific to this machine and warm local storage. Run
`data-frames-fst.R` to measure the current checkout on other systems.
