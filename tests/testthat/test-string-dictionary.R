skip_without_rust()

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

test_that("names round-trip under every dictionary policy at chunk boundaries", {
  chunk <- 64L * 1024L
  for (policy in dictionary_policies) {
    for (length in c(0L, 1L, chunk - 1L, chunk, chunk + 1L, 3L * chunk + 7L)) {
      x <- rep(c(TRUE, FALSE, NA), length.out = length)
      names(x) <- mixed_names(length, 20L)
      path <- tempfile(fileext = ".rdz")
      on.exit(unlink(path), add = TRUE)

      with_string_dictionary(policy, write_rdz(x, path, mode = "native"))
      label <- paste(policy, length)
      expect_identical(rdz_info(path)$codec, "native_v1", info = label)
      expect_identical(read_rdz(path), x, info = label)
      expect_identical(rdz_attributes(path, names = "names")$names, names(x), info = label)
    }
  }
})

test_that("reading does not depend on the writer's dictionary policy", {
  x <- rep(TRUE, 200000L)
  names(x) <- sprintf("id-%06d", sample.int(50000L, length(x), replace = TRUE))
  paths <- vapply(dictionary_policies, function(policy) {
    path <- tempfile(fileext = ".rdz")
    with_string_dictionary(policy, write_rdz(x, path, mode = "native"))
    path
  }, character(1L))
  on.exit(unlink(paths), add = TRUE)

  for (path in paths) {
    expect_identical(read_rdz(path), x)
  }
  expect_lt(file.size(paths[["global"]]), file.size(paths[["plain"]]) / 2)
})

test_that("an unsupported name found mid-write falls back without leaving files", {
  directory <- tempfile("rdz-dictionary-")
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  path <- file.path(directory, "object.rdz")

  native_non_ascii <- rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9)))
  x <- rep(TRUE, 3L * 64L * 1024L)
  names(x) <- c(sprintf("n%02d", seq_len(length(x) - 1L) %% 50L), native_non_ascii)

  for (policy in dictionary_policies) {
    with_string_dictionary(policy, {
      expect_error(write_rdz(x, path, mode = "native"), "non-ASCII native-encoded names")
      expect_identical(list.files(directory, all.files = TRUE, no.. = TRUE), character())

      write_rdz(x, path)
    })
    expect_identical(rdz_info(path)$codec, "r_serial_v3", info = policy)
    expect_identical(read_rdz(path), x, info = policy)
    expect_identical(list.files(directory, all.files = TRUE, no.. = TRUE), "object.rdz")
    unlink(path)
  }
})
