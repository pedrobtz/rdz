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

  A single, non-missing path to inspect.

- recursive:

  Whether `objects` describes every level (`TRUE`) or the root and its
  own parts.

## Value

A named schema list.

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
