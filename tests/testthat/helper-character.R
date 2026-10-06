# Helpers of test-character.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

roundtrip_native <- function(x, policy = NULL, ...) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  if (!is.null(policy)) {
    old_env <- Sys.getenv("RDZ_STRING_DICT", unset = NA)
    Sys.setenv(RDZ_STRING_DICT = policy)
    on.exit(if (is.na(old_env)) Sys.unsetenv("RDZ_STRING_DICT") else
      Sys.setenv(RDZ_STRING_DICT = old_env), add = TRUE)
  }
  old <- options(...)
  on.exit(options(old), add = TRUE)
  write_rdz(x, path, mode = "native")
  expect_identical(rdz_info(path)$codec, "native_v1")
  read_rdz(path)
}
encoded <- function() {
  latin1 <- "caf\xe9"
  Encoding(latin1) <- "latin1"
  bytes <- rawToChar(as.raw(c(0xff, 0x41, 0xfe)))
  Encoding(bytes) <- "bytes"
  utf8 <- "été ☃"
  Encoding(utf8) <- "UTF-8"
  c("plain", NA, "", utf8, latin1, bytes)
}
