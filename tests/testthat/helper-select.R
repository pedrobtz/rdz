# Helpers of test-select.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

select_both <- function(x, select) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "native")
  native <- read_rdz(path, select = select)
  write_rdz(x, path, mode = "r")
  generic <- read_rdz(path, select = select)
  expect_identical(native, generic)
  native
}
