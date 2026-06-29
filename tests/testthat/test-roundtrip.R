roundtrip <- function(x, codec = "auto", preset = "speed") {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path))
  expect_identical(write_rdz(x, path, codec, preset), invisible(path))
  read_rdz(path)
}

test_that("native atomic vectors round trip exactly", {
  cases <- list(
    NULL,
    c(TRUE, FALSE, NA),
    c(1L, NA_integer_, -2L),
    c(1, NA_real_, NaN, Inf, -Inf),
    c(1 + 2i, NA_complex_),
    as.raw(c(0, 127, 255)),
    character(),
    rep(NA_character_, 100),
    c("repeated", NA, "repeated", enc2utf8("Grüezi")),
    c(sprintf("unique-%06d", 1:2000), NA_character_)
  )
  for (x in cases) expect_identical(roundtrip(x, "native"), x)
})

test_that("string dictionary index widths round trip exactly", {
  make_dictionary_case <- function(length, unique_count) {
    x <- rep("seed", length)
    sampled <- floor((0:1023) * length / 1024) + 1L
    available <- setdiff(seq_len(length), sampled)
    x[available[seq_len(unique_count)]] <-
      sprintf("unique-%05d", seq_len(unique_count))
    x
  }
  cases <- list(
    `1` = rep(sprintf("group-%03d", 1:100), length.out = 10000L),
    `2` = make_dictionary_case(5000L, 300L),
    `4` = make_dictionary_case(70000L, 66000L)
  )

  for (width in names(cases)) {
    path <- tempfile(fileext = ".rdz")
    write_rdz(cases[[width]], path, codec = "native")
    header <- readBin(path, "raw", n = 27L)
    expect_identical(as.integer(header[[27L]]), as.integer(width))
    expect_identical(read_rdz(path), cases[[width]])
  }
})

test_that("balanced flat strings use LZ4 only when smaller", {
  x <- c(sprintf("unique-value-%08d", seq_len(50000L)), NA_character_)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")

  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")
  balanced_header <- readBin(balanced_path, "raw", n = 27L)

  expect_identical(as.integer(balanced_header[[27L]]), 3L)
  expect_identical(read_rdz(balanced_path), x)
  expect_lt(file.info(balanced_path)$size, file.info(speed_path)$size / 2)

  set.seed(20260628)
  alphabet <- c(letters, LETTERS, as.character(0:9))
  random <- replicate(
    4000L,
    paste0(sample(alphabet, 64L, replace = TRUE), collapse = "")
  )
  write_rdz(random, speed_path)
  write_rdz(random, balanced_path, preset = "balanced")
  balanced_header <- readBin(balanced_path, "raw", n = 27L)

  expect_identical(as.integer(balanced_header[[27L]]), 0L)
  expect_identical(read_rdz(balanced_path), random)
  expect_identical(file.info(balanced_path)$size, file.info(speed_path)$size)
})

test_that("balanced LZ4 strings preserve encodings and block boundaries", {
  x <- sprintf("compressible-prefix-%08d", seq_len(10000L))
  x[[2L]] <- enc2utf8("Grüezi")
  x[[3L]] <- iconv("café", from = "UTF-8", to = "latin1")
  Encoding(x[[3L]]) <- "latin1"
  x[[4L]] <- NA_character_
  x[[5L]] <- strrep("abcd", 50000L)
  path <- tempfile(fileext = ".rdz")

  write_rdz(x, path, preset = "balanced")
  header <- readBin(path, "raw", n = 27L)

  expect_identical(as.integer(header[[27L]]), 3L)
  expect_identical(read_rdz(path), x)

  direct_lengths <- vapply(
    seq_len(100L),
    function(i) strrep("a", 20000L + i),
    character(1)
  )
  write_rdz(direct_lengths, path, preset = "balanced")
  header <- readBin(path, "raw", n = 37L)

  expect_identical(as.integer(header[[27L]]), 3L)
  expect_identical(as.integer(header[[37L]]), 0L)
  expect_identical(read_rdz(path), direct_lengths)
})

test_that("malformed balanced LZ4 string blocks fail cleanly", {
  x <- sprintf("compressible-prefix-%08d", seq_len(10000L))
  path <- tempfile(fileext = ".rdz")
  write_rdz(x, path, preset = "balanced")
  bytes <- readBin(path, "raw", n = file.info(path)$size)
  expect_identical(as.integer(bytes[[27L]]), 3L)
  expect_identical(as.integer(bytes[[37L]]), 1L)

  length_size_bytes <- as.integer(bytes[38:45])
  if (.Platform$endian == "big") length_size_bytes <- rev(length_size_bytes)
  length_metadata_size <- sum(length_size_bytes * 256^(0:7))
  block_header <- 46L + as.integer(length_metadata_size)

  malformed <- bytes
  malformed[[37L]] <- as.raw(255L)
  writeBin(malformed, path)
  expect_error(read_rdz(path), "unknown string length encoding")

  malformed <- bytes
  malformed[[46L]] <- as.raw(0L)
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid string length metadata")

  malformed <- bytes
  malformed[[37L]] <- as.raw(0L)
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid string length metadata")

  malformed <- bytes
  malformed[block_header + 0:3] <- writeBin(
    0L, raw(), size = 4L, endian = .Platform$endian
  )
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 string block")

  malformed <- bytes
  malformed[block_header + 0:3] <- writeBin(
    65537L, raw(), size = 4L, endian = .Platform$endian
  )
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 string block")

  malformed <- bytes
  malformed[block_header + 0:3] <- writeBin(
    1L, raw(), size = 4L, endian = .Platform$endian
  )
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 string block")

  writeBin(bytes[-length(bytes)], path)
  expect_error(read_rdz(path), "truncated")
})

test_that("attributes, matrices, lists, and data frames round trip", {
  cases <- list(
    structure(1:4, label = "integer vector"),
    matrix(1:12, 3, dimnames = list(letters[1:3], LETTERS[1:4])),
    list(a = c(1L, 2L, 3L), b = c("x", "y"), nested = list(flag = TRUE)),
    data.frame(
      integer = c(1L, NA, 3L),
      real = c(1, NaN, Inf),
      string = c("x", NA, "x"),
      factor = factor(c("a", "b", NA))
    ),
    structure(as.double(1:3), class = "Date")
  )
  for (x in cases) expect_identical(roundtrip(x, "native"), x)
})

test_that("auto mode falls back for general R objects", {
  fun <- function(x) x + 1
  expression <- quote(mean(x, na.rm = TRUE))
  fun_copy <- roundtrip(fun)
  expect_identical(formals(fun_copy), formals(fun))
  expect_identical(body(fun_copy), body(fun))
  expect_identical(fun_copy(2), 3)
  expect_identical(roundtrip(expression), expression)
  list_copy <- roundtrip(list(fun = fun, values = 1:1000))
  expect_identical(list_copy$values, 1:1000)
  expect_identical(list_copy$fun(2), 3)
})

test_that("codec selection is enforced", {
  path <- tempfile(fileext = ".rdz")
  expect_error(write_rdz(function() NULL, path, "native"),
               "not supported")
  x <- list(a = 1:10, call = quote(sum(a)))
  expect_identical(roundtrip(x, "r"), x)
  expect_identical(roundtrip(x, "r", "balanced"), x)
})

test_that("legacy function names remain compatible aliases", {
  expect_identical(write_fastrds, write_rdz)
  expect_identical(read_fastrds, read_rdz)
  expect_identical(explain_fastrds, explain_rdz)

  path <- tempfile(fileext = ".rdz")
  x <- c(1L, 2L, NA_integer_)
  expect_identical(write_fastrds(x, path), invisible(path))
  expect_identical(read_fastrds(path), x)
})

test_that("serialization plans describe native node strategies", {
  sequence <- numeric(3000L)
  sequence[] <- seq_len(3000L)
  set.seed(20260629)
  x <- list(
    logical = rep(c(TRUE, FALSE, NA), 1000L),
    integer = rep(seq_len(100L), 30L),
    repeated = rep(c(0, 1, NA_real_), 1000L),
    sequence = sequence,
    random = runif(3000L),
    strings = sprintf("unique-%06d", seq_len(3000L))
  )
  plan <- explain_rdz(x, preset = "balanced")
  path <- tempfile(fileext = ".rdz")
  write_rdz(x, path, preset = "balanced")

  expect_s3_class(plan, "data.frame")
  expect_named(plan, c(
    "path", "relation", "name", "index", "depth", "type", "length",
    "codec", "strategy", "encoded_bytes"
  ))
  expect_identical(plan$path, c(
    "$", "$@names", "$[[1]]", "$[[2]]", "$[[3]]", "$[[4]]",
    "$[[5]]", "$[[6]]"
  ))
  expect_identical(plan$name, c(
    NA_character_, "names", names(x)
  ))
  expect_true(all(plan$codec == "native"))
  expect_identical(plan$encoded_bytes[[1L]], as.numeric(file.info(path)$size))
  expect_match(plan$strategy[which(plan$name == "logical")], "two-bit")
  expect_match(plan$strategy[which(plan$name == "integer")], "frame-of-reference")
  expect_match(plan$strategy[which(plan$name == "repeated")], "numeric dictionary")
  expect_match(plan$strategy[which(plan$name == "sequence")], "constant-delta")
  expect_match(plan$strategy[which(plan$name == "random")], "XOR-delta")
  expect_identical(
    plan$strategy[which(plan$name == "strings")],
    "flat strings + LZ4"
  )

  speed <- explain_rdz(x, preset = "speed")
  expect_match(speed$strategy[which(speed$name == "logical")], "direct")
  expect_match(speed$strategy[which(speed$name == "integer")], "direct")
  expect_match(speed$strategy[which(speed$name == "random")], "direct")
  expect_match(speed$strategy[which(speed$name == "strings")], "raw")
})

test_that("serialization plans explain fallback and forced codecs", {
  fn <- function(x) x + 1
  environment(fn) <- emptyenv()
  fallback <- explain_rdz(fn)
  fallback_path <- tempfile(fileext = ".rdz")
  write_rdz(fn, fallback_path)
  expect_equal(nrow(fallback), 1L)
  expect_identical(fallback$codec, "R serialization")
  expect_match(fallback$strategy, "native codec unsupported or ALTREP")
  expect_true(is.na(fallback$length))
  expect_identical(
    fallback$encoded_bytes,
    unname(as.numeric(file.info(fallback_path)$size))
  )

  altrep <- explain_rdz(seq_len(1000L))
  expect_identical(altrep$codec, "R serialization")
  expect_equal(altrep$length, 1000)
  expect_match(altrep$strategy, "ALTREP")

  forced <- explain_rdz(c(1, 2, 3), codec = "r")
  forced_path <- tempfile(fileext = ".rdz")
  write_rdz(c(1, 2, 3), forced_path, codec = "r")
  expect_identical(forced$codec, "R serialization")
  expect_equal(forced$length, 3)
  expect_match(forced$strategy, "forced")
  expect_identical(
    forced$encoded_bytes,
    unname(as.numeric(file.info(forced_path)$size))
  )

  expect_error(explain_rdz(function() NULL, codec = "native"),
               "not supported")
})

test_that("serialization plans preserve nested paths and names", {
  outer_name <- enc2utf8("gr\u00f6sse")
  values <- integer(3L)
  values[] <- seq_len(3L)
  x <- setNames(list(list(inner = values)), outer_name)
  plan <- explain_rdz(x)

  expect_identical(plan$path, c(
    "$", "$@names", "$[[1]]", "$[[1]]@names", "$[[1]][[1]]"
  ))
  expect_identical(
    plan$name[which(plan$relation == "element")],
    c(outer_name, "inner")
  )
  expect_identical(
    plan$index[which(plan$relation == "element")],
    c(1, 1)
  )
  expect_identical(
    plan$depth[which(plan$relation == "element")],
    c(1L, 2L)
  )
})

test_that("ALTREP objects retain their compact R representation", {
  path <- tempfile(fileext = ".rdz")
  x <- seq_len(1000000L)
  write_rdz(x, path)
  header <- readBin(path, "raw", n = 9L)

  expect_identical(as.integer(header[[9L]]), 2L)
  expect_lt(file.info(path)$size, 10000)
  expect_identical(read_rdz(path), x)
})

test_that("balanced preset packs logical vectors into two bits", {
  for (length in 0:9) {
    x <- rep(c(FALSE, TRUE, NA), length.out = length)
    expect_identical(roundtrip(x, preset = "balanced"), x)
  }

  x <- rep(c(FALSE, TRUE, NA), length.out = 1000000L)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")
  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(read_rdz(balanced_path), x)
  expect_equal(file.info(speed_path)$size, 26 + 4 * length(x))
  expect_equal(file.info(balanced_path)$size, 27 + ceiling(length(x) / 4))
  expect_lt(file.info(balanced_path)$size, file.info(speed_path)$size / 15)
})

test_that("balanced logical packing works inside data frames", {
  x <- data.frame(
    flag = rep(c(FALSE, TRUE, NA), 10000L),
    value = rep(c(1, 2, NA_real_), 10000L)
  )
  expect_identical(roundtrip(x, preset = "balanced"), x)
})

test_that("balanced preset packs small-range integer offsets", {
  all_na_path <- tempfile(fileext = ".rdz")
  cases <- list(
    rep(NA_integer_, 10000L),
    rep(.Machine$integer.max, 10000L),
    rep(-.Machine$integer.max, 10000L),
    rep(c(-100L, 0L, 100L, NA_integer_), 2500L)
  )
  for (x in cases) {
    expect_identical(roundtrip(x, preset = "balanced"), x)
  }
  write_rdz(cases[[1L]], all_na_path, preset = "balanced")
  expect_equal(file.info(all_na_path)$size, 33)

  x <- rep(c(1:100, NA_integer_), length.out = 1000000L)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")
  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(read_rdz(balanced_path), x)
  expect_equal(file.info(speed_path)$size, 26 + 4 * length(x))
  expect_equal(file.info(balanced_path)$size,
               33 + ceiling(7 * length(x) / 8))
  expect_lt(file.info(balanced_path)$size, file.info(speed_path)$size / 4)
})

test_that("integer packing reserves an NA code only when needed", {
  without_na <- rep(1:128, length.out = 10000L)
  with_na <- rep(c(1:128, NA_integer_), length.out = 10000L)
  without_path <- tempfile(fileext = ".rdz")
  with_path <- tempfile(fileext = ".rdz")

  write_rdz(without_na, without_path, preset = "balanced")
  write_rdz(with_na, with_path, preset = "balanced")
  without_header <- readBin(without_path, "raw", n = 33L)
  with_header <- readBin(with_path, "raw", n = 33L)

  expect_identical(as.integer(without_header[[32L]]), 7L)
  expect_identical(as.integer(without_header[[33L]]), 0L)
  expect_identical(as.integer(with_header[[32L]]), 8L)
  expect_identical(as.integer(with_header[[33L]]), 1L)
  expect_identical(read_rdz(without_path), without_na)
  expect_identical(read_rdz(with_path), with_na)
})

test_that("integer packing round trips every selected bit width", {
  base <- -1000L
  for (bits in 1:8) {
    maximum_code <- as.integer(2^bits - 1)
    cases <- list(
      rep(c(base, base + maximum_code), length.out = 1001L),
      rep(c(base, base + maximum_code - 1L, NA_integer_),
          length.out = 1001L)
    )
    for (x in cases) {
      path <- tempfile(fileext = ".rdz")
      write_rdz(x, path, preset = "balanced")
      header <- readBin(path, "raw", n = 33L)
      expect_identical(as.integer(header[[32L]]), bits)
      expect_identical(read_rdz(path), x)
    }
  }
})

test_that("balanced preset leaves wide-range integers direct", {
  x <- rep(c(-.Machine$integer.max, .Machine$integer.max), 10000L)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")
  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(read_rdz(balanced_path), x)
  expect_equal(file.info(balanced_path)$size,
               file.info(speed_path)$size + 1)
})

test_that("integer packing preserves factors and data frames", {
  x <- data.frame(
    id = rep(c(-50L, 0L, 50L, NA_integer_), 10000L),
    group = factor(rep(sprintf("level-%03d", 1:100), 400L))
  )
  expect_identical(roundtrip(x, preset = "balanced"), x)
})

test_that("balanced preset dictionary-encodes low-cardinality numerics", {
  x <- rep(c(0, -0, NA_real_, NaN, Inf, -Inf), length.out = 100000L)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")

  write_rdz(x, speed_path, preset = "speed")
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(read_rdz(speed_path), x)
  expect_identical(read_rdz(balanced_path), x)
  expect_identical(
    as.integer(readBin(balanced_path, "raw", n = 27L)[[27L]]), 3L
  )
  expect_lt(file.info(balanced_path)$size, file.info(speed_path)$size / 100)
  expect_identical(rawToChar(readBin(speed_path, "raw", n = 8L)), "FASTRDS1")
  expect_identical(rawToChar(readBin(balanced_path, "raw", n = 8L)), "FASTRDS2")
})

test_that("balanced numeric bit widths round trip exactly", {
  for (cardinality in c(1L, 2L, 3L, 4L, 7L, 16L, 63L)) {
    x <- rep(seq_len(cardinality) + 0, length.out = 10000L)
    expect_identical(roundtrip(x, preset = "balanced"), x)
  }

  x <- numeric(100000L)
  x[seq_len(255L)] <- seq_len(255L)
  expect_identical(roundtrip(x, preset = "balanced"), x)
})

test_that("balanced numeric dictionaries retain raw packed random indexes", {
  set.seed(20260628)
  x <- sample(seq_len(64L) + 0, 100000L, replace = TRUE)
  path <- tempfile(fileext = ".rdz")

  write_rdz(x, path, preset = "balanced")
  header <- readBin(path, "raw", n = 27L)

  expect_identical(as.integer(header[[27L]]), 1L)
  expect_identical(read_rdz(path), x)
})

test_that("balanced preset leaves high-cardinality numerics direct", {
  set.seed(1)
  x <- runif(10000L)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")

  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(read_rdz(balanced_path), x)
  expect_lte(file.info(balanced_path)$size, file.info(speed_path)$size + 1)
})

test_that("balanced numeric dictionaries work inside data frames", {
  x <- data.frame(value = rep(c(1, 2, NA_real_), 10000L), id = seq_len(30000L))
  expect_identical(roundtrip(x, preset = "balanced"), x)
})

test_that("balanced preset preserves exact constant-delta sequences", {
  cases <- list(
    seq_len(100000L) + 0,
    100000 - 2 * seq_len(100000L),
    0.5 + 0.25 * seq_len(100000L),
    as.Date("2000-01-01") + seq_len(100000L),
    as.POSIXct("2000-01-01", tz = "UTC") + seq_len(100000L)
  )
  for (x in cases) {
    speed_path <- tempfile(fileext = ".rdz")
    balanced_path <- tempfile(fileext = ".rdz")
    write_rdz(x, speed_path)
    write_rdz(x, balanced_path, preset = "balanced")

    expect_identical(read_rdz(balanced_path), x)
    expect_lt(file.info(balanced_path)$size, file.info(speed_path)$size / 100)
  }
})

test_that("balanced preset leaves irregular numeric sequences direct", {
  x <- seq_len(10000L) + 0
  x[[5000L]] <- x[[5000L]] + 0.5
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")
  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(read_rdz(balanced_path), x)
  expect_lte(file.info(balanced_path)$size, file.info(speed_path)$size + 1)
})

test_that("balanced preset XOR-compresses high-cardinality numerics exactly", {
  set.seed(20260628)
  x <- c(runif(100000L), -0, NA_real_, NaN, Inf, -Inf)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")

  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")
  restored <- read_rdz(balanced_path)
  header <- readBin(balanced_path, "raw", n = 27L)

  expect_identical(as.integer(header[[27L]]), 4L)
  expect_identical(restored, x)
  expect_identical(writeBin(restored, raw(), size = 8L),
                   writeBin(x, raw(), size = 8L))
  expect_lt(file.info(balanced_path)$size, file.info(speed_path)$size * 0.7)
})

test_that("balanced XOR numerics preserve block boundaries", {
  set.seed(20260628)
  for (length in c(8191L, 8192L, 8193L, 16385L)) {
    x <- seq_len(length) * 0.01 + rnorm(length, sd = 0.001)
    x[[length]] <- NA_real_
    path <- tempfile(fileext = ".rdz")
    write_rdz(x, path, preset = "balanced")

    expect_identical(as.integer(readBin(path, "raw", n = 27L)[[27L]]), 4L)
    expect_identical(read_rdz(path), x)
  }
})

test_that("balanced XOR numerics reject incompressible bit patterns", {
  set.seed(20260628)
  bytes <- as.raw(sample.int(256L, 80000L, replace = TRUE) - 1L)
  x <- readBin(bytes, "double", n = 10000L, size = 8L,
               endian = .Platform$endian)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")

  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(as.integer(readBin(balanced_path, "raw", n = 27L)[[27L]]),
                   0L)
  expect_identical(read_rdz(balanced_path), x)
  expect_equal(file.info(balanced_path)$size,
               file.info(speed_path)$size + 1)
})

test_that("balanced XOR numerics reject marginal compression gains", {
  set.seed(20260628)
  x <- rnorm(100000L)
  speed_path <- tempfile(fileext = ".rdz")
  balanced_path <- tempfile(fileext = ".rdz")

  write_rdz(x, speed_path)
  write_rdz(x, balanced_path, preset = "balanced")

  expect_identical(as.integer(readBin(balanced_path, "raw", n = 27L)[[27L]]),
                   0L)
  expect_identical(read_rdz(balanced_path), x)
  expect_equal(file.info(balanced_path)$size,
               file.info(speed_path)$size + 1)
})

test_that("malformed LZ4 XOR numeric blocks fail cleanly", {
  path <- tempfile(fileext = ".rdz")
  set.seed(20260628)
  x <- runif(10000L)
  write_rdz(x, path, preset = "balanced")
  bytes <- readBin(path, "raw", n = file.info(path)$size)
  expect_identical(as.integer(bytes[[27L]]), 4L)

  malformed <- bytes
  malformed[28:31] <- writeBin(0L, raw(), size = 4L,
                               endian = .Platform$endian)
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 XOR numeric block")

  malformed <- bytes
  malformed[28:31] <- writeBin(65537L, raw(), size = 4L,
                               endian = .Platform$endian)
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 XOR numeric block")

  malformed <- bytes
  malformed[28:31] <- writeBin(1L, raw(), size = 4L,
                               endian = .Platform$endian)
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 XOR numeric block")

  writeBin(bytes[-length(bytes)], path)
  expect_error(read_rdz(path), "truncated")
})

test_that("malformed balanced numeric sequences fail cleanly", {
  path <- tempfile(fileext = ".rdz")
  x <- seq_len(1000L) + 0
  write_rdz(x, path, preset = "balanced")
  bytes <- readBin(path, "raw", n = file.info(path)$size)
  bytes[36:43] <- writeBin(NaN, raw(), size = 8L)
  writeBin(bytes, path)

  expect_error(read_rdz(path), "invalid numeric sequence")
})

test_that("malformed balanced numeric dictionaries fail cleanly", {
  path <- tempfile(fileext = ".rdz")
  x <- rep(c(1, 2), 1000L)
  write_rdz(x, path, preset = "balanced")
  bytes <- readBin(path, "raw", n = file.info(path)$size)
  bytes[[27L]] <- as.raw(255L)
  writeBin(bytes, path)

  expect_error(read_rdz(path), "unknown numeric encoding")
})

test_that("malformed LZ4 numeric dictionary blocks fail cleanly", {
  path <- tempfile(fileext = ".rdz")
  x <- rep(c(0, 1, NA_real_), length.out = 10000L)
  write_rdz(x, path, preset = "balanced")
  bytes <- readBin(path, "raw", n = file.info(path)$size)
  expect_identical(as.integer(bytes[[27L]]), 3L)

  block_header <- 31L + 3L * 8L
  malformed <- bytes
  malformed[block_header + 0:3] <- writeBin(
    0L, raw(), size = 4L, endian = .Platform$endian
  )
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 numeric dictionary block")

  malformed <- bytes
  malformed[block_header + 0:3] <- writeBin(
    2501L, raw(), size = 4L, endian = .Platform$endian
  )
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 numeric dictionary block")

  malformed <- bytes
  malformed[block_header + 0:3] <- writeBin(
    1L, raw(), size = 4L, endian = .Platform$endian
  )
  writeBin(malformed, path)
  expect_error(read_rdz(path), "invalid LZ4 numeric dictionary block")

  writeBin(bytes[-length(bytes)], path)
  expect_error(read_rdz(path), "truncated")
})

test_that("malformed balanced logical vectors fail cleanly", {
  path <- tempfile(fileext = ".rdz")
  write_rdz(rep(FALSE, 5L), path, preset = "balanced")
  bytes <- readBin(path, "raw", n = file.info(path)$size)

  unknown_encoding <- bytes
  unknown_encoding[[27L]] <- as.raw(255L)
  writeBin(unknown_encoding, path)
  expect_error(read_rdz(path), "unknown logical encoding")

  invalid_code <- bytes
  invalid_code[[28L]] <- as.raw(3L)
  writeBin(invalid_code, path)
  expect_error(read_rdz(path), "invalid packed logical vector")

  write_rdz(FALSE, path, preset = "balanced")
  invalid_padding <- readBin(path, "raw", n = file.info(path)$size)
  invalid_padding[[28L]] <- as.raw(4L)
  writeBin(invalid_padding, path)
  expect_error(read_rdz(path), "invalid packed logical vector")
})

test_that("malformed balanced integer vectors fail cleanly", {
  path <- tempfile(fileext = ".rdz")
  x <- rep(1:3, length.out = 1001L)
  write_rdz(x, path, preset = "balanced")
  bytes <- readBin(path, "raw", n = file.info(path)$size)

  unknown_encoding <- bytes
  unknown_encoding[[27L]] <- as.raw(255L)
  writeBin(unknown_encoding, path)
  expect_error(read_rdz(path), "unknown integer encoding")

  invalid_bits <- bytes
  invalid_bits[[32L]] <- as.raw(25L)
  writeBin(invalid_bits, path)
  expect_error(read_rdz(path), "invalid packed integer metadata")

  invalid_flags <- bytes
  invalid_flags[[33L]] <- as.raw(2L)
  writeBin(invalid_flags, path)
  expect_error(read_rdz(path), "invalid packed integer vector")

  invalid_padding <- bytes
  invalid_padding[[length(invalid_padding)]] <- as.raw(
    bitwOr(as.integer(invalid_padding[[length(invalid_padding)]]), 128L)
  )
  writeBin(invalid_padding, path)
  expect_error(read_rdz(path), "invalid packed integer vector")
})

test_that("invalid and truncated files fail cleanly", {
  path <- tempfile(fileext = ".rdz")
  writeBin(charToRaw("not rdz"), path)
  expect_error(read_rdz(path), "not a rdz file|truncated")

  write_rdz(data.frame(x = 1:10), path)
  bytes <- readBin(path, "raw", n = file.info(path)$size)
  writeBin(bytes[seq_len(15)], path)
  expect_error(read_rdz(path), "truncated")
})
