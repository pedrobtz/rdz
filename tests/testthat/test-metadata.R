# User metadata (container-format.md): named UTF-8 strings recorded with the
# value, read by rdz_info() without the value, outside the content hash.

test_that("metadata round-trips, natively and generically, as UTF-8", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  # names from strings, not symbols: the parser turns a symbol native (C locale)
  md <- stats::setNames(c("datasets::mtcars", "1.2 \u2713", ""),
                        c("source", "vers\u00e3o", "empty"))
  for (mode in c("native", "r")) {
    write_rdz(mtcars, path, mode = mode, metadata = md)
    got <- rdz_info(path)$metadata
    expect_identical(unname(got), unname(enc2utf8(md)), label = mode)
    expect_identical(names(got), enc2utf8(names(md)), label = mode)
    expect_true(all(Encoding(got[nzchar(got)]) %in% c("UTF-8", "unknown")))
    expect_identical(read_rdz(path), mtcars)
  }
  write_rdz(1:3, path, metadata = list(a = "x", b = "y"))
  expect_identical(rdz_info(path)$metadata, c(a = "x", b = "y"))
  write_rdz(1:3, path)
  expect_length(rdz_info(path)$metadata, 0L)
})

test_that("metadata is not part of the content hash, but skip_unchanged sees it", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(1:10, path, metadata = c(v = "1"))
  h <- rdz_info(path)$content_hash
  write_rdz(1:10, path, metadata = c(v = "2"))
  expect_identical(rdz_info(path)$content_hash, h)
  Sys.setFileTime(path, as.POSIXct("2020-01-01", tz = "UTC"))
  mtime <- file.mtime(path)
  write_rdz(1:10, path, metadata = c(v = "2"), skip_unchanged = TRUE)
  expect_identical(file.mtime(path), mtime)
  write_rdz(1:10, path, metadata = c(v = "3"), skip_unchanged = TRUE)
  expect_identical(rdz_info(path)$metadata, c(v = "3"))
})

test_that("metadata is read without the value", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.preset = "speed")
  on.exit(options(old), add = TRUE)
  write_rdz(seq_len(50000L), path, metadata = c(k = "v"))
  bytes <- readBin(path, "raw", file.size(path))
  bytes[32L + 48L + 1L] <- xor(bytes[32L + 48L + 1L], as.raw(0xff)) # the data block
  writeBin(bytes, path)
  expect_error(read_rdz(path), class = "rdz_format_error")
  expect_identical(rdz_info(path)$metadata, c(k = "v"))
})

test_that("invalid metadata is refused when written and when read", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  expect_error(write_rdz(1, path, metadata = c("x")), "named")
  expect_error(write_rdz(1, path, metadata = c(a = NA_character_)), "without NA")
  expect_error(write_rdz(1, path, metadata = list(a = 1)), "single")
  expect_error(write_rdz(1, path, metadata = c(a = "1", a = "2")), "distinct")
  expect_error(write_rdz(1, path, metadata = stats::setNames("v", "")), "non-empty")
  expect_error(write_rdz(1, path, metadata = c(a = strrep("x", 70000))), "64 KiB")
  # a file with a repeated key or invalid UTF-8 (directory resealed)
  write_rdz(1L, path, metadata = c(ab = "x", ac = "y"))
  bytes <- readBin(path, "raw", file.size(path))
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  dir_len <- le_u64(bytes, n - 24L)
  reseal <- function(b) {
    b[n - 16 + 1:8] <- xxh3_le(b[dir + seq_len(dir_len)])
    b
  }
  at <- grepRaw(charToRaw("ac"), bytes, fixed = TRUE)
  repeated <- bytes
  repeated[at + 1L] <- charToRaw("b")
  writeBin(reseal(repeated), path)
  expect_error(rdz_info(path), "repeated metadata key", class = "rdz_format_error")
  invalid <- bytes
  invalid[at] <- as.raw(0xff)
  writeBin(reseal(invalid), path)
  expect_error(rdz_info(path), "not UTF-8", class = "rdz_format_error")
})
