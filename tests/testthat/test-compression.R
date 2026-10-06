# Per-block zstd compression and the threaded pipeline (plan-c Stage D).

test_that("rdz carries the zstd release its vendor manifest pins", {
  expect_identical(rdz:::rdz_zstd_version(), "1.5.7")
})

test_that("the default level compresses blocks and reads them back", {
  x <- rep_len(c(1.5, 2.5, NA), 1e6)
  path <- write_with(x)
  on.exit(unlink(path), add = TRUE)
  blocks <- rdz_block_encodings(path)
  expect_true(all(blocks$compression == 1))
  expect_lt(file.size(path), rdz_info(path)$payload_bytes / 10)
  expect_identical(read_rdz(path), x)
})

test_that("an incompressible block is stored raw beside compressed ones", {
  set.seed(1)
  noise <- as.raw(sample.int(256L, 1.5e6, replace = TRUE) - 1L)
  x <- list(noise, rep(0L, 5e5))
  path <- write_with(x)
  on.exit(unlink(path), add = TRUE)
  compression <- rdz_block_encodings(path)$compression
  expect_true(any(compression == 0))
  expect_true(any(compression == 1))
  expect_identical(read_rdz(path), x)
})

test_that("each level reads back, and level 0 stores every block raw", {
  x <- rep_len(letters, 3e5)
  sizes <- c()
  for (level in c("0", "1", "6", "19")) {
    path <- write_with(x, rdz.compress = as.integer(level))
    sizes[[level]] <- file.size(path)
    if (level == "0") expect_true(all(rdz_block_encodings(path)$compression == 0))
    expect_identical(read_rdz(path), x, label = level)
    unlink(path)
  }
  expect_lt(sizes[["1"]], sizes[["0"]])
  expect_lte(sizes[["6"]], sizes[["1"]])
  expect_lte(sizes[["19"]], sizes[["6"]])
})

test_that("the bytes do not depend on the number of threads", {
  set.seed(2)
  x <- list(runif(2e5), rep_len(letters, 1e5), as.raw(sample.int(256L, 2e5, TRUE) - 1L))
  paths <- vapply(test_threads(c(1L, 2L, 8L)), function(threads) {
    write_with(x, rdz.threads = threads, rdz.block_size = 4096L)
  }, character(1L))
  on.exit(unlink(paths), add = TRUE)
  expect_gt(rdz_info(paths[[1L]])$block_count, 500L)
  expect_identical(file_bytes(paths[[2L]]), file_bytes(paths[[1L]]))
  expect_identical(file_bytes(paths[[3L]]), file_bytes(paths[[1L]]))
  for (threads in test_threads(c(1L, 3L, 8L))) {
    expect_identical(with_rdz_options(read_rdz(paths[[1L]]), rdz.threads = threads), x)
  }
})

test_that("an error mid-write with eight threads leaves no file behind", {
  dir <- tempfile("rdz-threads-")
  dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE), add = TRUE)
  path <- file.path(dir, "x.rdz")
  x <- runif(1e5)
  settings <- with_rdz_options(rdz:::rdz_settings(), rdz.threads = test_threads(8L), rdz.block_size = 4096L)
  expect_error(
    rdz:::rdz_check(.Call(rdz:::rdz_test_write_generic_unwind, x, path, 40L, settings)),
    "failing after 40 blocks"
  )
  expect_length(list.files(dir, all.files = TRUE, no.. = TRUE), 0L)
})

test_that("a corrupt compressed block is a classed error with any thread count", {
  x <- rep_len(c(1.5, 2.5, NA), 1e6)
  path <- write_with(x, rdz.block_size = 65536L)
  on.exit(unlink(path), add = TRUE)
  bytes <- file_bytes(path)
  # A byte of the first block's stored bytes, past the file and block headers.
  bytes[[32L + 48L + 10L]] <- as.raw(bitwXor(as.integer(bytes[[32L + 48L + 10L]]), 0xffL))
  writeBin(bytes, path)
  for (threads in test_threads(c(1L, 4L))) {
    expect_error(
      with_rdz_options(read_rdz(path), rdz.threads = threads),
      "checksum mismatch in block 0",
      class = "rdz_format_error"
    )
  }
})

test_that("the options are validated", {
  expect_error(with_rdz_options(rdz:::rdz_settings(), rdz.compress = "fast"), "rdz.compress")
  expect_error(with_rdz_options(rdz:::rdz_settings(), rdz.compress = 20), "rdz.compress")
  expect_error(with_rdz_options(rdz:::rdz_settings(), rdz.compress = 1.5), "rdz.compress")
  expect_error(write_rdz(1, tempfile(), compress = -1), class = "rdz_argument_error")
  expect_error(with_rdz_options(rdz:::rdz_settings(), rdz.threads = 0), "rdz.threads")
  expect_error(with_rdz_options(rdz:::rdz_settings(), rdz.threads = 1.5), "rdz.threads")
})

test_that("compress = is the level, as options(rdz.compress) is", {
  x <- rep_len(letters, 3e5)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, compress = 0)
  expect_true(all(rdz_block_encodings(path)$compression == 0))
  raw_size <- file.size(path)
  write_rdz(x, path, compress = 3)
  expect_lt(file.size(path), raw_size)
  expect_identical(read_rdz(path), x)
  old <- options(rdz.compress = 0)
  on.exit(options(old), add = TRUE)
  expect_identical(file.size(write_rdz(x, path)), raw_size)
  expect_identical(length(rdz_serialize(x, compress = 0)), as.integer(raw_size))
})

test_that("options(rdz.preset) still works, deprecated, once a session", {
  x <- rep_len(letters, 3e5)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  state <- rdz:::rdz_state
  state$preset_warned <- NULL
  old <- options(rdz.preset = "speed", rdz.compress = NULL)
  on.exit(options(old), add = TRUE)
  expect_warning(write_rdz(x, path), class = "rdz_deprecated_warning")
  expect_true(all(rdz_block_encodings(path)$compression == 0)) # level 0
  expect_no_warning(write_rdz(x, path)) # once a session
  options(rdz.preset = "bogus")
  expect_error(write_rdz(x, path), "rdz.compress", class = "rdz_argument_error")
  # rdz.compress, and compress =, win
  options(rdz.preset = "speed", rdz.compress = 1L)
  write_rdz(x, path)
  expect_false(all(rdz_block_encodings(path)$compression == 0))
  options(rdz.compress = NULL)
  write_rdz(x, path, compress = 1)
  expect_false(all(rdz_block_encodings(path)$compression == 0))
  state$preset_warned <- NULL
})
