# Write an R Object to an rdz File

`write_rdz()` writes an object through the versioned `rdz` block
container. Logical, integer, double and character vectors, factors,
lists and data frames, nested and with their attributes, are stored
natively; anything else (in automatic mode) goes whole through R
serialization, streamed in blocks of 1 MiB. Every file records the
object's content hash
([`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)).

## Usage

``` r
write_rdz(
  x,
  path,
  mode = c("auto", "native", "r"),
  skip_unchanged = FALSE,
  metadata = NULL
)
```

## Arguments

- x:

  An R object to serialize.

- path:

  A single, non-missing path to write.

- mode:

  Codec selection. `"auto"` uses a native codec only when the complete
  value is supported and otherwise uses whole-root R serialization;
  `"native"` rejects unsupported values; `"r"` forces R serialization.

- skip_unchanged:

  Whether to leave an existing file untouched (its bytes and
  modification time) when it already holds `x` with the same `metadata`:
  when its stored content hash equals
  [`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)
  of `x` under the same `mode`. For caches and build tools that key on
  files.

- metadata:

  `NULL`, or user metadata to record with `x`: a named character vector
  (or a named list of single strings), up to 1,024 distinct non-empty
  names and 64 KiB in all, such as a source, a code version or a cache
  key's inputs.
  [`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
  reads it back, as `metadata`, without reading `x`; it is not part of
  the content hash.

## Value

`path`, invisibly.

## Details

Two options control how blocks are stored and how many threads do the
work. `options(rdz.preset = )` is `"balanced"` (the default; each block
is compressed with Zstandard at level 1), `"compact"` (level 6) or
`"speed"` (no compression). A block is stored compressed only when that
makes it smaller. `options(rdz.threads = )` sets the threads that
compress and, in
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md),
decompress blocks; the default is 1. The file does not depend on either:
any setting reads any file, and the same object written with any number
of threads gives the same bytes.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(list(answer = 42L), path)
read_rdz(path)
#> $answer
#> [1] 42
#> 
unlink(path)
```
