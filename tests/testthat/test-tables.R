# data.tables and tibbles come back as they were written: a data.table's
# .internal.selfref, never stored, is rebuilt by data.table::setalloccol()
# wherever the table is (root, list element, column of a list), and only when
# the file holds one; a tibble needs nothing rebuilt.

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
      expect_true(data.table:::selfrefok(t) == 1L, label = mode)
    }
    expect_no_warning(data.table::set(y$inner$deeper, j = "c", value = 1L))
    expect_identical(as.data.frame(y$dt), as.data.frame(dt), label = mode)
  }
  # a selection and a read below the root rebuild them too
  write_rdz(x, path)
  expect_true(data.table:::selfrefok(read_rdz(path, select = "inner")$inner$deeper) == 1L)
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
  out <- system2(file.path(R.home("bin"), "Rscript"),
                 c("--vanilla", "-e", shQuote(sprintf(
                   'invisible(rdz::read_rdz("%s")); cat("data.table" %%in%% loadedNamespaces())',
                   path))),
                 stdout = TRUE,
                 env = paste0("R_LIBS=", paste(.libPaths(), collapse = .Platform$path.sep)))
  expect_identical(utils::tail(out, 1L), "FALSE")
})
