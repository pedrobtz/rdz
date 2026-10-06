# read_rdz(select =): columns of a data frame, elements of a list. Natively,
# only the selected parts are read; generically, the whole value is read and
# subset. Either way the value is the selected parts with their names (and a
# data frame's row names and class).

test_that("data frame columns are selected by name and position, in order", {
  df <- data.frame(
    id = c(1L, 2L, 3L), score = c(1.5, NA, -0), name = c("a", NA, "c"),
    day = as.Date("2026-10-06") + 0:2, g = factor(c("x", "y", "x")),
    stringsAsFactors = FALSE
  )
  expect_identical(select_both(df, c("name", "id")), df[c("name", "id")])
  expect_identical(select_both(df, c(4, 1)), df[c(4, 1)])
  expect_identical(select_both(df, "g"), df["g"])
  rownames(df) <- c("r1", "r2", "r3")
  expect_identical(select_both(df, c("score", "day")), df[c("score", "day")])
  expect_identical(select_both(df, integer()), df[integer()])
})

test_that("list elements are selected, nested parts whole", {
  x <- list(a = 1:3, b = list(c = "x", d = list(NULL, TRUE)), e = NULL, f = c(u = 2.5))
  expect_identical(select_both(x, c("f", "b")), x[c("f", "b")])
  expect_identical(select_both(x, 3), x[3])
  unnamed <- list(1L, "two", list(3))
  expect_identical(select_both(unnamed, c(3, 1)), unnamed[c(3, 1)])
  expect_error(select_both(unnamed, "a"), "no names")
})

test_that("a selection keeps names, row names and class, not other root attributes", {
  x <- structure(list(a = 1, b = 2), meta = "dropped", class = "myclass")
  expect_identical(select_both(x, "b"), list(b = 2))
  tb <- structure(list(x = 1:2, y = c("a", "b")), class = c("tbl_df", "tbl", "data.frame"),
                  row.names = c(NA, -2L), label = "dropped")
  expect_identical(
    select_both(tb, "y"),
    structure(list(y = c("a", "b")), class = c("tbl_df", "tbl", "data.frame"),
              row.names = c(NA, -2L))
  )
  # a column's own attributes stay
  df <- data.frame(t = as.POSIXct(c(0, 60), origin = "1970-01-01", tz = "UTC"), n = 1:2)
  expect_identical(attr(select_both(df, "t")$t, "tzone"), "UTC")
})

test_that("a data.table selection is a usable data.table without its key", {
  skip_if_not_installed("data.table")
  dt <- data.table::data.table(k = c(2L, 1L, 3L), v = c("b", "a", "c"), w = 1:3)
  data.table::setkeyv(dt, "k")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(dt, path)
  got <- read_rdz(path, select = c("v", "w"))
  expect_s3_class(got, "data.table")
  expect_null(data.table::key(got))
  expect_identical(as.data.frame(got), as.data.frame(dt)[c("v", "w")])
  data.table::set(got, j = "x", value = got$w * 2L) # selfref restored: usable in place
  expect_identical(got$x, dt$w * 2L) # setkeyv() sorted the rows: w is 2, 1, 3
})

test_that("only the selected parts are read", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.compress = 0)
  on.exit(options(old), add = TRUE)
  df <- data.frame(a = seq_len(50000L), b = seq_len(50000L) * 3L)
  write_rdz(df, path)
  # corrupt the first payload byte of column b (object 4: root, names, a, b)
  bytes <- readBin(path, "raw", file.size(path))
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  nobj <- sum(as.numeric(bytes[dir + 17:20]) * 256^(0:3))
  natt <- sum(as.numeric(bytes[dir + 21:24]) * 256^(0:3))
  names_of <- vapply(seq_len(nobj) - 1L, function(i) {
    e <- dir + dir_header_len(bytes, dir) + 48 * i
    sum(as.numeric(bytes[e + 11:12]) * 256^(0:1)) # type tag
  }, numeric(1L))
  b <- which(names_of == 2)[[2L]] - 1L # the second integer object
  first_block <- sum(as.numeric(bytes[dir + dir_header_len(bytes, dir) + 48 * b + 41:44]) * 256^(0:3))
  entry <- dir + dir_header_len(bytes, dir) + 48 * nobj + 32 * natt + 64 * first_block
  payload <- le_u64(bytes, entry + 16)
  bytes[payload + 1L] <- xor(bytes[payload + 1L], as.raw(0xff))
  writeBin(bytes, path)
  expect_error(read_rdz(path), class = "rdz_format_error")
  expect_identical(read_rdz(path, select = "a"), df["a"])
  expect_error(read_rdz(path, select = "b"), class = "rdz_format_error")
})

test_that("selections that name nothing are errors", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(data.frame(a = 1:2, b = 3:4), path)
  expect_error(read_rdz(path, select = "z"), "Unknown in `select`: z")
  expect_error(read_rdz(path, select = c("a", "a")), "must not repeat")
  expect_error(read_rdz(path, select = 3), "from 1 to 2")
  expect_error(read_rdz(path, select = 1.5), "whole numbers")
  expect_error(read_rdz(path, select = NA_character_), "NA")
  expect_error(read_rdz(path, select = TRUE), "character vector or a numeric vector")
  write_rdz(1:3, path)
  expect_error(read_rdz(path, select = 1), "list or a data frame")
  write_rdz(1:3, path, mode = "r")
  expect_error(read_rdz(path, select = 1), "list or a data frame")
})
