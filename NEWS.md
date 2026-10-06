# rdz 0.1.0

* First release. The file format is frozen as of this version: every later
  rdz reads the files it writes (`?rdz`, "Compatibility"). Files written by
  development builds before 0.1.0 have no compatibility guarantee.
* `write_rdz()` and `read_rdz()` write and read R objects in
  a versioned, checksummed block container: logical, integer, double and
  character vectors, factors, lists and data frames natively, everything
  else through R serialization.
* Selective reads: `read_rdz(select =, rows =)` reads some columns or list
  elements, and some rows or vector elements, decoding only their blocks.
* Inspection without the data: `rdz_info()`, `rdz_schema()` and
  `rdz_attributes()`.
* Content hashes: every file records one, `rdz_hash()` computes it for an
  object, `write_rdz(skip_unchanged = TRUE)` leaves a file holding the
  value untouched, and `rdz_verify()` checks a file.
* `write_rdz(compress =)` (or `options(rdz.compress)`) is the Zstandard
  level of each block, from 0 (none: the fastest to write and read) to 19;
  the default, 1, is fast and already small.
* `write_rdz(hash = FALSE)` (or `options(rdz.hash = FALSE)`) skips the
  content hash, for about a tenth less write time.
* `rdz_serialize()` and `rdz_unserialize()` work on raw vectors;
  `rdz_save()` and `rdz_load()` keep several objects in one file.
