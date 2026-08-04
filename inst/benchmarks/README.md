# Benchmark guide

The benchmark scripts compare rdz with R serialization, qs2/qdata, and fst.
Treat CI timings as directional: shared runners are noisy, so retained results
must include the hardware, R version, package versions, thread counts, seeds,
and iteration count.

## Setup

Install the current checkout into an isolated library:

```sh
R CMD INSTALL --preclean --clean --library=/tmp/rdz-r-lib .
```

Install the comparison packages separately; benchmark-only dependencies are
not package dependencies:

```r
install.packages(c("bench", "data.table", "fst", "qs2"))
```

The taxi benchmark also requires `curl` when its cached input is absent.

## Commands

Main qs2/qdata comparison:

```sh
R_LIBS_USER=/tmp/rdz-r-lib \
  Rscript inst/benchmarks/benchmark.R
```

Broad object matrix:

```sh
R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_OUTPUT=/tmp/rdz-many-objects.csv \
  Rscript inst/benchmarks/many-objects.R
```

Data-frame comparison with fst:

```sh
R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_OUTPUT=/tmp/rdz-fst.csv \
  Rscript inst/benchmarks/data-frames-fst.R
```

Use smaller inputs for a smoke run:

```sh
R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_N=100000 \
  RDZ_BENCH_ITERATIONS=2 \
  Rscript inst/benchmarks/benchmark.R

R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_BENCH_N=100000 \
  RDZ_BENCH_ITERATIONS=2 \
  RDZ_BENCH_OUTPUT=/tmp/rdz-many-smoke.csv \
  Rscript inst/benchmarks/many-objects.R

R_LIBS_USER=/tmp/rdz-r-lib \
  RDZ_FST_N=100000 \
  RDZ_BENCH_ITERATIONS=2 \
  RDZ_BENCH_OUTPUT=/tmp/rdz-fst-smoke.csv \
  Rscript inst/benchmarks/data-frames-fst.R
```

The GitHub Actions benchmark workflow is manual-only and exposes the primary
workload sizes and iteration count as dispatch inputs.

## Environment variables

| Variable | Used by | Default | Purpose |
|---|---|---:|---|
| `RDZ_BENCH_ITERATIONS` | All scripts | 5 | Timed iterations per operation. |
| `RDZ_BENCH_N` | qs2 and object matrix | 2,000,000 / 1,000,000 | Base workload size. |
| `RDZ_FST_N` | fst comparison | 1,000,000 | Base row count. |
| `RDZ_FST_THREADS` | fst comparison | 0 | Requested fst threads; zero means automatic. |
| `RDZ_BENCH_OUTPUT` | Object matrix and fst | Temporary CSV | Retained CSV path. |

## Retained results

- [fst data-frame results](data-frames-fst-results.md)
- [many-object results](many-objects-results.md)

Update these result files rather than copying benchmark numbers into status or
roadmap documents.
