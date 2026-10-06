# read_rdz(rows =): a data frame's rows or a vector's elements. Natively,
# only the blocks covering range(rows) are read; the result is what `[`
# gives on the whole value, with automatic row names kept automatic.

rows_frame <- function(n) {
  i <- seq_len(n)
  data.frame(
    l = c(TRUE, FALSE, NA)[i %% 3L + 1L],
    int = i * 3L,
    dbl = round(i / 7, 2), # decimal (ALP) blocks when compressing
    full = i / 3, # full precision
    chr = c("alpha", "beta", NA, "gamma")[i %% 4L + 1L], # a dictionary
    uniq = sprintf("u%07d", i), # plain strings
    fct = factor(c("x", "y", "z"))[i %% 3L + 1L],
    day = as.Date("2020-01-01") + i %% 1000L,
    stringsAsFactors = FALSE
  )
}

test_that("rows across every type's block boundaries match `[`", {
  skip_on_cran() # 600,000 rows
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- rows_frame(600000L)
  sets <- list(
    first = 1:3, last = 599990:600000, logical_boundary = 65530:65545,
    double_boundary = 131065:131080, integer_boundary = 262140:262150,
    dictionary_chunk = 65536:65537, scattered = c(500000L, 7L, 300000L, 7L),
    one = 424242L, none = integer(), wide = seq(1L, 600000L, by = 997L)
  )
  for (level in c(0L, 1L)) {
    old <- options(rdz.compress = level)
    write_rdz(df, path)
    options(old)
    for (name in names(sets)) {
      expect_identical(read_rdz(path, rows = sets[[name]]), rows_ref(df, sets[[name]]),
                       label = paste(level, name))
    }
  }
  expect_identical(read_rdz(path, rows = 262140:262150, select = c("fct", "int")),
                   rows_ref(df[c("fct", "int")], 262140:262150))
})

test_that("vectors keep names and class through `[`", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  v <- stats::setNames(seq_len(100000L) * 2L, sprintf("n%06d", seq_len(100000L)))
  write_rdz(v, path)
  expect_identical(read_rdz(path, rows = c(99999L, 3L, 70000L)), v[c(99999L, 3L, 70000L)])
  d <- as.Date("2026-01-01") + 0:999
  write_rdz(d, path)
  expect_identical(read_rdz(path, rows = 10:20), d[10:20])
  f <- factor(sample(letters, 5000L, replace = TRUE), levels = letters)
  write_rdz(f, path)
  expect_identical(read_rdz(path, rows = 4000:4010), f[4000:4010])
  s <- sprintf("s%05d", 1:70000)
  write_rdz(s, path)
  expect_identical(read_rdz(path, rows = 65530:65540), s[65530:65540])
})

test_that("row names, classes and other columns follow the rows", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(a = 1:5, b = letters[1:5], row.names = paste0("r", 1:5))
  write_rdz(df, path)
  expect_identical(read_rdz(path, rows = c(4L, 2L)), df[c(4L, 2L), ])
  lst <- data.frame(a = 1:4)
  lst$l <- list(1, "x", NULL, 2:3) # a list column: read whole, then subset
  write_rdz(lst, path)
  expect_identical(read_rdz(path, rows = 2:3), rows_ref(lst, 2:3))
  shared <- data.frame(a = seq_len(3000L))
  shared$b <- shared$a # a shared column, windowed once
  write_rdz(shared, path)
  expect_identical(read_rdz(path, rows = 2990:3000), rows_ref(shared, 2990:3000))
  skip_if_not_installed("tibble")
  tb <- tibble::tibble(x = 1:10, y = letters[1:10])
  write_rdz(tb, path)
  expect_identical(read_rdz(path, rows = 3:4), tb[3:4, ])
  skip_if_not_installed("data.table")
  dt <- data.table::data.table(x = 1:10, y = letters[1:10])
  write_rdz(dt, path)
  got <- read_rdz(path, rows = 5:6)
  expect_s3_class(got, "data.table")
  expect_identical(as.data.frame(got), as.data.frame(dt)[5:6, , drop = FALSE] |> `rownames<-`(NULL))
  expect_no_warning(data.table::set(got, j = "z", value = 0L))
})

test_that("only the blocks covering the rows are read", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.compress = 0)
  on.exit(options(old), add = TRUE)
  x <- seq_len(600000L) # three integer blocks
  write_rdz(x, path)
  bytes <- readBin(path, "raw", file.size(path))
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  hl <- dir_header_len(bytes, dir)
  entries <- dir + hl + 48 # one object
  last_payload <- le_u64(bytes, entries + 64 * 2 + 16) # the third block
  bytes[last_payload + 1L] <- xor(bytes[last_payload + 1L], as.raw(0xff))
  writeBin(bytes, path)
  expect_error(read_rdz(path), class = "rdz_format_error")
  expect_identical(read_rdz(path, rows = 1:10), 1:10)
  expect_identical(read_rdz(path, rows = 300000:300005), 300000:300005)
  expect_error(read_rdz(path, rows = 599999L), class = "rdz_format_error")
})

test_that("rows from raw vectors and generic files match too; bad rows are errors", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(a = 1:6, b = c(1.5, 2, NA, 4, 5, 6))
  expect_identical(read_rdz(rdz_serialize(df), rows = 2:3), rows_ref(df, 2:3))
  write_rdz(df, path, mode = "r")
  expect_identical(read_rdz(path, rows = c(6L, 1L)), rows_ref(df, c(6L, 1L)))
  write_rdz(df, path)
  expect_error(read_rdz(path, rows = 7L), "at most 6")
  expect_error(read_rdz(path, rows = 0L), "positive whole")
  expect_error(read_rdz(path, rows = 1.5), "positive whole")
  expect_error(read_rdz(path, rows = NA_integer_), "positive whole")
  write_rdz(list(1, 2), path)
  expect_error(read_rdz(path, rows = 1L), "use `select`")
})

test_that("generic files take the rows asked for, not ones shifted by the window", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(id = 1:6, x = c(1.5, 2, NA, 4, 5, 6))
  write_rdz(df, path, mode = "r")
  for (rows in list(2:3, 4:6, c(6L, 2L), 6L)) {
    expect_identical(read_rdz(path, rows = rows), rows_ref(df, rows), label = deparse(rows))
  }
  expect_error(read_rdz(path, rows = 7L), "at most 6")
  v <- c(10, 20, 30, 40, 50, 60)
  write_rdz(v, path, mode = "r")
  expect_identical(read_rdz(path, rows = 4:6), v[4:6])
  expect_error(read_rdz(path, rows = 7L), "at most 6")
  # an automatic fallback (a complex column) is a generic file too
  cx <- data.frame(id = 1:4, z = complex(real = 1:4, imaginary = -1))
  write_rdz(cx, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path, rows = 3:4), rows_ref(cx, 3:4))
})

test_that("matrix and data frame columns give their rows, as `[` does", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(id = 1:5)
  df$m <- matrix(1:10, 5)
  write_rdz(df, path)
  got <- read_rdz(path, rows = 2:3)
  expect_identical(got, rows_ref(df, 2:3))
  expect_identical(got$m, matrix(c(2L, 3L, 7L, 8L), 2))
  df$s <- data.frame(b = 11:15, c = letters[1:5])
  write_rdz(df, path, mode = "r")
  expect_identical(read_rdz(path, rows = c(5L, 1L)), rows_ref(df, c(5L, 1L)))
  # a nested frame with as many columns as rows: its rows, not its columns
  sq <- data.frame(id = 1:2)
  sq$s <- data.frame(p = c("a", "b"), q = c("c", "d"))
  write_rdz(sq, path, mode = "r")
  expect_identical(read_rdz(path, rows = 2L), rows_ref(sq, 2L))
  # an array of three dimensions: `[.data.frame` takes its elements
  ar <- data.frame(id = 1:4)
  ar$a <- array(1:24, c(4, 3, 2))
  write_rdz(ar, path)
  expect_identical(read_rdz(path, rows = 3:4), rows_ref(ar, 3:4))
})

test_that("a vector shared with a list column is not cut short by the window", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  v <- seq_len(3000L) + 0.5 # large enough to be written once and shared
  df <- data.frame(a = v)
  df$l <- I(rep(list(v), 3000L))
  write_rdz(df, path)
  expect_identical(rdz_info(path)$codec, "native_v1")
  got <- read_rdz(path, rows = 2:3)
  expect_identical(length(got$l[[1L]]), 3000L)
  expect_identical(got, rows_ref(df, 2:3))
  # the shared vector first met inside the list column
  df <- df[c("l", "a")]
  write_rdz(df, path)
  expect_identical(read_rdz(path, rows = 5:6), rows_ref(df, 5:6))
})

test_that("matrices, time series and one-column matrix columns give their rows", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  m <- matrix(1:20, 10)
  write_rdz(m, path, mode = "native")
  expect_identical(read_rdz(path, rows = 3:4), m[3:4])
  t <- ts(1:10, start = 2000)
  write_rdz(t, path, mode = "native")
  expect_identical(read_rdz(path, rows = 3:4), t[3:4])
  df <- data.frame(id = 1:10)
  df$m <- matrix(1:10, 10, dimnames = list(letters[1:10], "x"))
  write_rdz(df, path, mode = "native")
  expect_identical(read_rdz(path, rows = 9:10), rows_ref(df, 9:10))
})
