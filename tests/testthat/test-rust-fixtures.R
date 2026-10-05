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
  expect_identical(unname(tools::sha256sum(files)), manifest$sha256)
  expect_identical(as.character(file.size(files)), manifest$bytes)

  # Every block encoding a writer emits is represented (1, the legacy two-bit
  # logical record, is read-only), and every dictionary policy is used.
  encodings <- unique(unlist(strsplit(manifest$encodings, " ", fixed = TRUE)))
  expect_setequal(encodings, as.character(c(0L, 2:9)))
  expect_setequal(manifest$policy, c("plain", "block", "global", "auto"))
  expect_setequal(manifest$codec, c("native_v1", "r_serial_v3"))
})

test_that("every Rust fixture reads back to its spec's value", {
  manifest <- rust_fixture_manifest()
  infos <- readRDS(file.path(rust_fixture_dir(), "info.rds"))
  for (spec in rust_fixture_specs()) {
    path <- file.path(rust_fixture_dir(), paste0(spec$name, ".rdz"))
    row <- manifest[manifest$name == spec$name, ]

    expect_identical(read_rdz(path), spec$value(), label = spec$name)
    expect_identical(unclass(rdz_info(path)), infos[[spec$name]], label = spec$name)

    blocks <- rdz_block_encodings(path)
    expect_identical(
      paste(sort(unique(blocks$encoding)), collapse = " "),
      row$encodings,
      label = spec$name
    )
    expect_true(all(blocks$compression == 0), label = spec$name)
  }
})
