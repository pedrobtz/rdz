# Helpers of test-c-generic.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

roundtrip_generic <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "r")
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  read_rdz(path)
}
leftover_temporaries <- function(dir) {
  list.files(dir, pattern = "-rdz-.*\\.(tmp|backup)$", all.files = TRUE)
}
