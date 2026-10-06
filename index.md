# rdz

`rdz` writes and reads R objects in a versioned, seekable, checksummed
block container. Common objects are stored natively, column by column
and block by block, compressed with Zstandard on a pool of threads;
everything else goes through R’s own serializer, streamed through the
same blocks.

- **Native:** logical, integer, double and character vectors, factors,
  lists and data frames (data.table and tibble included), nested, with
  their attributes: names, classes, a Date’s or POSIXct’s metadata, a
  matrix’s dimensions. Each vector picks its own block records (runs,
  frame of reference, delta, byte planes, sparse and bitplane logicals,
  string dictionaries).
- **Generic:** anything else, such as environments, closures, calls,
  complex and raw vectors, and S4 objects, goes through R serialization
  as one whole root, so sharing and references are kept.
- **Selective:** `read_rdz(path, select = , rows = )` reads some columns
  of a data frame or elements of a list, and some rows of a data frame
  or elements of a vector, decoding only the blocks they need.
- **Inspectable:**
  [`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md)
  shows a native file’s tree (each part’s type, class, shape, attributes
  and size), and `rdz_attributes(path, object = c("sales", "date"))`
  reads any part’s attributes, without decoding its data.
- **Content-addressed:** every file records a 128-bit hash of the value
  (`rdz_info(path)$content_hash`, read without the data), and
  `rdz_hash(x)` gives the same hash for an object, whatever preset,
  threads or platform. `write_rdz(skip_unchanged = TRUE)` leaves a file
  holding the value as it is, and
  [`rdz_verify()`](https://pedrobtz.github.io/rdz/reference/rdz_verify.md)
  checks a file’s integrity without building it.
- **Compact graphs:** a large vector that appears twice in an object is
  stored once and read back shared, as pickle and Kryo do.
- **Archives:** `rdz_save(a, b, file = )` and
  `rdz_load(file, names = "b")` work as
  [`save()`](https://rdrr.io/r/base/save.html) and
  [`load()`](https://rdrr.io/r/base/load.html) do, and load only the
  objects named.
- **In memory:** `rdz_serialize(x)` gives the bytes as a raw vector (for
  Redis, a database, a socket) and
  [`rdz_unserialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)
  reads them back; every reader takes a raw vector where it takes a
  path.
- **Self-describing:**
  `write_rdz(x, path, metadata = c(source = "..."))` records key-value
  strings that
  [`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
  reads without the data.
- **Tables as written:** data.tables come back usable in place, nested
  or not, and tibbles as tibbles.
- **Checked:** every header, directory and block carries an XXH3-64
  checksum, and every length and offset is bounded before anything is
  allocated.
- **Portable:** files are byte-identical on every platform, little- and
  big-endian alike.

## Installation

rdz needs R 4.1 or newer and a C compiler. It links against
[zubin](https://github.com/pedrobtz/zubin) and
[zufast](https://github.com/pedrobtz/zufast), which are installed with
it.

``` r

# install.packages("pak")
pak::pak("pedrobtz/rdz")
```

## Usage

``` r

library(rdz)

path <- tempfile(fileext = ".rdz")
write_rdz(mtcars, path)
read_rdz(path)
read_rdz(path, select = c("mpg", "wt"))          # only these columns are read
rdz_schema(path)                                  # the tree, from the directory
rdz_attributes(path, names = "names")

options(rdz.preset = "compact", rdz.threads = 4)  # smaller files, more threads
write_rdz(mtcars, path, mode = "native")           # fail rather than fall back
```

## Compatibility

The file format froze at 0.1.0. Every later rdz reads 0.1.0 files, and a
writer adds new block records or attributes without changing the meaning
of existing ones; see `.agents/container-format.md`. Files written
before 0.1.0 have no compatibility guarantee.

## Serialization benchmark matrix

Run the default paired read/write benchmark with:

``` sh
Rscript tools/benchmark.R
```

The command prints median read and write milliseconds plus file sizes.
It records throughput, an R-allocation proxy, package/backend settings,
thread/checksum policies, and machine details in the returned object.
Control a single-size run through environment variables:

``` sh
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

``` sh
RDZ_BENCH_SIZES=1000,100000,1000000 \
Rscript tools/benchmark.R
```

Set `RDZ_BENCH_SUITE=expanded` to use the validation distributions for
logical, integer, numeric, character, factor, list, and data-frame
cases. This is substantially slower than the compact representative
suite.

For custom objects, load the package and source the helper:

``` r

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

The result contains `read_ms`, `write_ms`, `file_mib`, size ratios
against the in-memory object and uncompressed base R, read/write
throughput, read/write allocation-proxy matrices, long-form `details`,
backend settings, and environment metadata. Unsupported, missing,
failed, or non-identical format/object combinations are reported as `NA`
with a reason in `details`. The default matrix pins fst, qs2, and qdata
to one thread. It enables qs2/qdata checksum validation so their read
path is comparable with RDZ’s mandatory XXH3-64 validation.

Warm-cache reads are the portable default. A genuinely cold-cache run
requires `cache_mode = "cold_hook"`, a user-supplied
`cold_cache_hook(path)`, and a descriptive `cold_cache_label`; the hook
runs outside each one-iteration timed read. RDZ does not label
fresh-file or best-effort reads as cold-cache results.
