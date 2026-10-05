test_that("logical vectors use the native adaptive codec", {
  cases <- list(
    empty = logical(),
    scalar = TRUE,
    mixed = c(FALSE, TRUE, NA, TRUE, FALSE, NA)
  )

  for (name in names(cases)) {
    path <- tempfile(fileext = ".rdz")
    on.exit(unlink(path), add = TRUE)
    write_rdz(cases[[name]], path)

    expect_identical(rdz_info(path)$codec, "native_v1", info = name)
    expect_identical(read_rdz(path), cases[[name]], info = name)
  }
})

test_that("logical block boundaries round-trip", {
  elements_per_block <- 64L * 1024L
  x <- rep(c(FALSE, TRUE, NA), length.out = elements_per_block + 1L)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(x, path)
  info <- rdz_info(path)

  expect_identical(info$codec, "native_v1")
  expect_identical(info$block_count, 2L)
  expect_identical(read_rdz(path), x)
})

test_that("adaptive logical distributions round-trip", {
  set.seed(2026)
  size <- 64L * 1024L
  cases <- list(
    constant = rep(NA, size),
    dense = sample(c(FALSE, TRUE, NA), size, replace = TRUE),
    sparse = sample(
      c(FALSE, TRUE, NA),
      size,
      replace = TRUE,
      prob = c(0.998, 0.001, 0.001)
    ),
    runs = rep(c(FALSE, TRUE, NA), each = size / 4L, length.out = size),
    periodic = rep(c(FALSE, TRUE, NA), length.out = size)
  )

  for (name in names(cases)) {
    path <- tempfile(fileext = ".rdz")
    on.exit(unlink(path), add = TRUE)
    write_rdz(cases[[name]], path)
    expect_identical(read_rdz(path), cases[[name]], info = name)
  }
})

test_that("native logical names preserve values and R encoding tags", {
  latin1 <- iconv("Ärende", from = "UTF-8", to = "latin1")
  utf8 <- enc2utf8("日本語")
  bytes <- rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9)))
  Encoding(bytes) <- "bytes"
  x <- c(FALSE, TRUE, NA, FALSE, TRUE)
  names(x) <- c("plain", latin1, utf8, bytes, NA_character_)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(x, path)
  restored <- read_rdz(path)

  expect_identical(rdz_info(path)$codec, "native_v1")
  expect_identical(restored, x)
  expect_identical(Encoding(names(restored)), Encoding(names(x)))
})

test_that("an empty names attribute remains distinct from no names", {
  named <- logical()
  names(named) <- character()
  unnamed <- logical()
  named_path <- tempfile(fileext = ".rdz")
  unnamed_path <- tempfile(fileext = ".rdz")
  on.exit(unlink(c(named_path, unnamed_path)), add = TRUE)

  write_rdz(named, named_path)
  write_rdz(unnamed, unnamed_path)

  expect_identical(read_rdz(named_path), named)
  expect_identical(read_rdz(unnamed_path), unnamed)
  expect_identical(rdz_info(named_path)$attribute_names, "names")
  expect_identical(rdz_info(unnamed_path)$attribute_names, character())
})

test_that("logical vectors keep any native attributes", {
  objects <- list(
    custom = structure(c(TRUE, NA), note = "preserve me"),
    classed = structure(c(TRUE, FALSE), class = "custom_logical"),
    matrix = matrix(c(TRUE, FALSE, NA, TRUE), nrow = 2L)
  )
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  for (name in names(objects)) {
    write_rdz(objects[[name]], path, mode = "native")
    expect_identical(rdz_info(path)$codec, "native_v1", info = name)
    expect_identical(read_rdz(path), objects[[name]], info = name)
    expect_setequal(rdz_info(path)$attribute_names, names(attributes(objects[[name]])))
  }
})

test_that("a logical vector with an attribute that is not native falls back whole", {
  x <- structure(c(TRUE, NA), env = globalenv())
  automatic <- tempfile(fileext = ".rdz")
  strict <- tempfile(fileext = ".rdz")
  on.exit(unlink(c(automatic, strict)), add = TRUE)
  write_rdz(x, automatic)
  expect_identical(rdz_info(automatic)$codec, "r_serial_v3")
  expect_identical(read_rdz(automatic), x)
  expect_error(write_rdz(x, strict, mode = "native"), "environment",
               class = "rdz_unsupported_error")
  expect_false(file.exists(strict))
})

test_that("forced R mode bypasses native logical encoding", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(c(TRUE, NA), path, mode = "r")

  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path), c(TRUE, NA))
})

test_that("native logical metadata is authoritative and selectively readable", {
  x <- c(first = TRUE, second = FALSE, missing = NA)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path)

  info <- rdz_info(path)
  schema <- rdz_schema(path)

  expect_true(info$authoritative)
  expect_true(info$exact_attributes)
  expect_false(info$full_read_required_for_attributes)
  expect_identical(info$object_count, 3L)
  expect_identical(info$attribute_count, 1L)
  expect_identical(schema$root_type, "logical")
  expect_identical(schema$length, 3)
  expect_identical(schema$attribute_names, "names")
  expect_false(schema$data_blocks_read)
  expect_identical(rdz_attributes(path), list(names = names(x)))
  expect_identical(rdz_attributes(path, names = "names"), list(names = names(x)))
  expect_error(rdz_attributes(path, names = "class"), "Unknown attribute")
})

test_that("selective names access does not read logical data blocks", {
  x <- c(first = TRUE, second = FALSE, missing = NA)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path)
  bytes <- readBin(path, what = "raw", n = file.info(path)$size)

  # The first native payload byte begins after the file and first block headers.
  bytes[[81L]] <- as.raw(bitwXor(as.integer(bytes[[81L]]), 0xffL))
  writeBin(bytes, path)

  expect_identical(rdz_schema(path)$root_type, "logical")
  expect_identical(rdz_attributes(path, names = "names"), list(names = names(x)))
  expect_error(read_rdz(path), "checksum mismatch")
})

test_that("generic attribute access requires explicit full-read permission", {
  x <- structure(1:3, note = "generic")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "r")

  expect_error(rdz_attributes(path), "allow_full = TRUE")
  expect_identical(
    rdz_attributes(path, names = "note", allow_full = TRUE),
    list(note = "generic")
  )
})
