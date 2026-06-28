# Diverse R object benchmark

Run on x86-64 macOS with R 4.5.2, `qs2` 0.2.2, five measured
iterations, and `RDZ_BENCH_N=1000000`. Each format receives an isolated
serialized clone so materializing an ALTREP object in one implementation cannot
affect another. Garbage collection runs before, but outside, every timed call.

Each rdz result is compared with the fastest format that preserves the
object: default `qs2`, throughput-oriented `qs2` (`compress_level=-1000`, byte
shuffle disabled), or `qdata`. `qdata` is excluded for language objects,
pairlists, closures, environments, S4 objects, and model objects it cannot
preserve.

## Summary

- All 27 object classes passed round-trip validation.
- rdz won 25 of 27 write comparisons and all 27 read comparisons.
- Geometric-mean speedup: 3.61x write and 1.79x read.
- Native-codec geometric mean: 3.47x write and 2.15x read.
- General R-serialization fallback: 4.05x write and 1.06x read.
- The two write losses were repeated numerics (3.41 ms versus 3.05 ms) and
  repeated strings (59.81 ms versus 52.43 ms). rdz was still 1.62x and
  4.30x faster when reading those files.

## Per-object latency

`Write x` and `Read x` compare rdz with the fastest valid competing format;
values above 1 mean rdz is faster.

| Object | Codec | Write ms | Write x | Read ms | Read x |
|---|---|---:|---:|---:|---:|
| Numeric random | Native | 3.43 | 3.59 | 2.01 | 2.46 |
| Numeric repeated | Native | 3.41 | 0.89 | 2.06 | 1.62 |
| Integer random | Native | 1.62 | 7.29 | 1.04 | 2.39 |
| Integer sequence (ALTREP) | R serialization | 0.28 | 1.02 | 0.12 | 1.00 |
| Logical sparse | Native | 1.67 | 2.06 | 1.08 | 1.47 |
| Complex random | Native | 6.61 | 6.12 | 3.92 | 2.39 |
| Raw random | Native | 0.74 | 4.59 | 0.34 | 2.56 |
| Strings repeated | Native | 59.81 | 0.88 | 9.74 | 4.30 |
| Strings unique | Native | 51.18 | 1.12 | 93.45 | 1.10 |
| Factor repeated | Native | 1.79 | 3.90 | 0.99 | 2.10 |
| Named numeric | Native | 11.11 | 2.87 | 13.84 | 1.33 |
| Numeric matrix | Native | 3.26 | 7.30 | 1.76 | 2.79 |
| Integer matrix | Native | 1.57 | 7.94 | 0.93 | 2.72 |
| Numeric array | Native | 3.38 | 7.26 | 1.83 | 2.63 |
| Mixed data frame | Native | 16.85 | 1.91 | 3.49 | 4.45 |
| Wide data frame | Native | 3.43 | 7.14 | 3.53 | 2.13 |
| Nested list | Native | 3.50 | 3.62 | 3.12 | 2.27 |
| Dates | Native | 3.32 | 1.98 | 1.84 | 1.08 |
| POSIXct | Native | 3.24 | 1.85 | 1.71 | 1.16 |
| Time series | Native | 3.15 | 7.64 | 1.79 | 2.73 |
| Contingency table | Native | 1.64 | 7.42 | 0.99 | 2.59 |
| Language in list | R serialization | 3.61 | 6.58 | 4.39 | 1.11 |
| Pairlist | R serialization | 3.64 | 6.74 | 4.60 | 1.07 |
| Closure | R serialization | 3.80 | 6.43 | 4.27 | 1.14 |
| Environment | R serialization | 5.77 | 6.60 | 6.65 | 1.06 |
| S4 object | R serialization | 3.48 | 6.76 | 4.53 | 1.04 |
| Linear model | R serialization | 39.01 | 1.39 | 35.64 | 1.03 |

## File size

The default is deliberately latency-oriented and generally uncompressed:

- Geometric mean versus default `qs2`: 4.32x larger.
- Geometric mean versus throughput-oriented `qs2`: 0.91x as large.
- Geometric mean versus `qdata` on compatible objects: 4.84x larger.

The largest compression gaps are highly structured vectors: repeated numerics
(7.63 MiB versus about 950 bytes), POSIXct sequences (7.63 MiB versus 0.011
MiB), dates (7.63 MiB versus 0.026 MiB), and sparse logicals (3.81 MiB versus
0.047 MiB). These are candidates for optional RLE, delta/bit-packing, or a
compressed mode; applying those transformations unconditionally would weaken
the latency-first result.

Compact, unmaterialized ALTREP sequences now retain R serialization. A targeted
million-integer check produced a 145-byte rdz file, versus 137 bytes for
default `qs2` and 170 bytes for throughput-oriented `qs2`.
