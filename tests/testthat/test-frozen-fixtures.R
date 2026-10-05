# The frozen 0.1.0 corpus (helper-frozen-fixtures.R): every later rdz reads
# each file to its spec's value, and writes a native `speed` file's bytes
# again (but the writer field). Compressed files are compared by value only:
# zstd's output may change between its versions (container-format.md). So
# are generic files: R's serialization header records the R version that
# wrote the stream.

frozen_dir <- function() testthat::test_path("fixtures", "v0.1.0")

frozen_manifest <- function() {
  utils::read.delim(file.path(frozen_dir(), "manifest.tsv"), colClasses = "character",
                    quote = "")
}

test_that("the frozen corpus is complete and unmodified", {
  manifest <- frozen_manifest()
  specs <- frozen_fixture_specs()
  expect_identical(manifest$name, vapply(specs, `[[`, "", "name"))
  files <- file.path(frozen_dir(), paste0(manifest$name, ".rdz"))
  expect_identical(as.character(file.size(files)), manifest$bytes)
  if (exists("sha256sum", asNamespace("tools"))) { # R 4.5.0
    expect_identical(unname(tools::sha256sum(files)), manifest$sha256)
  }
  # every block encoding and both compressions are represented
  encodings <- unique(unlist(strsplit(manifest$encodings, " ", fixed = TRUE)))
  expect_setequal(encodings, as.character(c(0L, 2:14, 20:23)))
  compressions <- unique(unlist(strsplit(manifest$compressions, " ", fixed = TRUE)))
  expect_setequal(compressions, c("0", "1"))
})

test_that("every frozen file reads to its spec's value", {
  manifest <- frozen_manifest()
  for (spec in frozen_fixture_specs()) {
    path <- file.path(frozen_dir(), paste0(spec$name, ".rdz"))
    info <- rdz_info(path)
    expect_identical(info$codec, spec$codec, label = spec$name)
    expect_identical(info$writer, manifest$writer[manifest$name == spec$name], label = spec$name)
    expect_identical(read_rdz(path), spec$value(), label = spec$name)
  }
})

test_that("this rdz writes every frozen native speed file's bytes again", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  for (spec in frozen_fixture_specs()) {
    if (!identical(spec$preset, "speed") || !identical(spec$codec, "native_v1")) next
    write_frozen_fixture(spec, path)
    expect_identical(bytes_but_writer(path),
                     bytes_but_writer(file.path(frozen_dir(), paste0(spec$name, ".rdz"))),
                     label = spec$name)
  }
})
