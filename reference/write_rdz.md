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
  metadata = NULL,
  hash = getOption("rdz.hash", TRUE)
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

- hash:

  Whether to record the content hash
  ([`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)),
  from `options(rdz.hash)`, `TRUE` by default. Computing it reads the
  value once more (about a tenth of a write's time); without it the file
  is just as valid and as readable, but `skip_unchanged`,
  `rdz_verify(content = TRUE)` and `rdz_info()$content_hash` (then `NA`)
  have nothing to use.

## Value

`path`, invisibly.

## Details

Two options control how blocks are stored and how many threads do the
work. `options(rdz.preset = )` is `"balanced"` (the default; each block
is compressed with Zstandard at level 1), `"compact"` (level 6) or
`"speed"` (no compression). Under `"balanced"` a block is stored
compressed only when that saves at least an eighth of it, so data that
barely compresses reads at memory speed; under `"compact"`, whenever it
is smaller. `options(rdz.threads = )` sets the threads that compress
and, in
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md),
decompress blocks; the default is 1. The file does not depend on either:
any setting reads any file, and the same object written with any number
of threads gives the same bytes.

## Native and generic

In automatic mode (`mode = "auto"`), a value is written natively when
every part of it can be; otherwise the whole value goes through R
serialization. Native files support selective reads (`select`, `rows`)
and inspection below the root
([`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md),
[`rdz_attributes()`](https://pedrobtz.github.io/rdz/reference/rdz_attributes.md));
generic files are read whole. A value is written generically when it
holds, anywhere:

- a type other than logical, integer, double, character or list (complex
  and raw vectors, environments, functions, calls, S4 objects);

- a string longer than 1 MiB, or a non-ASCII string the session cannot
  convert to UTF-8 losslessly (unmarked strings in the C locale);

- a non-ASCII attribute name, or names, row names or a class carrying
  attributes of their own;

- a factor with names, or a data frame column of another length than its
  rows (a matrix of two or more columns);

- nesting deeper than 1,000 levels, or more than 1,000,000 parts.

Automatic mode also writes generically a value of at least 1,024 parts
that average less than 1 KiB of data each, where the native directory
would cost more than the data. `mode = "native"` writes those natively,
and raises an `rdz_unsupported_error` (see
[rdz-errors](https://pedrobtz.github.io/rdz/reference/rdz-errors.md))
for the others.

A vector of 4 KiB or more that appears more than once in `x` (the same
object, as after `y <- x`) is stored once and read back shared.

## Replacing files

The file is written to a temporary file beside `path` and renamed over
it at the end, so readers see the old file or the new one, never a
partial one, and an error or interrupt leaves `path` as it was. rdz does
not ask the operating system to flush the file to disk (no `fsync`), so
after a power failure the file may be missing or empty. On file systems
whose rename cannot replace a file (some Windows shares), the old file
is moved aside first, and a crash at that moment can leave it under a
name ending in `.backup`.

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
