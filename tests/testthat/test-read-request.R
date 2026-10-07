# read_rdz() resolves `select` and `rows` against the file in one call
# (issue #27, F.1): one open of the file, whatever it reads and wherever it
# falls back to reading the value whole.

test_that("read_rdz() opens the file once for select, rows, both and a refused window", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(a = 1:10, b = letters[1:10], c = as.numeric(1:10))
  write_rdz(df, path)
  expect_identical(opens_during(read_rdz(path, select = c("c", "a"))), 1)
  expect_identical(opens_during(read_rdz(path, rows = 2:4)), 1)
  expect_identical(opens_during(read_rdz(path, select = "b", rows = 2:4)), 1)

  # a window the reader refuses (a column shared with an attribute) falls
  # back to the whole value without a second open
  v <- as.numeric(1:600)
  shared <- data.frame(a = v, b = 1:600)
  attr(shared, "x") <- v
  write_rdz(shared, path)
  got <- NULL
  expect_identical(opens_during(got <- read_rdz(path, select = "a", rows = 2:3)), 1)
  expect_identical(got$a, v[2:3])

  generic <- tempfile(fileext = ".rdz")
  on.exit(unlink(generic), add = TRUE)
  write_rdz(df, generic, mode = "r")
  expect_identical(opens_during(read_rdz(generic, select = "b", rows = 2:3)), 1)
})

test_that("select and rows together on a frame with a shared column", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  v <- as.numeric(1:600) # 4,800 bytes: shared
  df <- data.frame(a = v, n = 600:1, b = v)
  write_rdz(df, path)
  got <- read_rdz(path, select = c("b", "n"), rows = c(5, 2, 9))
  ref <- df[c(5, 2, 9), c("b", "n")]
  rownames(ref) <- NULL
  expect_identical(got, ref)

  # the shared vector is also an attribute: the window is refused and the
  # value is read whole, then the rows taken in R
  attr(df, "x") <- v
  write_rdz(df, path)
  got <- read_rdz(path, select = c("b", "n"), rows = 3:4)
  expect_identical(got$b, v[3:4])
  expect_identical(got$n, 598:597)
})

test_that("select and rows together on a generic file", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(a = 1:6, b = letters[1:6], c = 6:1)
  write_rdz(df, path, mode = "r")
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  got <- read_rdz(path, select = c("c", "a"), rows = c(2, 5))
  ref <- df[c(2, 5), c("c", "a")]
  rownames(ref) <- NULL
  expect_identical(got, ref)
})

test_that("select by name works when the names are shared with another attribute", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  l <- as.list(seq_len(2000))
  names(l) <- sprintf("k%04d", seq_len(2000))
  attr(l, "keys") <- names(l) # stored as a reference to the names
  write_rdz(l, path, mode = "native")
  expect_identical(read_rdz(path, select = c("k0007", "k1999")), list(k0007 = 7L, k1999 = 1999L))
})

test_that("the file's refusals of select and rows keep R's words and class", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(data.frame(a = 1:3, b = 4:6), path)
  expect_error(read_rdz(path, select = c("z", "a", "y")), "^Unknown in `select`: z, y$",
               class = "rdz_argument_error")
  expect_error(read_rdz(path, select = 3), "^`select` positions must be whole numbers from 1 to 2\\.$",
               class = "rdz_argument_error")
  expect_error(read_rdz(path, select = TRUE),
               "^`select` must be NULL, a character vector or a numeric vector\\.$",
               class = "rdz_argument_error")
  expect_error(read_rdz(path, rows = 4), "^`rows` must be at most 3\\.$",
               class = "rdz_argument_error")
  write_rdz(1:5, path)
  expect_error(read_rdz(path, select = 1), "^`select` needs a list or a data frame; the file holds integer\\.$",
               class = "rdz_argument_error")
  write_rdz(list(a = 1, b = "x"), path, mode = "native")
  expect_error(read_rdz(path, rows = 1),
               "^`rows` needs a data frame or a vector; use `select` for a list's elements\\.$",
               class = "rdz_argument_error")
  write_rdz(list(1, 2), path, mode = "native")
  expect_error(read_rdz(path, select = "a"), "^`select` names parts, but they have no names\\.$",
               class = "rdz_argument_error")
})
