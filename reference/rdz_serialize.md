# Serialize an R Object to a Raw Vector

`rdz_serialize()` gives the bytes
[`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
would write, as a raw vector, for a database column, a key-value store
such as Redis, or a socket; `rdz_unserialize()` reads them back.
Everything else takes a raw vector where it takes a path:
[`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md)
(`select` included),
[`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md),
[`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md),
[`rdz_attributes()`](https://pedrobtz.github.io/rdz/reference/rdz_attributes.md)
and
[`rdz_verify()`](https://pedrobtz.github.io/rdz/reference/rdz_verify.md).

## Usage

``` r
rdz_serialize(x, mode = c("auto", "native", "r"), metadata = NULL)

rdz_unserialize(bytes, select = NULL)
```

## Arguments

- x:

  An R object to serialize.

- mode:

  Codec selection. `"auto"` uses a native codec only when the complete
  value is supported and otherwise uses whole-root R serialization;
  `"native"` rejects unsupported values; `"r"` forces R serialization.

- metadata:

  `NULL`, or user metadata to record with `x`: a named character vector
  (or a named list of single strings), up to 1,024 distinct non-empty
  names and 64 KiB in all, such as a source, a code version or a cache
  key's inputs.
  [`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
  reads it back, as `metadata`, without reading `x`; it is not part of
  the content hash.

- bytes:

  A raw vector holding an rdz file.

- select:

  As for
  [`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md).

## Value

`rdz_serialize()`: a raw vector. `rdz_unserialize()`: the object.

## Examples

``` r
bytes <- rdz_serialize(mtcars)
identical(rdz_unserialize(bytes), mtcars)
#> [1] TRUE
rdz_unserialize(bytes, select = "mpg")
#>                      mpg
#> Mazda RX4           21.0
#> Mazda RX4 Wag       21.0
#> Datsun 710          22.8
#> Hornet 4 Drive      21.4
#> Hornet Sportabout   18.7
#> Valiant             18.1
#> Duster 360          14.3
#> Merc 240D           24.4
#> Merc 230            22.8
#> Merc 280            19.2
#> Merc 280C           17.8
#> Merc 450SE          16.4
#> Merc 450SL          17.3
#> Merc 450SLC         15.2
#> Cadillac Fleetwood  10.4
#> Lincoln Continental 10.4
#> Chrysler Imperial   14.7
#> Fiat 128            32.4
#> Honda Civic         30.4
#> Toyota Corolla      33.9
#> Toyota Corona       21.5
#> Dodge Challenger    15.5
#> AMC Javelin         15.2
#> Camaro Z28          13.3
#> Pontiac Firebird    19.2
#> Fiat X1-9           27.3
#> Porsche 914-2       26.0
#> Lotus Europa        30.4
#> Ford Pantera L      15.8
#> Ferrari Dino        19.7
#> Maserati Bora       15.0
#> Volvo 142E          21.4
```
