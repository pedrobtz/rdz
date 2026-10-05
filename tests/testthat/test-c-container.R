# The C container (src/core) through its entry points. These run in every
# build, with or without the Rust reference implementation.

test_that("the record layouts match their zubin specifications", {
  expect_null(.Call(rdz:::rdz_test_records))
})

fixture_bytes <- function(name) {
  path <- file.path(rust_fixture_dir(), paste0(name, ".rdz"))
  readBin(path, "raw", file.size(path))
}

corrupt_copy <- function(bytes, at, value = NULL) {
  if (is.null(value)) {
    value <- as.raw(bitwXor(as.integer(bytes[[at]]), 0xffL))
  }
  bytes[at] <- value
  path <- tempfile(fileext = ".rdz")
  writeBin(bytes, path)
  path
}

test_that("a corrupt header, directory or trailer is a classed error", {
  bytes <- fixture_bytes("gen_integer_auto")
  n <- length(bytes)

  path <- corrupt_copy(bytes, 25L)
  on.exit(unlink(path), add = TRUE)
  expect_error(rdz_info(path), "header checksum mismatch", class = "rdz_format_error")

  path <- corrupt_copy(bytes, 5:6, writeBin(4L, raw(), size = 2L, endian = "little"))
  expect_error(rdz_info(path), "unsupported rdz format version 4", class = "rdz_version_error")

  path <- corrupt_copy(bytes, n - 40L)
  expect_error(rdz_info(path), "directory checksum mismatch", class = "rdz_format_error")

  path <- corrupt_copy(bytes, n)
  expect_error(rdz_info(path), "closing trailer magic mismatch", class = "rdz_error")

  path <- tempfile(fileext = ".rdz")
  writeBin(bytes[seq_len(100L)], path)
  expect_error(rdz_info(path), "file is truncated", class = "rdz_format_error")
})

test_that("a corrupt block is found by a read, not by rdz_info()", {
  bytes <- fixture_bytes("gen_integer_auto")
  # The first payload byte follows the 32-byte file and 48-byte block headers.
  path <- corrupt_copy(bytes, 81L)
  on.exit(unlink(path), add = TRUE)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_error(
    rdz:::rdz_check(.Call(rdz:::rdz_test_read_generic, path)),
    "checksum mismatch in block 0",
    class = "rdz_format_error"
  )
})

test_that("ordinary RDS files are not rdz containers", {
  path <- tempfile(fileext = ".rds")
  on.exit(unlink(path), add = TRUE)
  saveRDS(list(value = pi), path)
  expect_error(rdz_info(path), class = "rdz_error")
})

test_that("the C writer replaces a file and keeps its permissions", {
  skip_on_os("windows")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  writeLines("old", path)
  Sys.chmod(path, "0640")
  payload <- serialize(1:3, NULL, xdr = TRUE, version = 3L)
  rdz:::rdz_check(.Call(rdz:::rdz_test_write_generic, payload, raw(), path))
  expect_identical(format(file.mode(path)), "640")
  expect_identical(rdz:::rdz_check(.Call(rdz:::rdz_test_read_generic, path)), payload)
  expect_length(list.files(dirname(path), pattern = "-rdz-.*\\.tmp$", all.files = TRUE), 0L)
})

test_that("the C writer refuses a directory and an oversized synopsis", {
  dir <- tempfile("rdz-dir-")
  dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE), add = TRUE)
  expect_error(
    rdz:::rdz_check(.Call(rdz:::rdz_test_write_generic, as.raw(1), raw(), dir)),
    "destination is a directory",
    class = "rdz_format_error"
  )
  expect_error(
    rdz:::rdz_check(.Call(
      rdz:::rdz_test_write_generic, as.raw(1), raw(65537), file.path(dir, "x.rdz")
    )),
    "generic synopsis exceeds its format limit",
    class = "rdz_limit_error"
  )
  expect_length(list.files(dir, all.files = TRUE, no.. = TRUE), 0L)
})

test_that("files record the writer, which matches the package version", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(1:3, path)
  v <- unclass(utils::packageVersion("rdz"))[[1L]]
  want <- paste0("rdz ", paste(v[1:3], collapse = "."),
                 if (length(v) > 3L) " (development)" else "")
  expect_identical(rdz_info(path)$writer, want)
  # bytes 20 to 23 of the header
  bytes <- readBin(path, "raw", 24L)[21:24]
  expect_identical(bytes[[1L]] & as.raw(0x7f), as.raw(1L))
})
