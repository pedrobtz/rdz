# read_rdz(rows =): a data frame's rows or a vector's elements. Natively,
# only the blocks covering range(rows) are read; the result is what `[`
# gives on the whole value, with automatic row names kept automatic.

rows_ref <- function(df, rows) {
  ref <- df[rows, , drop = FALSE]
  if (.row_names_info(df) < 0L) rownames(ref) <- NULL
  ref
}

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
  for (preset in c("speed", "balanced")) {
    old <- options(rdz.preset = preset)
    write_rdz(df, path)
    options(old)
    for (name in names(sets)) {
      expect_identical(read_rdz(path, rows = sets[[name]]), rows_ref(df, sets[[name]]),
                       label = paste(preset, name))
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
  old <- options(rdz.preset = "speed")
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
