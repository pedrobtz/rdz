# Native lists and data frames (plan-c Stage H).

roundtrip_native <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "native")
  expect_identical(rdz_info(path)$codec, "native_v1")
  read_rdz(path)
}

test_that("lists round-trip: empty, nested, heterogeneous, named, with NULL", {
  cases <- list(
    empty = list(),
    flat = list(1L, 2.5, "a", TRUE, NULL),
    named = list(a = 1:3 + 0L, b = c("x", NA), c = list()),
    partly_named = stats::setNames(list(1, 2), c("a", "")),
    nested = list(list(list(list(1)), list()), list(NULL)),
    factors = list(factor(c("a", NA)), factor("z", levels = c("y", "z"), ordered = TRUE))
  )
  for (name in names(cases)) {
    expect_identical(roundtrip_native(cases[[name]]), cases[[name]], label = name)
  }
})

test_that("a vector shared within a list is written as the copies R serializes", {
  v <- runif(10)
  x <- list(v, v, list(v))
  y <- roundtrip_native(x)
  expect_identical(y, x)
})

test_that("nesting is bounded and deeper objects stay generic", {
  deep <- list(1)
  for (i in seq_len(999)) deep <- list(deep)
  expect_identical(roundtrip_native(deep), deep) # 1000 levels below the root
  deeper <- list(deep)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  expect_error(write_rdz(deeper, path, mode = "native"), "nested more deeply",
               class = "rdz_unsupported_error")
  write_rdz(deeper, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path), deeper)
})

test_that("an unsupported part anywhere sends the whole root to the generic codec", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  cases <- list(
    call = list(1, list(quote(f(x)))),
    environment = list(a = 1, e = globalenv()),
    attribute = list(structure(1:3, note = "x")),
    altrep = list(1:10),
    complex = list(1i)
  )
  for (name in names(cases)) {
    write_rdz(cases[[name]], path)
    expect_identical(rdz_info(path)$codec, "r_serial_v3", label = name)
    expect_identical(read_rdz(path), cases[[name]], label = name)
    expect_error(write_rdz(cases[[name]], path, mode = "native"), class = "rdz_unsupported_error")
  }
})

test_that("data frames round-trip in every supported shape", {
  cases <- list(
    mixed = data.frame(l = c(TRUE, NA), i = 1:2 * 1L, d = c(0.5, NaN), s = c("a", NA),
                       f = factor(c("u", "v")), stringsAsFactors = FALSE),
    zero_rows = data.frame(a = integer(), b = character()),
    zero_columns = data.frame(),
    zero_columns_rows = data.frame(row.names = 1:3),
    char_row_names = data.frame(a = 1:2 * 1L, row.names = c("r1", "r2")),
    int_row_names = structure(list(a = 1:3 * 1L), names = "a", row.names = c(5L, 2L, 9L),
                              class = "data.frame"),
    duplicate_names = stats::setNames(data.frame(1:2 * 1L, 3:4 * 1L), c("a", "a")),
    empty_name = stats::setNames(data.frame(1:2 * 1L), ""),
    list_column = local({
      d <- data.frame(a = 1:2 * 1L)
      d$b <- list(1, "x")
      d
    })
  )
  for (name in names(cases)) {
    expect_identical(roundtrip_native(cases[[name]]), cases[[name]], label = name)
  }
  big <- data.frame(x = runif(5e5), y = sample.int(100L, 5e5, TRUE),
                    z = sample(letters, 5e5, TRUE), stringsAsFactors = FALSE)
  old <- options(rdz.threads = 4L)
  on.exit(options(old), add = TRUE)
  expect_identical(roundtrip_native(big), big)
  expect_identical(.row_names_info(roundtrip_native(big)), .row_names_info(big))
})

test_that("tibbles and data.tables keep their class and work", {
  skip_if_not_installed("tibble")
  tb <- tibble::tibble(a = 1:3 * 1L, b = c("x", "y", "z"))
  expect_identical(roundtrip_native(tb), tb)
  skip_if_not_installed("data.table")
  dt <- data.table::data.table(a = 1:3 * 1L, b = c("x", "y", "z"))
  got <- roundtrip_native(dt)
  expect_identical(class(got), c("data.table", "data.frame"))
  expect_true(data.table::is.data.table(got))
  expect_equal(as.data.frame(got), as.data.frame(dt))
  # its self-reference restored: room to add columns by reference, and
  # set() adds one without the warning an invalid selfref gives
  expect_gt(data.table::truelength(got), ncol(got))
  expect_silent(data.table::set(got, j = "c", value = got$a * 2L))
  expect_identical(got$c, c(2L, 4L, 6L))
  # another external pointer attribute is not the registered one
  bad <- data.table::copy(dt)
  attr(bad, "other") <- new("externalptr")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(bad, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
})

test_that("a data frame's schema and attributes are read without its columns", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(a = 1:3 * 1L, b = c("x", "y", "z"), row.names = c("p", "q", "r"))
  write_rdz(df, path)
  info <- rdz_info(path)
  expect_identical(info$root_type, "data.frame")
  expect_identical(info$root_length, 2)
  expect_identical(
    rdz_attributes(path, names = c("names", "row.names", "class")),
    list(names = c("a", "b"), row.names = c("p", "q", "r"), class = "data.frame")
  )
})

test_that("columns of different lengths are refused before anything is written", {
  bad <- structure(list(a = 1:3 * 1L, b = 1:2 * 1L), names = c("a", "b"),
                   row.names = c(NA_integer_, -3L), class = "data.frame")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  expect_error(write_rdz(bad, path, mode = "native"), "differ in length",
               class = "rdz_unsupported_error")
  expect_false(file.exists(path))
})
