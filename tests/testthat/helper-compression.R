# Helpers of test-compression.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

with_rdz_options <- function(code, ...) {
  old <- options(...)
  on.exit(options(old), add = TRUE)
  force(code)
}
write_with <- function(x, ...) {
  path <- tempfile(fileext = ".rdz")
  with_rdz_options(write_rdz(x, path, mode = "r"), ...)
  path
}
file_bytes <- function(path) readBin(path, "raw", file.size(path))
