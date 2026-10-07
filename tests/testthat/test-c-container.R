# The C container (src/core) through its entry points. These run in every
# build, with or without the Rust reference implementation.

test_that("the record layouts match their zubin specifications", {
  expect_null(.Call(rdz:::rdz_test_records))
})

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

test_that("a NUL inside a string or in metadata is a format error, not R's", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.compress = 0)
  on.exit(options(old), add = TRUE)
  find <- function(bytes, s) {
    p <- charToRaw(s)
    at <- which(vapply(seq_len(length(bytes) - length(p) + 1L) - 1L,
                       function(i) identical(bytes[i + seq_along(p)], p), TRUE))
    at[[1L]] - 1L
  }
  # a string record "aQc" whose middle byte becomes NUL
  write_rdz("aQc", path)
  bytes <- readBin(path, "raw", file.size(path))
  at <- find(bytes, "aQc")
  bytes[at + 2L] <- as.raw(0L)
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  first <- dir + dir_header_len(bytes, dir) + 48 # one object, no attributes
  writeBin(reseal_block(bytes, le_u64(bytes, first + 16)), path)
  expect_error(read_rdz(path), "NUL", class = "rdz_format_error")
  # a metadata value "vQv" whose middle byte becomes NUL
  write_rdz(1L, path, metadata = c(k = "vQv"))
  bytes <- readBin(path, "raw", file.size(path))
  bytes[find(bytes, "vQv") + 2L] <- as.raw(0L)
  writeBin(reseal_block(bytes, -1), path) # the directory's checksum only
  expect_error(rdz_info(path), "NUL", class = "rdz_format_error")
})

test_that("a factor code past its levels is a format error", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  f <- factor(rep(c("a", "b", "c"), 100)) # frame of reference, base 1, codes 0..2 a byte each
  write_rdz(f, path, compress = 0)
  expect_identical(read_rdz(path), f)
  bytes <- readBin(path, "raw", file.size(path))
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  hl <- dir_header_len(bytes, dir)
  nobj <- sum(as.numeric(bytes[dir + 17:20]) * 256^(0:3))
  natt <- sum(as.numeric(bytes[dir + 21:24]) * 256^(0:3))
  first <- sum(as.numeric(bytes[dir + hl + 41:44]) * 256^(0:3)) # the root's first block
  entry <- dir + hl + 48 * nobj + 32 * natt + 64 * first
  payload <- le_u64(bytes, entry + 16)
  expect_identical(as.integer(bytes[payload + 1L]), 8L) # level 0: width 8
  bytes[payload + 8L + 1L] <- as.raw(3) # the first code 3: the value 4, past 3 levels
  writeBin(reseal_block(bytes, payload), path)
  expect_error(read_rdz(path), "outside its levels", class = "rdz_format_error")
})

test_that("a factor with codes outside its levels is written generically, and hashes so", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  bad <- structure(c(1L, 5L, NA, 2L), levels = c("a", "b"), class = "factor")
  big <- structure(c(rep(1:2, 200000L), 3L), levels = c("a", "b"), class = "factor") # past a block
  for (x in list(bad, big)) {
    write_rdz(x, path)
    expect_identical(rdz_info(path)$codec, "r_serial_v3")
    expect_identical(unclass(read_rdz(path)), unclass(x))
    expect_identical(rdz_info(path)$content_hash, rdz_hash(x))
    expect_error(write_rdz(x, path, mode = "native"), class = "rdz_unsupported_error")
  }
})
