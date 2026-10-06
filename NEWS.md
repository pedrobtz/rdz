# rdz 0.1.0

* First release. `write_rdz()` and `read_rdz()` write and read R objects in
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
* `rdz_serialize()` and `rdz_unserialize()` work on raw vectors;
  `rdz_save()` and `rdz_load()` keep several objects in one file.
