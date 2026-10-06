# Verify an rdz File

`rdz_verify()` checks a whole file without building any R object: the
header, trailer and directory, then every block's checksum, without
decompressing it. With `content = TRUE` it also reads the value and
checks it against the content hash the file records.

## Usage

``` r
rdz_verify(path, content = FALSE)
```

## Arguments

- path:

  A single, non-missing path to verify.

- content:

  Whether to read the value and compare its
  [`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)
  with the stored one.

## Value

`path`, invisibly, when the file is sound; otherwise an `rdz_error` (an
`rdz_format_error` for a corrupt file).

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(mtcars, path)
rdz_verify(path, content = TRUE)
unlink(path)
```
