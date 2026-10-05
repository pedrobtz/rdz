# The native integer and double codecs (plan-c Stage F).

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

test_that("integers round-trip at every block boundary", {
  per <- 262144L
  for (n in c(0L, 1L, per - 1L, per, per + 1L, 3L * per + 5L)) {
    x <- as.integer((seq_len(n) * 7919L) %% 1000003L)
    x[seq_len(n) %% 97L == 1L] <- NA_integer_
    expect_identical(native_roundtrip(x), x, label = n)
  }
})

test_that("each integer record is chosen where it pays and reads back", {
  n <- 300000L
  cases <- list(
    delta = list(seq_len(n) * 3L, 13L),
    runs = list(rep(c(5L, NA, -7L), each = 100000L), 14L),
    frame = list(rep_len(c(10L, 11L, NA, 13L), n), 12L),
    constant = list(rep(42L, n), 12L),
    all_na = list(rep(NA_integer_, n), 14L),
    # three values repeating: plain bytes, whose repeats zstd finds, not planes
    extremes = list(rep_len(c(.Machine$integer.max, -.Machine$integer.max, NA), n), 10L)
  )
  for (name in names(cases)) {
    x <- cases[[name]][[1L]]
    expect_identical(native_roundtrip(x, expect_encodings = cases[[name]][[2L]]), x,
                     label = name)
  }
  set.seed(5)
  # the full range: 32 bits, so no frame of reference beats raw
  noise <- as.integer(floor(stats::runif(n, -2^31 + 1, 2^31 - 1)))
  expect_identical(native_roundtrip(noise, rdz.preset = "speed", expect_encodings = 10L), noise)
  expect_identical(native_roundtrip(noise, expect_encodings = 11L), noise)
})

test_that("doubles round-trip bit for bit, NaN payloads and -0 included", {
  payload <- readBin(as.raw(c(1, 0, 0, 0, 0, 0, 0xf8, 0x7f)), "double", endian = "little")
  specials <- c(0, -0, NA, NaN, payload, Inf, -Inf, 2^-1074, .Machine$double.xmax, pi)
  per <- 131072L
  for (n in c(0L, 1L, per - 1L, per, per + 1L, 2L * per + 3L)) {
    x <- rep_len(specials, n) * rep_len(c(1, 1, 1, 1, 1, 1, 1, 1, 1, 0.5), n)
    at <- which(seq_len(n) %% 11L == 1L)
    x[at] <- rep_len(specials, length(at))
    got <- native_roundtrip(x)
    expect_identical(double_bits(got), double_bits(x), label = n)
  }
})

test_that("each double record reads back", {
  n <- 200000L
  set.seed(6)
  expect_identical(native_roundtrip(rep(NA_real_, n), expect_encodings = 22L), rep(NA_real_, n))
  x <- runif(n)
  expect_identical(native_roundtrip(x, expect_encodings = 21L), x)
  expect_identical(native_roundtrip(x, rdz.preset = "speed", expect_encodings = 20L), x)
})

test_that("integer and double vectors keep their names", {
  x <- stats::setNames(1:5 * 2L, letters[1:5])
  expect_identical(native_roundtrip(x), x)
  y <- stats::setNames(c(1.5, NA, -0), c("a", NA, "c"))
  expect_identical(double_bits(native_roundtrip(y)), double_bits(y))
  expect_identical(names(native_roundtrip(y)), names(y))
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(y, path)
  expect_identical(rdz_info(path)$root_type, "double")
  expect_identical(rdz_attributes(path, names = "names"), list(names = names(y)))
})

test_that("the bytes do not depend on the number of threads", {
  set.seed(7)
  x <- list(sample.int(1000L, 1e6, TRUE), cumsum(runif(5e5)))
  for (value in x) {
    paths <- vapply(c(1L, 8L), function(threads) {
      p <- tempfile(fileext = ".rdz")
      old <- options(rdz.threads = threads)
      on.exit(options(old))
      write_rdz(value, p)
      p
    }, character(1L))
    expect_identical(readBin(paths[[1L]], "raw", 1e8), readBin(paths[[2L]], "raw", 1e8))
    old <- options(rdz.threads = 8L)
    expect_identical(read_rdz(paths[[1L]]), value)
    options(old)
    unlink(paths)
  }
})

test_that("ALTREP vectors are left to the generic codec", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(1:10, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path), 1:10)
  expect_error(write_rdz(1:10, path, mode = "native"), "ALTREP integer",
               class = "rdz_unsupported_error")
  write_rdz(structure(1:3 + 0L, class = "myclass"), path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
})

test_that("a corrupt numeric record is a classed error", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.preset = "speed")
  on.exit(options(old), add = TRUE)
  write_rdz(seq_len(1000L) * 2L, path) # one delta block
  expect_identical(rdz_block_encodings(path)$encoding[[1L]], 13)
  bytes <- readBin(path, "raw", file.size(path))
  # the delta width, the first byte of the record, set past 32; the block
  # and directory checksums are then rewritten so only the record is wrong
  skip_if_not_installed("zufast")
  bytes[[32L + 48L + 1L]] <- as.raw(40L)
  le <- function(x, n) writeBin(as.integer(x), raw(), size = n, endian = "little")
  hash_le <- function(r) {
    h <- zufast::fast_hash(r)
    rev(as.raw(strtoi(substring(h, seq(1, 15, 2), seq(2, 16, 2)), 16L)))
  }
  stored <- bytes[(32L + 48L + 1L):(32L + 48L + 16L + 250L)]
  n <- length(bytes)
  dir_off <- sum(as.numeric(bytes[(n - 31L):(n - 24L)]) * 256^(0:7))
  stored_len <- sum(as.numeric(bytes[(32L + 33L):(32L + 36L)]) * 256^(0:3))
  stored <- bytes[(32L + 48L + 1L):(32L + 48L + stored_len)]
  bytes[(32L + 41L):(32L + 48L)] <- hash_le(stored)
  entry <- dir_off + 40L + 48L
  bytes[(entry + 57L):(entry + 64L)] <- hash_le(stored)
  dir_len <- sum(as.numeric(bytes[(n - 23L):(n - 16L)]) * 256^(0:7))
  bytes[(n - 15L):(n - 8L)] <- hash_le(bytes[(dir_off + 1L):(dir_off + dir_len)])
  writeBin(bytes, path)
  expect_error(read_rdz(path), "invalid integer block", class = "rdz_format_error")
})
