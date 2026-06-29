# Data-frame benchmark against fst

This benchmark compares `rdz` speed and balanced presets with `fst` 0.9.8 at
compression levels 0 and 50. It uses `fstcore` 0.10.0 rebuilt with OpenMP,
libomp 22.1.8, R 4.5.2, and an x86-64 macOS host with six physical and six
logical cores. The benchmark ran with 1, 2, 4, and 6 effective fst threads,
seven seeded iterations per configuration, strict `identical()` validation,
and one million rows except for the 500,000-row unique-string case. Every rdz
file was checked to ensure it used the native codec.

The CRAN macOS fst binary used previously was not linked to OpenMP and resolved
every thread request to one. These results therefore use a temporary source
build linked to libomp; both requested and effective thread counts were checked
before every run.

## Importance of fst parallelism

The table reports fst's default compressed mode because that is where OpenMP
has the largest effect. The best time is selected from the 1-, 2-, 4-, and
6-thread runs. Speedup is the one-thread time divided by that best time.

| case | write 1t | best write | write scaling | read 1t | best read | read scaling |
|---|---:|---:|---:|---:|---:|---:|
| mixed | 77.33 ms | 60.41 ms (4t) | 1.28x | 67.56 ms | 58.11 ms (4t) | 1.16x |
| wide numeric | 41.78 ms | 41.78 ms (1t) | 1.00x | 23.02 ms | 15.63 ms (6t) | 1.47x |
| repeated | 39.85 ms | 32.13 ms (4t) | 1.24x | 48.26 ms | 38.32 ms (4t) | 1.26x |
| unique strings | 32.95 ms | 32.18 ms (2t) | 1.02x | 53.69 ms | 52.43 ms (4t) | 1.02x |
| temporal | 42.20 ms | 23.27 ms (4t) | 1.81x | 25.49 ms | 9.01 ms (6t) | 2.83x |

Geometric-mean best scaling was 1.24x for compressed writes and 1.44x for
compressed reads. Uncompressed fst did not scale materially: comparing one
with six threads gave geometric-mean factors of 1.02x for writes and 0.98x for
reads. Parallelism is therefore important for selected compression-heavy
workloads, but it is not the primary explanation for fst performance on every
data shape. Four threads were usually best for compressed writes; six threads
sometimes added overhead.

## Comparison with rdz

The table compares the fastest rdz preset with the fastest fst combination of
compression mode and thread count. rdz timings are the median of the four
per-thread-run medians, making them independent of fst's selected thread count.
A speedup above one favors rdz. The size ratio compares rdz balanced with fst
compression level 50.

| case | write speedup | read speedup | balanced/fst size |
|---|---:|---:|---:|
| mixed | 2.11x | 2.80x | 0.63x |
| wide numeric | 3.24x | 1.26x | 1.00x |
| repeated | 1.62x | 2.57x | 0.32x |
| unique strings | 1.22x | 1.03x | 0.79x |
| temporal | 2.41x | 1.69x | 0.30x |

rdz won all five writes and reads, although the unique-string read was
effectively tied at 47.95 ms versus 49.18 ms. Geometric-mean rdz speedups were
2.00x for writes and 1.73x for reads. With fst restricted to one thread, the
corresponding controlled speedups were 2.37x and 1.93x. Allowing fst its best
thread count therefore reduced rdz's aggregate lead by about 16% for writes and
10% for reads, without changing the winner.

Balanced rdz files remained 0.54x the size of default fst files geometrically.
The repeated frame was 2.03 MiB versus 6.40 MiB, the temporal frame was 4.65
MiB versus 15.60 MiB, and the unique-string frame was 6.66 MiB versus 8.47 MiB.

These results are specific to this machine and warm local storage. Run
`data-frames-fst.R` with `RDZ_FST_THREADS` set to the desired thread count to
measure the current checkout on other systems.
