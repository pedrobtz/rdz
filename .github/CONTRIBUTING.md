# Contributing to rdz

The design documents are in `.agents/`; `AGENTS.md` lists the gates a pull
request runs (the C harness, the mutation check, fuzzing, the R tests).

## Benchmarks

Run the default paired read/write benchmark with:

```sh
Rscript tools/benchmark.R
```

The command prints median read and write milliseconds plus file sizes. It records
throughput, an R-allocation proxy, package/backend settings, thread/checksum
policies, and machine details in the returned object. Control a single-size run
through environment variables:

```sh
RDZ_BENCH_SIZE=1000000 \
RDZ_BENCH_ITERATIONS=20 \
RDZ_BENCH_WARMUPS=1 \
RDZ_BENCH_THREADS=1 \
RDZ_BENCH_MEMORY=true \
RDZ_BENCH_OUTPUT=/tmp/rdz-benchmark-details.csv \
RDZ_BENCH_RDS_OUTPUT=/tmp/rdz-benchmark-result.rds \
Rscript tools/benchmark.R
```

Run the required small, medium, and throughput sizes with:

```sh
RDZ_BENCH_SIZES=1000,100000,1000000 \
Rscript tools/benchmark.R
```

Set `RDZ_BENCH_SUITE=expanded` to use the validation distributions for logical,
integer, numeric, character, factor, list, and data-frame cases. This is
substantially slower than the compact representative suite.

For custom objects, load the package and source the helper:

```r
devtools::load_all(".", quiet = TRUE, debug = FALSE, recompile = TRUE)
source("tools/bench-helpers.R")

benchmark_serialization_matrix(
  objects = list(
    integer = sample.int(1000L, 1e6, replace = TRUE),
    data_frame = data.frame(x = runif(1e6))
  ),
  iterations = 20L
)
```

The result contains `read_ms`, `write_ms`, `file_mib`, size ratios against the
in-memory object and uncompressed base R, read/write throughput, read/write
allocation-proxy matrices, long-form `details`, backend settings, and environment
metadata. Unsupported, missing, failed, or non-identical
format/object combinations are reported as `NA` with a reason in `details`.
The default matrix pins fst, qs2, and qdata to one thread. It enables qs2/qdata
checksum validation so their read path is comparable with RDZ's mandatory XXH3-64
validation.

Warm-cache reads are the portable default. A genuinely cold-cache run requires
`cache_mode = "cold_hook"`, a user-supplied `cold_cache_hook(path)`, and a
descriptive `cold_cache_label`; the hook runs outside each one-iteration timed
read. RDZ does not label fresh-file or best-effort reads as cold-cache results.
