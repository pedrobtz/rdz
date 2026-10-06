# Using rdz

rdz writes R objects to files (or raw vectors) that can be read back
whole or in part. This article walks through a typical session: writing
and reading, reading only what you need, looking inside a file without
reading it, caching on content hashes, and keeping several objects in
one file.

``` r

library(rdz)
```

## Writing and reading

[`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
and [`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md)
work as [`saveRDS()`](https://rdrr.io/r/base/readRDS.html) and
[`readRDS()`](https://rdrr.io/r/base/readRDS.html) do.

``` r

path <- tempfile(fileext = ".rdz")
write_rdz(mtcars, path)
identical(read_rdz(path), mtcars)
#> [1] TRUE
```

Logical, integer, double and character vectors, factors, lists and data
frames, nested and with their attributes, are stored *natively*: column
by column, in checksummed blocks, each vector with the encoding that
suits it. Anything else (an environment, a function, a model with a
formula) goes through R’s own serializer in the same blocks. Either way
the value comes back as written.

``` r

fit <- lm(mpg ~ wt, data = mtcars)
write_rdz(fit, path)
rdz_info(path)$codec
#> [1] "r_serial_v3"
all.equal(read_rdz(path), fit)
#> [1] TRUE
```

`mode = "native"` makes
[`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
fail instead of falling back, for code that relies on selective reads,
and `mode = "r"` always uses R’s serializer.

## Compression and threads

`compress` is the Zstandard level of each block, from `0` (none) to
`19`, and `options(rdz.threads)` the number of threads; neither changes
what a reader needs: any setting reads any file.

``` r

set.seed(1)
sales <- data.frame(
  day = as.Date("2026-01-01") + 0:99999 %/% 100,
  store = factor(sample(c("north", "south", "east", "west"), 1e5, TRUE)),
  units = sample.int(50L, 1e5, TRUE),
  price = round(runif(1e5, 1, 100), 2)
)

sizes <- sapply(c(none = 0, default = 1, level_9 = 9), function(level) {
  write_rdz(sales, path, compress = level)
  file.size(path)
})
rds <- tempfile(fileext = ".rds")
saveRDS(sales, rds)
c(sizes, saveRDS = file.size(rds))
#>    none default level_9 saveRDS 
#>  917817  278811  278779  425618
```

Level 0 is the fastest to write and to read, and the largest; level 1,
the default, is nearly as fast and much smaller; higher levels spend
write time for smaller files, and read as fast. `options(rdz.compress)`
sets the default level. `options(rdz.threads = 4)` compresses and
decompresses blocks on four threads, and the file is byte for byte the
same whatever the number.

## Reading only what you need

`select` takes columns of a data frame (or elements of a list) by name
or position, and `rows` takes rows of a data frame (or elements of a
vector). Only the blocks that hold them are read and decoded.

``` r

write_rdz(sales, path)
read_rdz(path, select = c("day", "price"), rows = 1:5)
#>          day price
#> 1 2026-01-01 33.47
#> 2 2026-01-01 66.66
#> 3 2026-01-01 33.27
#> 4 2026-01-01  5.32
#> 5 2026-01-01 64.15
read_rdz(path, rows = c(99999, 100000))
#>          day store units price
#> 1 2028-09-26 north    22 89.72
#> 2 2028-09-26 north    15 31.33
```

The result is what `[` gives on the whole value, so classes, factor
levels and automatic row names come through as they would in memory.

## Looking inside a file

[`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
reads the header and directory, never the data.
[`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md)
lists a native file’s tree, with each part’s class, shape, attributes
and stored size.

``` r

report <- list(sales = sales, note = "Q1", models = list(fit = coef(fit)))
write_rdz(report, path)
rdz_schema(path)
#> <rdz_schema> native_v1
#>   <root>: list 3  (277,237 B)  [names]
#>     $sales: data.frame 100000 x 4  (277,123 B)  [names, row.names, class]
#>       $day: Date 100000  (453 B)  [class]
#>       $store: factor 100000  (25,046 B)  [levels, class]
#>       $units: integer 100000  (75,008 B)
#>       $price: numeric 100000  (176,568 B)
#>     $note: character 1  (7 B)
#>     $models: list 1  (67 B)  [names]
#>       $fit: numeric 2  (49 B)  [names]
```

[`rdz_attributes()`](https://pedrobtz.github.io/rdz/reference/rdz_attributes.md)
reads the attributes of any part, given its path through the tree as
names or positions, without decoding that part’s data.

``` r

rdz_attributes(path, object = c("sales", "store"))
#> $levels
#> [1] "east"  "north" "south" "west" 
#> 
#> $class
#> [1] "factor"
rdz_attributes(path, object = c("models", "fit"))
#> $names
#> [1] "(Intercept)" "wt"
```

Files can also carry their own key-value metadata, such as a source or a
code version, which
[`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
returns without reading the value.

``` r

write_rdz(sales, path, metadata = c(source = "pos-export", version = "2026.10"))
rdz_info(path)$metadata
#>       source      version 
#> "pos-export"    "2026.10"
```

## Caching on content

Every file records a 128-bit hash of the value it holds, and
[`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)
gives the same hash for an object in memory, whatever the level, the
threads or the platform. That makes a file its own cache key.

``` r

rdz_info(path)$content_hash
#> [1] "88b0d26b25185e5fdef96ff365608910"
rdz_hash(sales)
#> [1] "88b0d26b25185e5fdef96ff365608910"
```

With `skip_unchanged = TRUE`,
[`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
leaves a file that already holds the value untouched, bytes and
modification time alike, so build tools and caches keyed on files see no
change.

``` r

before <- file.mtime(path)
Sys.sleep(1)
write_rdz(sales, path, skip_unchanged = TRUE,
          metadata = c(source = "pos-export", version = "2026.10"))
identical(file.mtime(path), before)
#> [1] TRUE
```

[`rdz_verify()`](https://pedrobtz.github.io/rdz/reference/rdz_verify.md)
checks every block’s checksum without building the value, and with
`content = TRUE` it also checks that the value hashes to the stored
hash. It returns the path, invisibly, when the file is sound, and an
error says which check failed when it is not.

``` r

rdz_verify(path, content = TRUE)

damaged <- readBin(path, "raw", file.size(path))
damaged[200] <- xor(damaged[200], as.raw(0xff)) # one flipped byte
writeBin(damaged, path)
rdz_verify(path)
#> Error:
#> ! invalid rdz file: block 2 header does not match the directory
```

## In memory

[`rdz_serialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)
gives the file’s bytes as a raw vector, for a database column, a message
queue or a socket, and every reader takes a raw vector where it takes a
path.

``` r

bytes <- rdz_serialize(sales)
length(bytes)
#> [1] 278811
read_rdz(bytes, select = "units", rows = 1:3)
#>   units
#> 1    22
#> 2    39
#> 3    29
identical(rdz_unserialize(bytes), sales)
#> [1] TRUE
```

## Several objects in one file

[`rdz_save()`](https://pedrobtz.github.io/rdz/reference/rdz_save.md) and
[`rdz_load()`](https://pedrobtz.github.io/rdz/reference/rdz_save.md)
work as [`save()`](https://rdrr.io/r/base/save.html) and
[`load()`](https://rdrr.io/r/base/load.html) do, and
[`rdz_load()`](https://pedrobtz.github.io/rdz/reference/rdz_save.md)
reads only the objects you name.

``` r

archive <- tempfile(fileext = ".rdz")
rdz_save(sales, fit, file = archive)
rm(sales, fit)
rdz_load(archive, names = "fit")
exists("fit")
#> [1] TRUE
exists("sales")
#> [1] FALSE
```

The archive is an ordinary rdz file holding a named list, so
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md) and
[`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md)
work on it too.
