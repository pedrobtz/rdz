# Read an R Object from an rdz File

`read_rdz()` validates the header, trailer, directory, block headers,
bounds, and block checksums before deserializing a generic payload.

## Usage

``` r
read_rdz(path, select = NULL, rows = NULL)
```

## Arguments

- path:

  A single, non-missing path to read.

- select:

  `NULL` (everything), or the columns of a data frame or the elements of
  a list to read: distinct names, or distinct positive positions.

- rows:

  `NULL` (all), or the rows of a data frame or elements of a vector to
  read: positive positions, in any order. From a natively written file
  only the blocks covering `range(rows)` are read; each column is then
  taken with `[`, so a Date or factor column keeps its class. Stored row
  names are taken too; automatic ones stay automatic (`1:length(rows)`).

## Value

The R object stored in `path`, or its selected part.

## Details

`select` reads some columns of a data frame or some elements of a list,
by name or by position, in the order given. From a natively written file
only the selected parts are read and decoded; a file written through R
serialization is read whole and then subset. A data frame keeps its row
names and class, and a list its names; the root's other attributes,
which may describe the parts left out (a data.table's key), are not
kept, and
[`rdz_attributes()`](https://pedrobtz.github.io/rdz/reference/rdz_attributes.md)
still reads them.

A data.table's `.internal.selfref`, a pointer to the table itself, is
never stored: every data.table comes back as from
[`readRDS()`](https://rdrr.io/r/base/readRDS.html), marked as loaded
from disk, and data.table rebuilds it by itself at the first change, at
the root or nested in lists. rdz never calls or loads data.table.
Tibbles need nothing rebuilt.

## Examples

``` r
path <- tempfile(fileext = ".rdz")
write_rdz(c(TRUE, FALSE, NA), path)
read_rdz(path)
#> [1]  TRUE FALSE    NA
write_rdz(mtcars, path)
read_rdz(path, select = c("mpg", "wt"))
#>                      mpg    wt
#> Mazda RX4           21.0 2.620
#> Mazda RX4 Wag       21.0 2.875
#> Datsun 710          22.8 2.320
#> Hornet 4 Drive      21.4 3.215
#> Hornet Sportabout   18.7 3.440
#> Valiant             18.1 3.460
#> Duster 360          14.3 3.570
#> Merc 240D           24.4 3.190
#> Merc 230            22.8 3.150
#> Merc 280            19.2 3.440
#> Merc 280C           17.8 3.440
#> Merc 450SE          16.4 4.070
#> Merc 450SL          17.3 3.730
#> Merc 450SLC         15.2 3.780
#> Cadillac Fleetwood  10.4 5.250
#> Lincoln Continental 10.4 5.424
#> Chrysler Imperial   14.7 5.345
#> Fiat 128            32.4 2.200
#> Honda Civic         30.4 1.615
#> Toyota Corolla      33.9 1.835
#> Toyota Corona       21.5 2.465
#> Dodge Challenger    15.5 3.520
#> AMC Javelin         15.2 3.435
#> Camaro Z28          13.3 3.840
#> Pontiac Firebird    19.2 3.845
#> Fiat X1-9           27.3 1.935
#> Porsche 914-2       26.0 2.140
#> Lotus Europa        30.4 1.513
#> Ford Pantera L      15.8 3.170
#> Ferrari Dino        19.7 2.770
#> Maserati Bora       15.0 3.570
#> Volvo 142E          21.4 2.780
unlink(path)
```
