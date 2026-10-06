# Save and Load Named Objects

`rdz_save()` writes named objects into one rdz file, as
[`save()`](https://rdrr.io/r/base/save.html) does; `rdz_load()` assigns
them back into an environment, all or only those named. Loading some
reads only their parts of the file. The file is an ordinary rdz file
holding a named list:
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md)
reads it whole, and
[`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md)
lists what it holds.

## Usage

``` r
rdz_save(
  ...,
  list = character(),
  file,
  envir = parent.frame(),
  mode = c("auto", "native", "r"),
  skip_unchanged = FALSE,
  metadata = NULL,
  hash = getOption("rdz.hash", TRUE)
)

rdz_load(file, names = NULL, envir = parent.frame())
```

## Arguments

- ...:

  Objects to save, as names or symbols (as for
  [`save()`](https://rdrr.io/r/base/save.html)).

- list:

  A character vector naming further objects to save.

- file:

  A path to write or read (or, for `rdz_load()`, a raw vector from
  [`rdz_serialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)).

- envir:

  For `rdz_save()`, where to find the objects; for `rdz_load()`, where
  to assign them.

- mode, skip_unchanged, metadata, hash:

  As for
  [`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md).

- names:

  `NULL` (everything) or the names of the objects to load.

## Value

`rdz_save()`: `file`, invisibly. `rdz_load()`: the names of the objects
assigned, invisibly.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
a <- 1:3
b <- mtcars
rdz_save(a, b, file = path)
rm(a, b)
rdz_load(path, names = "b")
exists("b")
#> [1] TRUE
exists("a")
#> [1] FALSE
unlink(path)
```
