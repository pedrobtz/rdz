# Helpers of test-c-container.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

fixture_bytes <- function(name) {
  path <- file.path(rust_fixture_dir(), paste0(name, ".rdz"))
  readBin(path, "raw", file.size(path))
}
corrupt_copy <- function(bytes, at, value = NULL) {
  if (is.null(value)) {
    value <- as.raw(bitwXor(as.integer(bytes[[at]]), 0xffL))
  }
  bytes[at] <- value
  path <- tempfile(fileext = ".rdz")
  writeBin(bytes, path)
  path
}
