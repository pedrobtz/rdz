# Helpers of test-numeric.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

native_roundtrip <- function(x, ..., expect_encodings = NULL) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(...)
  on.exit(options(old), add = TRUE)
  write_rdz(x, path, mode = "native")
  expect_identical(rdz_info(path)$codec, "native_v1")
  if (!is.null(expect_encodings)) {
    expect_true(all(expect_encodings %in% rdz_block_encodings(path)$encoding),
                label = paste(deparse(expect_encodings), collapse = ""))
  }
  read_rdz(path)
}
double_bits <- function(x) writeBin(x, raw(), endian = "little")
