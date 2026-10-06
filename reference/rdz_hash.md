# Content Hash of an R Object

`rdz_hash()` is the 128-bit hash of `x` as
[`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
would store it, and every file it writes records that hash in its
directory, where
[`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
reads it as `content_hash` without touching the data. A value gives the
same hash on every platform and R version, whatever the level, threads,
block sizes or encodings chosen, and whether its vectors are ALTREP or
not; so a cache can key files by it, and compare an object with a file
without reading the file.

## Usage

``` r
rdz_hash(x, mode = c("auto", "native", "r"))
```

## Arguments

- x:

  An R object.

- mode:

  As for
  [`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md).

## Value

The hash, 32 hexadecimal digits.

## Details

Natively stored values are hashed from their canonical form: types,
lengths, attribute structure and values (doubles bit for bit, strings
with their encoding). Values written through R serialization are hashed,
as the digest package does, from R serialization version 2 without its
header (so ALTREP and the R version do not enter it); equal values then
hash equally only when R serializes them equally, which excludes, for
example, a closure and its byte-compiled copy. The hash identifies the
value as stored, so it depends on `mode`: a value written natively and
the same value written with `mode = "r"` hash differently. XXH3-128 is
not a cryptographic hash.

The hash also records sharing: a vector of 4 KiB or more that appears
twice in `x` as one object is stored once
([`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md),
"Native and generic"), and hashes as a reference to its first
occurrence. So `list(v, v)` and `list(v, v + 0)` are
[`identical()`](https://rdrr.io/r/base/identical.html) but hash
differently when `v` is that large; build values the same way to get the
same hash.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(mtcars, path)
identical(rdz_hash(mtcars), rdz_info(path)$content_hash)
#> [1] TRUE
unlink(path)
```
