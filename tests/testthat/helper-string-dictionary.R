# Helpers of test-string-dictionary.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

with_string_dictionary <- function(policy, code) {
  previous <- Sys.getenv("RDZ_STRING_DICT", unset = NA)
  Sys.setenv(RDZ_STRING_DICT = policy)
  on.exit(
    if (is.na(previous)) Sys.unsetenv("RDZ_STRING_DICT") else Sys.setenv(RDZ_STRING_DICT = previous),
    add = TRUE
  )
  force(code)
}
dictionary_policies <- c("plain", "block", "global", "auto")
mixed_names <- function(length, distinct) {
  latin1 <- iconv("\u00c4rende", from = "UTF-8", to = "latin1")
  bytes <- rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9)))
  Encoding(bytes) <- "bytes"
  pool <- c(
    sprintf("name-%04d", seq_len(max(1L, distinct - 4L))),
    enc2utf8("\u65e5\u672c"), latin1, bytes, "", NA_character_
  )
  pool[(seq_len(length) * 7L) %% length(pool) + 1L]
}
