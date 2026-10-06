# Content hashes (container-format.md, "Content hash"): rdz_hash(x) is what
# write_rdz() records, whatever the preset, threads, block sizes, encodings or
# ALTREP; any change to the value changes it.

hash_cases <- function() {
  list(
    logical = c(TRUE, NA, FALSE),
    integer = c(1L, NA, -5L),
    double = c(1.5, NA, -2.25),
    character = c("a", NA, "été"),
    factor = factor(c("lo", "hi", NA)),
    frame = data.frame(d = as.Date("2026-10-06") + 0:2, x = c(0.5, 1, 1.5)),
    nested = list(a = 1:3, b = list(c = "x", d = NULL), m = matrix(1:4, 2)),
    generic = list(f = quote(g(x)), e = 1i),
    empty = list()
  )
}

test_that("the stored hash is rdz_hash() of the value, whatever the settings", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  for (name in names(hash_cases())) {
    x <- hash_cases()[[name]]
    want <- rdz_hash(x)
    expect_match(want, "^[0-9a-f]{32}$")
    for (preset in c("speed", "balanced", "compact")) {
      for (threads in test_threads(c(1L, 4L))) {
        old <- options(rdz.preset = preset, rdz.threads = threads)
        write_rdz(x, path)
        options(old)
        expect_identical(rdz_info(path)$content_hash, want, label = paste(name, preset, threads))
      }
    }
  }
  # small blocks, another dictionary policy: other bytes, the same hash
  x <- rep(c("alpha", "beta", NA), 5000L)
  old <- options(rdz.block_size = 4096L)
  write_rdz(x, path)
  options(old)
  expect_identical(rdz_info(path)$content_hash, rdz_hash(x))
})

test_that("representations of one value hash equally", {
  expect_identical(rdz_hash(1:100000), rdz_hash(as.integer(as.numeric(1:100000))))
  expect_identical(rdz_hash(1:10, mode = "r"), rdz_hash(c(1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 10L), mode = "r"))
  expect_identical(rdz_hash(as.character(1:10)), rdz_hash(c("1", "2", "3", "4", "5", "6", "7", "8", "9", "10")))
})

test_that("any change to the value changes the hash", {
  base <- list(a = c(1, 2, 3), b = factor(c("x", "y")), c = "s")
  neg_zero <- readBin(as.raw(c(0, 0, 0, 0, 0, 0, 0, 0x80)), "double", endian = "little")
  na_payload <- readBin(as.raw(c(1, 0, 0, 0, 0, 0, 0xf8, 0x7f)), "double", endian = "little")
  variants <- list(
    element = within(base, a[2] <- 2.5),
    type = within(base, a <- c(1L, 2L, 3L)),
    names = stats::setNames(base, c("a", "B", "c")),
    levels = within(base, b <- factor(c("x", "y"), levels = c("y", "x"))),
    attribute = within(base, attr(a, "note") <- "n"),
    order = base[c(2, 1, 3)],
    neg_zero = within(base, a[1] <- neg_zero),
    nan_payload = within(base, a[1] <- na_payload),
    string_encoding = within(base, c <- { s <- "caf\xe9"; Encoding(s) <- "latin1"; s })
  )
  h <- rdz_hash(base)
  for (name in names(variants)) {
    expect_false(identical(rdz_hash(variants[[name]]), h), label = name)
  }
  # NA and NaN hash apart, as their bits differ
  expect_false(identical(rdz_hash(NA_real_), rdz_hash(NaN)))
  # the hash depends on how rdz stores the value
  expect_false(identical(rdz_hash(base), rdz_hash(base, mode = "r")))
  expect_error(rdz_hash(new.env(), mode = "native"), class = "rdz_unsupported_error")
})

test_that("skip_unchanged leaves a file holding the value untouched", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- data.frame(a = 1:3, b = c("x", "y", "z"))
  write_rdz(x, path)
  before <- readBin(path, "raw", file.size(path))
  Sys.setFileTime(path, as.POSIXct("2020-01-01", tz = "UTC"))
  mtime <- file.mtime(path)
  write_rdz(x, path, skip_unchanged = TRUE)
  expect_identical(file.mtime(path), mtime)
  expect_identical(readBin(path, "raw", file.size(path)), before)
  # another value, or another mode, writes
  write_rdz(transform(x, a = a + 1L), path, skip_unchanged = TRUE)
  expect_false(identical(file.mtime(path), mtime))
  expect_identical(read_rdz(path)$a, 2:4)
  Sys.setFileTime(path, as.POSIXct("2020-01-01", tz = "UTC"))
  write_rdz(read_rdz(path), path, mode = "r", skip_unchanged = TRUE)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  # a file that records no hash is rewritten
  old <- file.path(testthat::test_path("fixtures", "rust"), "lgl_len1.rdz")
  file.copy(old, path, overwrite = TRUE)
  expect_true(is.na(rdz_info(path)$content_hash))
  write_rdz(TRUE, path, skip_unchanged = TRUE)
  expect_false(is.na(rdz_info(path)$content_hash))
  expect_error(write_rdz(x, path, skip_unchanged = NA), "TRUE or FALSE")
})

test_that("rdz_verify() checks every block and, on request, the content hash", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.preset = "speed")
  on.exit(options(old), add = TRUE)
  x <- list(a = seq_len(40000L), b = letters)
  write_rdz(x, path)
  expect_identical(rdz_verify(path), path)
  expect_identical(rdz_verify(path, content = TRUE), path)
  bytes <- readBin(path, "raw", file.size(path))
  # a stored hash that is not the value's (directory resealed: only the
  # content check can tell)
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  tampered <- bytes
  tampered[dir + 41L] <- xor(tampered[dir + 41L], as.raw(1L))
  dir_len <- le_u64(tampered, n - 24L)
  tampered[n - 16 + 1:8] <- xxh3_le(tampered[dir + seq_len(dir_len)])
  writeBin(tampered, path)
  expect_identical(rdz_verify(path), path)
  expect_error(rdz_verify(path, content = TRUE), "content hash", class = "rdz_format_error")
  # a corrupt block, found without decompressing anything
  corrupt <- bytes
  corrupt[32L + 48L + 1L] <- xor(corrupt[32L + 48L + 1L], as.raw(0xff))
  writeBin(corrupt, path)
  expect_error(rdz_verify(path), "checksum mismatch", class = "rdz_format_error")
  # a file without a hash: blocks verified, content cannot be
  file.copy(file.path(testthat::test_path("fixtures", "rust"), "lgl_len1.rdz"), path,
            overwrite = TRUE)
  expect_identical(rdz_verify(path), path)
  expect_error(rdz_verify(path, content = TRUE), "no content hash")
})

test_that("hashing many character vectors needs no memory per vector", {
  skip_on_cran()
  x <- lapply(1:1000, function(i) sprintf("s%04d_%d", 1:200, i))
  invisible(gc(reset = TRUE))
  base <- gc()["Vcells", "max used"]
  invisible(gc(reset = TRUE))
  rdz_hash(x)
  peak <- gc()["Vcells", "max used"]
  expect_lt((peak - base) * 8, 64 * 2^20) # one string cache, not one per vector
})
