test_that("write_rdz writes a file and returns its path invisibly", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  result <- withVisible(write_rdz(list(value = 1L), path))

  expect_false(result$visible)
  expect_identical(result$value, path)
  expect_true(file.exists(path))
  expect_identical(read_rdz(path), list(value = 1L))
})

test_that("write_rdz safely replaces an existing file", {
  path <- tempfile(pattern = "rdz path with spaces ", fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz("first", path)
  write_rdz(list(replacement = TRUE), path)

  expect_identical(read_rdz(path), list(replacement = TRUE))
})

test_that("file access failures are reported", {
  missing <- tempfile(fileext = ".rdz")
  invalid <- file.path(tempfile("missing-directory-"), "object.rdz")

  expect_error(suppressWarnings(read_rdz(missing)))
  expect_error(suppressWarnings(write_rdz(1L, invalid)))
  expect_false(file.exists(invalid))
})

test_that("truncated and invalid files are rejected", {
  valid <- tempfile(fileext = ".rdz")
  truncated <- tempfile(fileext = ".rdz")
  invalid <- tempfile(fileext = ".rdz")
  on.exit(unlink(c(valid, truncated, invalid)), add = TRUE)

  write_rdz(list(value = rep(pi, 100L)), valid)
  bytes <- readBin(valid, what = "raw", n = file.info(valid)$size)
  writeBin(bytes[seq_len(length(bytes) %/% 2L)], truncated)
  writeBin(as.raw(0:15), invalid)

  expect_error(read_rdz(truncated))
  expect_error(read_rdz(invalid))
})

test_that("native example functions preserve values and missingness", {
  expect_identical(int_times_int(c(1L, NA_integer_, -2L), 3L), c(3L, NA_integer_, -6L))
  expect_identical(to_upper(c("hello", NA_character_)), c("HELLO", NA_character_))
})
