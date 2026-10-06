# Helpers of test-lists.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

roundtrip_list <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "native")
  expect_identical(rdz_info(path)$codec, "native_v1")
  read_rdz(path)
}
