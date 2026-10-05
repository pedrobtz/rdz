# The Rust reference corpus: files written by the Rust implementation at the
# commit in manifest.tsv. Whatever implements rdz must read every one of them
# to the value its spec constructs and report the same rdz_info().

rust_fixture_manifest <- function() {
  utils::read.delim(
    file.path(rust_fixture_dir(), "manifest.tsv"),
    colClasses = "character", quote = ""
  )
}

test_that("the Rust reference corpus is complete and unmodified", {
  manifest <- rust_fixture_manifest()
  specs <- rust_fixture_specs()
  expect_identical(manifest$name, vapply(specs, `[[`, "", "name"))
  expect_identical(manifest$mode, vapply(specs, `[[`, "", "mode"))
  expect_identical(manifest$policy, vapply(specs, `[[`, "", "policy"))

  files <- file.path(rust_fixture_dir(), paste0(manifest$name, ".rdz"))
  # tools::sha256sum() arrived in R 4.5.0.
  if (exists("sha256sum", asNamespace("tools"))) {
    expect_identical(unname(tools::sha256sum(files)), manifest$sha256)
  }
  expect_identical(as.character(file.size(files)), manifest$bytes)

  # Every block encoding a writer emits is represented (1, the legacy two-bit
  # logical record, is read-only), and every dictionary policy is used.
  encodings <- unique(unlist(strsplit(manifest$encodings, " ", fixed = TRUE)))
  expect_setequal(encodings, as.character(c(0L, 2:9)))
  expect_setequal(manifest$policy, c("plain", "block", "global", "auto"))
  expect_setequal(manifest$codec, c("native_v1", "r_serial_v3"))
})

test_that("rdz_info() reports what the Rust build reported for every fixture", {
  infos <- readRDS(file.path(rust_fixture_dir(), "info.rds"))
  for (spec in rust_fixture_specs()) {
    path <- file.path(rust_fixture_dir(), paste0(spec$name, ".rdz"))
    info <- unclass(rdz_info(path))
    # the Rust reference recorded no writer, and its build had no such field
    expect_identical(info$writer, "", label = spec$name)
    info$writer <- NULL
    expect_identical(info, infos[[spec$name]], label = spec$name)
  }
})

test_that("the C reader reads every generic fixture to its spec's value", {
  for (spec in rust_fixture_specs()) {
    path <- file.path(rust_fixture_dir(), paste0(spec$name, ".rdz"))
    if (rdz_info(path)$codec != "r_serial_v3") next
    payload <- rdz:::rdz_check(.Call(rdz:::rdz_test_read_generic, path))
    expect_identical(unserialize(payload), spec$value(), label = spec$name)
  }
})

test_that("the C writer reproduces every generic fixture byte for byte", {
  dir <- tempfile("rdz-c-writer-")
  dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE), add = TRUE)
  for (spec in rust_fixture_specs()) {
    path <- file.path(rust_fixture_dir(), paste0(spec$name, ".rdz"))
    info <- rdz_info(path)
    if (info$codec != "r_serial_v3") next
    payload <- rdz:::rdz_check(.Call(rdz:::rdz_test_read_generic, path))
    synopsis <- rdz:::rdz_c_file_info(path)$synopsis
    copy <- file.path(dir, basename(path))
    rdz:::rdz_check(.Call(rdz:::rdz_test_write_generic, payload, synopsis, copy))
    expect_identical(
      bytes_but_writer(copy),
      bytes_but_writer(path),
      label = spec$name
    )
  }
})

test_that("every Rust fixture reads back to its spec's value", {
  manifest <- rust_fixture_manifest()
  for (spec in rust_fixture_specs()) {
    path <- file.path(rust_fixture_dir(), paste0(spec$name, ".rdz"))
    row <- manifest[manifest$name == spec$name, ]

    expect_identical(read_rdz(path), spec$value(), label = spec$name)

    blocks <- rdz_block_encodings(path)
    expect_identical(
      paste(sort(unique(blocks$encoding)), collapse = " "),
      row$encodings,
      label = spec$name
    )
    expect_true(all(blocks$compression == 0), label = spec$name)
  }
})
