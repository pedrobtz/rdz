# Inspect an rdz Object Schema

`rdz_schema()` reads the bounded directory without reading object data
blocks. Native schemas are authoritative; generic schemas are bounded
synopses and are marked non-authoritative.

## Usage

``` r
rdz_schema(path, recursive = TRUE)
```

## Arguments

- path:

  A path, or a raw vector holding an rdz file
  ([`rdz_serialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)).

- recursive:

  Whether `objects` describes every level (`TRUE`) or the root and its
  own parts.

## Value

A list of class `rdz_schema`: `codec`, `authoritative`,
`exact_attributes`, `root_type`, `length` and `attribute_names` as in
[`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md);
`data_blocks_read`, always `FALSE`; and `objects`, for a native file a
data frame with a row per part, depth first (`NULL` for a generic file):

- `path`: the part's place, such as `"$sales$day"` or `"$models[[2]]"`
  (`""` for the root).

- `depth`: 0 for the root.

- `type`: its native type; `class`: its classes, joined by `/`.

- `length`: elements (a list's parts, a data frame's rows); `columns`: a
  data frame's columns, else `NA`; `shape`: its dimensions as text.

- `attributes`: its attributes' names, joined by `, `.

- `stored_bytes`: the bytes its blocks take in the file, its parts'
  included.

- `shared_with`: for a part stored once and met again (a shared large
  vector), the path of its first occurrence, else `NA`.

- `id`: its object ID in the directory.

## Details

For a native file, `objects` describes the stored object as a tree: one
row per part (the root, each list element and data frame column,
recursively), with its path from the root, type, class, length,
attribute names and the stored bytes of everything below it. Only the
directory and the parts' names and classes are read, never their data.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(list(a = 1:3, b = data.frame(d = Sys.Date() + 0:1, x = c(1.5, 2))), path)
rdz_schema(path)
#> <rdz_schema> native_v1
#>   <root>: list 2  (104 B)  [names]
#>     $a: integer 3  (9 B)
#>     $b: data.frame 2 x 2  (73 B)  [names, row.names, class]
#>       $d: Date 2  (35 B)  [class]
#>       $x: numeric 2  (16 B)
unlink(path)
```
