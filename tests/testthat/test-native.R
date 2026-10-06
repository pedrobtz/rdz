# The native logical codec in C (plan-c Stage E): the files it writes are the
# Rust implementation's, byte for byte, whichever classifier kernel runs --
# with blocks stored raw (the "speed" preset), as the Rust writer stored them.

with_dictionary_policy <- function(policy, code) {
  old <- Sys.getenv("RDZ_STRING_DICT", unset = NA)
  Sys.setenv(RDZ_STRING_DICT = policy)
  on.exit(if (is.na(old)) Sys.unsetenv("RDZ_STRING_DICT") else Sys.setenv(RDZ_STRING_DICT = old))
  force(code)
}

native_specs <- function() {
  manifest <- utils::read.delim(
    file.path(rust_fixture_dir(), "manifest.tsv"),
    colClasses = "character", quote = ""
  )
  Filter(function(spec) manifest$codec[manifest$name == spec$name] == "native_v1",
         rust_fixture_specs())
}

test_that("every native fixture is reproduced byte for byte, with every kernel", {
  old <- options(rdz.preset = "speed")
  on.exit(options(old), add = TRUE)
  copy <- tempfile(fileext = ".rdz")
  on.exit({
    unlink(copy)
    .Call(rdz:::rdz_c_logical_kernel, FALSE)
  }, add = TRUE)
  specs <- native_specs()
  expect_length(specs, 21L)
  for (force_scalar in c(FALSE, TRUE)) {
    kernel <- .Call(rdz:::rdz_c_logical_kernel, force_scalar)
    for (spec in specs) {
      fixture <- file.path(rust_fixture_dir(), paste0(spec$name, ".rdz"))
      with_dictionary_policy(spec$policy, write_rdz(spec$value(), copy, mode = "native"))
      expect_identical(
        bytes_without_hash(copy),
        bytes_without_hash(fixture),
        label = paste(spec$name, kernel)
      )
    }
  }
})

test_that("the classifier rejects an impossible logical value the same way", {
  # Not constructible from R: checked through the codec directly in
  # tools/c-tests. Here, every kernel agrees on a long mixed vector.
  x <- rep_len(c(TRUE, NA, FALSE, FALSE, TRUE, NA, NA), 200003L)
  paths <- c(tempfile(), tempfile())
  on.exit({
    unlink(paths)
    .Call(rdz:::rdz_c_logical_kernel, FALSE)
  }, add = TRUE)
  .Call(rdz:::rdz_c_logical_kernel, FALSE)
  write_rdz(x, paths[[1L]], mode = "native")
  .Call(rdz:::rdz_c_logical_kernel, TRUE)
  write_rdz(x, paths[[2L]], mode = "native")
  expect_identical(readBin(paths[[1L]], "raw", 1e7), readBin(paths[[2L]], "raw", 1e7))
  expect_identical(read_rdz(paths[[1L]]), x)
})

test_that("strict native mode raises rdz_unsupported_error and leaves no file", {
  path <- tempfile(fileext = ".rdz")
  expect_error(write_rdz(1i, path, mode = "native"), class = "rdz_unsupported_error")
  expect_error(
    write_rdz(structure(TRUE, note = globalenv()), path, mode = "native"),
    "environment",
    class = "rdz_unsupported_error"
  )
  expect_false(file.exists(path))
})
