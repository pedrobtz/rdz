# rdz_schema() reads the directory, the attribute names and the small
# objects its tree shows in one call: one open of the file.

test_that("rdz_schema() and rdz_info() open the file once", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  v <- seq_len(5000) + 0.5
  x <- list(a = data.frame(d = 1:3, f = factor(c("p", "q", "p"))), b = list(m = matrix(1:4, 2)),
            s = v, t = v)
  write_rdz(x, path, mode = "native")
  expect_identical(opens_during(rdz_schema(path)), 1)
  expect_identical(opens_during(rdz_schema(path, recursive = FALSE)), 1)
  expect_identical(opens_during(rdz_info(path)), 1)
  expect_true(any(!is.na(rdz_schema(path)$objects$shared_with)))
  write_rdz(x, path, mode = "r")
  expect_identical(opens_during(rdz_schema(path)), 1)
})
