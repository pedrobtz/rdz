# Helpers of test-attributes.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

roundtrip_attrs <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "native")
  list(value = read_rdz(path), info = rdz_info(path))
}
