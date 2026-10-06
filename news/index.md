# Changelog

## rdz 0.1.0

- First release. The file format is frozen as of this version: every
  later rdz reads the files it writes
  ([`?rdz`](https://pedrobtz.github.io/rdz/reference/rdz-package.md),
  “Compatibility”). Files written by development builds before 0.1.0
  have no compatibility guarantee.
- [`write_rdz()`](https://pedrobtz.github.io/rdz/reference/write_rdz.md)
  and
  [`read_rdz()`](https://pedrobtz.github.io/rdz/reference/read_rdz.md)
  write and read R objects in a versioned, checksummed block container:
  logical, integer, double and character vectors, factors, lists and
  data frames natively, everything else through R serialization.
- Selective reads: `read_rdz(select =, rows =)` reads some columns or
  list elements, and some rows or vector elements, decoding only their
  blocks.
- Inspection without the data:
  [`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md),
  [`rdz_schema()`](https://pedrobtz.github.io/rdz/reference/rdz_schema.md)
  and
  [`rdz_attributes()`](https://pedrobtz.github.io/rdz/reference/rdz_attributes.md).
- Content hashes: every file records one,
  [`rdz_hash()`](https://pedrobtz.github.io/rdz/reference/rdz_hash.md)
  computes it for an object, `write_rdz(skip_unchanged = TRUE)` leaves a
  file holding the value untouched, and
  [`rdz_verify()`](https://pedrobtz.github.io/rdz/reference/rdz_verify.md)
  checks a file.
- `write_rdz(hash = FALSE)` (or `options(rdz.hash = FALSE)`) skips the
  content hash, for about a tenth less write time.
- [`rdz_serialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)
  and
  [`rdz_unserialize()`](https://pedrobtz.github.io/rdz/reference/rdz_serialize.md)
  work on raw vectors;
  [`rdz_save()`](https://pedrobtz.github.io/rdz/reference/rdz_save.md)
  and
  [`rdz_load()`](https://pedrobtz.github.io/rdz/reference/rdz_save.md)
  keep several objects in one file.
