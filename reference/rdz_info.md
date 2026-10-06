# Inspect an rdz Container Without Reading Its Payload

`rdz_info()` reads the fixed header, closing trailer, and bounded
directory. For a file written through R serialization (the generic
codec), `synopsis` is informative and exact attribute values still
require
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

  A path, or a raw vector holding an rdz file
  ([`rdz_serialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)).

- x:

  An `rdz_info` object.

- ...:

  Additional arguments, currently unused.

## Value

A list of class `rdz_info`:

- `container_version`, `codec` (`"native_v1"` or `"r_serial_v3"`),
  `codec_id`, `codec_version`: the file's format versions and payload
  codec.

- `block_size` (the largest decoded block, in bytes), `block_count`,
  `object_count`, `attribute_count`: the directory's counts.

- `payload_bytes` (stored block bytes) and `file_bytes`.

- `root_type`, `root_length`, `attribute_names`: the root's type (a
  native type such as `"data.frame"`, or for a generic file R's
  [`typeof()`](https://rdrr.io/r/base/typeof.html)), its length (a data
  frame's number of columns) and its attributes' names.

- `writer`: the implementation and version that wrote the file, such as
  `"rdz 0.1.0"`, or `""` when the file does not record one.

- `content_hash`: the value's hash
  ([`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)),
  32 hexadecimal digits, or `NA` when the file records none.

- `metadata`: the user metadata
  [`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
  recorded, a named character vector (empty when there is none).

- `synopsis`: for a generic file, the bounded description of the root
  recorded when it was written (`NULL` for a native file).

- `authoritative`, `exact_attributes`: whether the directory describes
  the value exactly (native files) rather than through the synopsis;
  `full_read_required_for_attributes` is their opposite.

- `schema`: `root_type`, `length` and `attribute_names` as one list.

- `integrity_checks`: the checks `rdz_info()` makes: the header's and
  the directory's checksums and every offset and length in the
  directory. Block checksums are checked by
  [`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md)
  and
  [`rdz_verify()`](https://pedrobtz.github.io/rdz/reference/rdz_verify.md).

`x`, invisibly.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(data.frame(value = 1:3), path)
rdz_info(path)
#> <rdz_info>
#>   codec: native_v1 (version 1)
#>   container version: 3
#>   written by: rdz 0.1.0
#>   blocks: 3 (maximum 1048576 bytes)
#>   payload: 29 bytes
#>   metadata: authoritative native directory
unlink(path)
```
