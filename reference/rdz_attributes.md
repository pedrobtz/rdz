# Read rdz Object Attributes

Native attributes are independently addressable: `rdz_attributes()`
reads the directory and the requested attributes' own blocks, for the
root or for any part below it, never the data of the object that holds
them. Generic files require `allow_full = TRUE`, which explicitly
permits a complete object read.

## Usage

``` r
rdz_attributes(path, object = NULL, names = NULL, allow_full = FALSE)
```

## Arguments

- path:

  A single, non-missing path to inspect.

- object:

  The object whose attributes to read: `NULL` or `0` for the root, or a
  path from the root as for `[[`, one name or position per level, such
  as `c("sales", "date")` or `list("models", 2)`.

- names:

  Optional character vector selecting attribute names.

- allow_full:

  Whether generic files may be fully deserialized.

## Value

A named list of attribute values.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(list(when = as.POSIXct("2026-10-06 12:00", tz = "UTC")), path)
rdz_attributes(path, object = "when")
#> $class
#> [1] "POSIXct" "POSIXt" 
#> 
#> $tzone
#> [1] "UTC"
#> 
unlink(path)
```
