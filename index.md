# rdz

`rdz` writes and reads R objects in a versioned, seekable, checksummed
block container. Common objects are stored natively, column by column
and block by block, compressed with Zstandard on a pool of threads;
everything else goes through R’s own serializer, streamed through the
same blocks.

- **Native:** logical, integer, double and character vectors, factors,
  lists and data frames (data.table and tibble included), nested, with
  their attributes: names, classes, a Date’s or POSIXct’s metadata, a
  matrix’s dimensions. Each vector picks its own block records (runs,
  frame of reference, delta, byte planes, sparse and bitplane logicals,
  string dictionaries).
- **Generic:** anything else, such as environments, closures, calls,
  complex and raw vectors, and S4 objects, goes through R serialization
  as one whole root, so sharing and references are kept.
- **Selective:** `read_rdz(path, select = , rows = )` reads some columns
  of a data frame or elements of a list, and some rows of a data frame
  or elements of a vector, decoding only the blocks they need.
- **Inspectable:**
  [`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md)
  shows a native file’s tree (each part’s type, class, shape, attributes
  and size), and `rdz_attributes(path, object = c("sales", "date"))`
  reads any part’s attributes, without decoding its data.
- **Content-addressed:** every file records a 128-bit hash of the value
  (`rdz_info(path)$content_hash`, read without the data), and
  `rdz_hash(x)` gives the same hash for an object, whatever preset,
  threads or platform. `write_rdz(skip_unchanged = TRUE)` leaves a file
  holding the value as it is, and
  [`rdz_verify()`](https://pedrobtz.github.io/rdz/reference/rdz_verify.md)
  checks a file’s integrity without building it.
- **Compact graphs:** a large vector that appears twice in an object is
  stored once and read back shared, as pickle and Kryo do.
- **Archives:** `rdz_save(a, b, file = )` and
  `rdz_load(file, names = "b")` work as
  [`save()`](https://rdrr.io/r/base/save.html) and
  [`load()`](https://rdrr.io/r/base/load.html) do, and load only the
  objects named.
- **In memory:** `rdz_serialize(x)` gives the bytes as a raw vector (for
  Redis, a database, a socket) and
  [`rdz_unserialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)
  reads them back; every reader takes a raw vector where it takes a
  path.
- **Self-describing:**
  `write_rdz(x, path, metadata = c(source = "..."))` records key-value
  strings that
  [`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
  reads without the data.
- **Tables as written:** data.tables come back usable in place, nested
  or not, and tibbles as tibbles.
- **Checked:** every header, directory and block carries an XXH3-64
  checksum, and every length and offset is bounded before anything is
  allocated.
- **Portable:** files are byte-identical on every platform, little- and
  big-endian alike.

## Installation

rdz needs R 4.1 or newer and a C compiler. It links against
[zubin](https://github.com/pedrobtz/zubin) and
[zufast](https://github.com/pedrobtz/zufast), which are installed with
it.

``` r

install.packages("rdz")
```

or the development version:

``` r

# install.packages("pak")
pak::pak("pedrobtz/rdz")
```

[Using rdz](https://pedrobtz.github.io/rdz/articles/rdz.html) walks
through writing and reading, selective reads, inspection, content hashes
and archives.

## Usage

``` r

library(rdz)

path <- tempfile(fileext = ".rdz")
write_rdz(mtcars, path)
read_rdz(path)
read_rdz(path, select = c("mpg", "wt"))          # only these columns are read
rdz_schema(path)                                  # the tree, from the directory
rdz_attributes(path, names = "names")

options(rdz.preset = "compact", rdz.threads = 4)  # smaller files, more threads
write_rdz(mtcars, path, mode = "native")           # fail rather than fall back
```

## Compatibility

The file format is frozen as of rdz 0.1.0: every later rdz reads every
file that rdz 0.1.0 or later wrote. A later writer adds block encodings
or attributes under new identifiers, never changing the meaning of
existing ones, and a reader refuses, rather than misreads, what it does
not know. The tests read a corpus written by rdz 0.1.0, which is never
rewritten.
[`?rdz`](https://pedrobtz.github.io/rdz/reference/rdz-package.md) states
this in the package, and the [container
format](https://github.com/pedrobtz/rdz/blob/main/.agents/container-format.md)
gives the details. Files written by development builds before 0.1.0 have
no compatibility guarantee.
