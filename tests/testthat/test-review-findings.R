skip_without_rust()

# Regression tests for the defects recorded in review.md.

# review.md finding 2 ---------------------------------------------------------

test_that("the synopsis never shortens a name without setting `truncated`", {
  # Regression coverage for the former mismatch between the truncation
  # operation and the independently calculated `truncated` flag.
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  class_name <- strrep("c", 100L)
  attribute_name <- paste0("attribute_", strrep("b", 60L))
  x <- structure(1L, class = class_name)
  attr(x, attribute_name) <- 2L

  write_rdz(x, path)
  synopsis <- rdz_info(path)$synopsis

  # Both names are pure ASCII and well inside the documented 256-byte bound, so
  # they should be reported exactly.
  expect_identical(synopsis$class, class_name)
  expect_true(attribute_name %in% synopsis$attribute_names)

  # And whichever bound the writer settles on, shortening a name must set the
  # flag. This assertion holds for either fix: widen the bound, or report it.
  expect_true(identical(synopsis$class, class_name) || isTRUE(synopsis$truncated))
  expect_true(
    attribute_name %in% synopsis$attribute_names || isTRUE(synopsis$truncated)
  )
})

test_that("a name past the synopsis bound is still reported as truncated", {
  # Positive control for the test above: the flag must keep working once the
  # bound is fixed.
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  x <- structure(1L, class = strrep("c", 1000L))

  write_rdz(x, path)
  synopsis <- rdz_info(path)$synopsis

  expect_true(synopsis$truncated)
  expect_lt(nchar(synopsis$class), 1000L)
})

# review.md finding 6 ---------------------------------------------------------

test_that("the synopsis always reports `synopsis_error`", {
  # The field is a stable part of the synopsis schema on both success and
  # failure paths.
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(list(value = 1L), path)

  expect_identical(rdz_info(path)$synopsis$synopsis_error, FALSE)
})

# review.md finding 8 ---------------------------------------------------------

test_that("replacing an rdz file preserves its permissions", {
  # Replacement must retain the existing destination's mode.
  skip_on_os("windows")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(1L, path)
  default_mode <- file.mode(path)
  Sys.chmod(path, "0600")
  skip_if(
    file.mode(path) == default_mode,
    "the process umask already produces mode 0600"
  )

  write_rdz(2L, path)

  expect_identical(file.mode(path), as.octmode("600"))
})

# review.md finding 1 ---------------------------------------------------------

test_that(".Rbuildignore excludes stray build artifacts and agent guidance", {
  build_ignore <- test_path("..", "..", ".Rbuildignore")
  skip_if_not(
    file.exists(build_ignore),
    "`.Rbuildignore` is not available in the installed package"
  )

  patterns <- readLines(build_ignore)
  patterns <- patterns[nzchar(trimws(patterns))]
  # `R CMD build` matches each pattern against package-relative paths and prunes
  # matching directories, so a directory path is the right unit to assert on.
  is_ignored <- function(path) {
    any(vapply(
      patterns,
      function(pattern) grepl(pattern, path, perl = TRUE, ignore.case = TRUE),
      logical(1L)
    ))
  }

  expect_true(is_ignored("src/rust/target"))
  # macOS duplicates the directory rather than the file when a name collides.
  # `^src/rust/target$` is anchored and does not match, so 62 MB of Cargo
  # artifacts land in the source tarball.
  expect_true(is_ignored("src/rust/target 2"))
  expect_true(is_ignored("src/rust/vendor"))
  # CLAUDE.md is a symlink to the ignored AGENTS.md, and `tar` follows it.
  expect_true(is_ignored("CLAUDE.md"))
})
