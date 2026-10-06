# Inspect an rdz Container Without Reading Its Payload

`rdz_info()` reads the fixed header, closing trailer, and bounded
directory. For the transitional R-serialization codec, `synopsis` is
informative and exact attribute values still require
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md).
Data block checksums are validated by
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md),
not by this metadata-only operation. `writer` names the implementation
and version that wrote the file, or is `""` when the file does not
record one.

## Usage

``` r
rdz_info(path)

# S3 method for class 'rdz_info'
print(x, ...)
```

## Arguments

- path:

  A single, non-missing path to inspect.

- x:

  An `rdz_info` object.

- ...:

  Additional arguments, currently unused.

## Value

A named list of container information and a bounded root synopsis.

`x`, invisibly.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(data.frame(value = 1:3), path)
rdz_info(path)
#> <rdz_info>
#>   codec: native_v1 (version 1)
#>   container version: 3
#>   written by: rdz 0.0.0 (development)
#>   blocks: 3 (maximum 1048576 bytes)
#>   payload: 29 bytes
#>   metadata: authoritative native directory
unlink(path)
```
