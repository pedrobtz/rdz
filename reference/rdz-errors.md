# Errors rdz Raises

Every error rdz raises is a condition of class `rdz_error`, and of one
of these classes, so `tryCatch(..., rdz_error = )` catches them all and
a handler can tell them apart:

## Details

- `rdz_argument_error`: an argument is not what the function takes (a
  path that is not a string, `rows` that are not positive whole numbers,
  an unknown `select` name, an invalid option).

- `rdz_format_error`: the file is not a valid rdz file: a checksum does
  not match, a length or offset is out of bounds, a record is malformed,
  or the value does not match its content hash.

- `rdz_limit_error`: a count or length exceeds the format's limits.

- `rdz_version_error`: the file uses a container version this rdz does
  not read.

- `rdz_codec_error`: the file's payload codec is not one this rdz reads.

- `rdz_io_error`: the operating system refused a read or a write.

- `rdz_memory_error`: an allocation failed.

- `rdz_unsupported_error`: `mode = "native"` was given a value the
  native codecs do not take (see
  [`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md),
  "Native and generic").

Tests and code should test the class, never the message.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
writeBin(as.raw(1:10), path)
tryCatch(read_rdz(path), rdz_error = function(e) class(e)[[1L]])
#> [1] "rdz_format_error"
unlink(path)
```
