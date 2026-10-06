# data.tables and tibbles come back as they were written. A data.table's
# .internal.selfref, never stored, comes back as R_Unserialize() gives it (a
# NULL pointer): data.table takes the table as loaded from disk and rebuilds
# it at the first change, wherever it is, as after readRDS(); rdz never calls
# or loads data.table. A tibble needs nothing rebuilt.

test_that("nested data.tables are usable in place after a read, native or generic", {
  skip_if_not_installed("data.table")
  dt <- data.table::data.table(a = 1:3, b = c("x", "y", "z"))
  data.table::setkeyv(dt, "b")
  x <- list(dt = dt, inner = list(deeper = data.table::copy(dt)), n = 1)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  for (mode in c("native", "r")) {
    write_rdz(x, path, mode = mode)
    y <- read_rdz(path)
    for (t in list(y$dt, y$inner$deeper)) {
      expect_s3_class(t, "data.table")
      expect_identical(data.table::key(t), "b", label = mode)
      expect_identical(data.table:::selfrefok(t), -1L, label = mode) # as from disk
    }
    # changed in place at once: no shallow copy, no warning, the change kept
    expect_no_warning(data.table::set(y$inner$deeper, j = "c", value = 1L))
    expect_identical(y$inner$deeper$c, rep(1L, 3L), label = mode)
    expect_no_warning(data.table::set(y$dt, j = "c", value = 2L))
    expect_identical(y$dt$c, rep(2L, 3L), label = mode)
    expect_identical(as.data.frame(y$dt)[c("a", "b")], as.data.frame(dt), label = mode)
  }
  # a selection gives the same, native or generic, at the root too
  for (mode in c("native", "r")) {
    write_rdz(dt, path, mode = mode)
    s <- read_rdz(path, select = "a")
    expect_identical(data.table:::selfrefok(s), -1L, label = mode)
    expect_no_warning(data.table::set(s, j = "z", value = 0L))
    expect_identical(s$z, rep(0L, 3L), label = mode)
  }
})

test_that("tibbles come back identical, nested or not", {
  skip_if_not_installed("tibble")
  tb <- tibble::tibble(a = 1:2, g = list(1, "x"))
  x <- list(tb = tb, inner = list(tb2 = tb))
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path)
  expect_identical(rdz_info(path)$codec, "native_v1")
  expect_identical(read_rdz(path), x)
  expect_s3_class(read_rdz(path)$inner$tb2, "tbl_df")
})

test_that("reading a file without a data.table does not load data.table", {
  skip_on_cran()
  skip_if_not_installed("data.table")
  skip_if(length(find.package("rdz", lib.loc = .libPaths(), quiet = TRUE)) == 0L,
          "rdz is not installed")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(list(df = data.frame(a = 1:3), b = "x"), path)
  # a script file, not -e: no shell quoting to differ between systems
  script <- tempfile(fileext = ".R")
  on.exit(unlink(script), add = TRUE)
  # the child finds rdz through .libPaths(), set in the script: system2()'s
  # env = works only on Unix
  writeLines(c(
    sprintf(".libPaths(%s)", paste(deparse(.libPaths()), collapse = "")),
    sprintf("invisible(rdz::read_rdz(%s))", deparse(normalizePath(path, winslash = "/"))),
    'cat("data.table" %in% loadedNamespaces())'
  ), script)
  out <- system2(file.path(R.home("bin"), "Rscript"), c("--vanilla", shQuote(script)),
                 stdout = TRUE)
  expect_identical(utils::tail(out, 1L), "FALSE")
})

test_that("compact row names keep their sign", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  y <- head(data.frame(a = 1:10, b = letters[1:10]), 3) # row names c(NA, +3)
  expect_identical(.row_names_info(y, 0L), c(NA, 3L))
  write_rdz(y, path, mode = "native")
  got <- read_rdz(path)
  expect_identical(.row_names_info(got, 0L), c(NA, 3L))
  expect_identical(rownames(as.matrix(got)), c("1", "2", "3"))
  expect_identical(rdz_info(path)$content_hash, rdz_hash(y))
  expect_identical(read_rdz(path, rows = 2:3), y[2:3, ])
  auto <- data.frame(a = 1:3)
  write_rdz(auto, path)
  expect_identical(.row_names_info(read_rdz(path), 0L), c(NA, -3L))
})
