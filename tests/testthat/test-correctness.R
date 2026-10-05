skip_without_rust()

test_that("atomic R object types round-trip exactly", {
  objects <- list(
    null = NULL,
    logical_empty = logical(),
    logical = c(FALSE, TRUE, NA),
    integer_empty = integer(),
    integer = c(-1L, 0L, 1L, NA_integer_),
    double_empty = double(),
    double = c(
      -Inf, -.Machine$double.xmax, -1, 0, .Machine$double.xmin,
      1, 1 + .Machine$double.eps, pi, .Machine$double.xmax,
      Inf, NaN, NA_real_
    ),
    complex_empty = complex(),
    complex = c(1 + 2i, -3 - 4i, NA_complex_),
    character_empty = character(),
    character = c("", "ASCII", enc2utf8("Grüezi"), NA_character_),
    raw_empty = raw(),
    raw = as.raw(c(0, 1, 127, 255))
  )

  for (name in names(objects)) {
    expect_identical(
      roundtrip_rdz(objects[[name]]),
      objects[[name]],
      info = name
    )
  }
})

test_that("structured and classed R objects round-trip exactly", {
  closure <- function(x = 1) x + 1
  environment(closure) <- baseenv()

  formula <- y ~ x + group
  environment(formula) <- baseenv()

  posixct_empty_tzone <- as.POSIXct("2026-01-01 12:34:56", tz = "UTC")
  attr(posixct_empty_tzone, "tzone") <- ""

  posixct_no_tzone <- posixct_empty_tzone
  attr(posixct_no_tzone, "tzone") <- NULL

  if (!methods::isClass("rdz_test_record")) {
    methods::setClass("rdz_test_record", slots = c(value = "numeric"))
  }

  objects <- list(
    named_vector = structure(1:3, names = c("a", "b", "c"), note = "attribute"),
    list = list(NULL, 1L, "x", list(nested = TRUE)),
    pairlist = pairlist(a = 1L, b = "x"),
    matrix = matrix(1:6, nrow = 2L, dimnames = list(c("a", "b"), letters[1:3])),
    array = array(1:8, dim = c(2L, 2L, 2L)),
    data_frame = data.frame(
      logical = c(TRUE, NA),
      integer = c(1L, NA_integer_),
      double = c(pi, NA_real_),
      character = c("x", NA_character_),
      stringsAsFactors = FALSE
    ),
    data_frame_empty = data.frame(),
    data_frame_zero_rows = data.frame(
      logical = logical(),
      integer = integer(),
      double = double(),
      character = character(),
      factor = factor(character(), levels = c("a", "b"))
    ),
    factor = factor(c("a", "b", NA), levels = c("b", "a")),
    factor_with_na_level = addNA(factor(c("a", NA))),
    ordered = ordered(c("low", "high", NA), levels = c("low", "high")),
    date = as.Date(c("2026-01-01", NA)),
    integer_date = structure(c(0L, NA_integer_), class = "Date"),
    posixct = as.POSIXct(c("2026-01-01 12:34:56", NA), tz = "UTC"),
    posixct_empty_tzone = posixct_empty_tzone,
    posixct_no_tzone = posixct_no_tzone,
    posixlt = as.POSIXlt(c("2026-01-01 12:34:56", NA), tz = "Europe/Zurich"),
    difftime = as.difftime(c(1, NA), units = "hours"),
    symbol = as.name("alpha"),
    call = quote(mean(x, na.rm = TRUE)),
    expression = expression(x + 1, alpha),
    formula = formula,
    closure = closure,
    bytecode = compiler::cmpfun(closure),
    builtin = .Primitive("sum"),
    special = .Primitive("if"),
    s3 = structure(list(value = 1L), class = "rdz_test_s3"),
    s4 = methods::new("rdz_test_record", value = c(pi, NA_real_)),
    compact_sequence = 1:10000
  )

  for (name in names(objects)) {
    expect_identical(
      roundtrip_rdz(objects[[name]]),
      objects[[name]],
      info = name
    )
  }
})

test_that("character and name encodings are preserved", {
  latin1 <- iconv("Ärende", from = "UTF-8", to = "latin1")
  utf8 <- enc2utf8("日本語")
  bytes <- rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9)))
  Encoding(bytes) <- "bytes"

  object <- c(native = "plain", latin1 = latin1, utf8 = utf8, bytes = bytes)
  names(object)[3] <- utf8
  restored <- roundtrip_rdz(object)

  expect_identical(restored, object)
  expect_identical(Encoding(restored), Encoding(object))
  expect_identical(Encoding(names(restored)), Encoding(names(object)))
})

test_that("common length boundaries round-trip exactly", {
  sizes <- c(0L, 1L, 2L, 127L, 128L, 255L, 256L, 4095L, 4096L, 4097L)

  for (size in sizes) {
    object <- list(
      logical = rep(c(FALSE, TRUE, NA), length.out = size),
      integer = rep(c(-1L, 0L, 1L, NA_integer_), length.out = size),
      double = rep(c(-Inf, pi, NaN, NA_real_), length.out = size),
      complex = rep(c(1 + 2i, NA_complex_), length.out = size),
      character = rep(c("", enc2utf8("Grüezi"), NA_character_), length.out = size),
      raw = as.raw(seq_len(size) %% 256L)
    )

    expect_identical(roundtrip_rdz(object), object, info = paste("length", size))
  }
})

test_that("environments, cycles, and shared references are preserved", {
  env <- new.env(parent = emptyenv())
  env$value <- list(a = 1L, b = "two")
  env$self <- env

  restored <- roundtrip_rdz(env)

  expect_identical(restored$value, env$value)
  expect_identical(restored$self, restored)
  expect_identical(parent.env(restored), emptyenv())

  shared <- new.env(parent = emptyenv())
  shared$value <- 42L
  restored_shared <- roundtrip_rdz(list(first = shared, second = shared))

  expect_identical(restored_shared$first, restored_shared$second)
  expect_identical(restored_shared$first$value, 42L)
})

test_that("closures retain their captured values", {
  add_offset <- local({
    offset <- 2L
    function(x) x + offset
  })

  restored <- roundtrip_rdz(add_offset)

  expect_identical(restored(3L), 5L)
  expect_identical(get("offset", envir = environment(restored)), 2L)
})

# Connections, external pointers, weak references, and similar native resources
# are process-specific. Their underlying resources cannot be recreated by an
# exact, portable serialization round-trip and are deliberately not in this
# compatibility matrix.
