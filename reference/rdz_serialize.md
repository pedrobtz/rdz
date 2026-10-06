# Serialize an R Object to a Raw Vector

`rdz_serialize()` gives the bytes
[`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
would write, as a raw vector, for a database column, a key-value store
such as Redis, or a socket; `rdz_unserialize()` reads them back.
Everything else takes a raw vector where it takes a path:
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md)
(`select` and `rows` included),
[`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md),
[`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md),
[`rdz_attributes()`](https://pedrobtz.github.io/rdz/reference/rdz_attributes.md)
and
[`rdz_verify()`](https://pedrobtz.github.io/rdz/reference/rdz_verify.md).

## Usage

``` r
rdz_serialize(
  x,
  mode = c("auto", "native", "r"),
  metadata = NULL,
  compress = getOption("rdz.compress", 1L),
  hash = getOption("rdz.hash", TRUE)
)

rdz_unserialize(bytes, select = NULL, rows = NULL)
```

## Arguments

- x:

  An R object to serialize.

- mode:

  Codec selection. `"auto"` uses a native codec only when the complete
  value is supported and otherwise uses whole-root R serialization;
  `"native"` rejects unsupported values; `"r"` forces R serialization.

- metadata:

  `NULL`, or user metadata to record with `x`: a named character vector
  (or a named list of single strings), up to 1,024 distinct non-empty
  names and 64 KiB in all, such as a source, a code version or a cache
  key's inputs.
  [`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
  reads it back, as `metadata`, without reading `x`; it is not part of
  the content hash.

- compress:

  The Zstandard level of each block, a whole number from `0` (none) to
  `19`; by default `options(rdz.compress)`, else `1`.

- hash:

  Whether to record the content hash
  ([`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)),
  from `options(rdz.hash)`, `TRUE` by default. Computing it reads the
  value once more (about a tenth of a write's time); without it the file
  is just as valid and as readable, but `skip_unchanged`,
  `rdz_verify(content = TRUE)` and `rdz_info()$content_hash` (then `NA`)
  have nothing to use.

- bytes:

  A raw vector holding an rdz file.

- select, rows:

  As for
  [`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md).

## Value

`rdz_serialize()`: a raw vector. `rdz_unserialize()`: the object.

## Examples

``` r
bytes <- rdz_serialize(mtcars)
identical(rdz_unserialize(bytes), mtcars)
#> [1] TRUE
rdz_unserialize(bytes, select = "mpg", rows = 1:3)
#>                mpg
#> Mazda RX4     21.0
#> Mazda RX4 Wag 21.0
#> Datsun 710    22.8
```
