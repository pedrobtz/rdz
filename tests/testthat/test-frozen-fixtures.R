# The frozen 0.1.0 corpus (helper-frozen-fixtures.R): every later rdz reads
# each file to its spec's value, and writes a native `speed` file's bytes
# again (but the writer field). Compressed files are compared by value only:
# zstd's output may change between its versions (container-format.md). So
# are generic files: R's serialization header records the R version that
# wrote the stream.

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
    if (!is.null(spec$metadata)) {
      expect_identical(rdz_info(path)$metadata, enc2utf8(spec$metadata), label = spec$name)
    }
  }
})

test_that("every frozen value hashes as its file records, on every platform", {
  # the content hash is defined by the value, not the platform or the build;
  # R on x87 (i386) changes NA's payload when copying it (as below)
  na <- writeBin(c(NA_real_, 0)[1L], raw(), endian = "little")
  skip_if(!identical(na, as.raw(c(0xa2, 0x07, 0, 0, 0, 0, 0xf0, 0x7f))),
          "R on this platform changes NaN payloads")
  manifest <- frozen_manifest()
  for (spec in frozen_fixture_specs()) {
    want <- manifest$content_hash[manifest$name == spec$name]
    path <- file.path(frozen_dir(), paste0(spec$name, ".rdz"))
    expect_identical(rdz_info(path)$content_hash, want, label = spec$name)
    if (identical(spec$codec, "native_v1")) {
      expect_identical(rdz_hash(spec$value(), mode = "native"), want, label = spec$name)
    }
  }
})

test_that("this rdz writes every frozen native speed file's bytes again", {
  # R on x87 (i386) sets the quiet bit of NA_real_, a signalling NaN, when it
  # merely copies it, so a spec's doubles there are not the bits the corpus
  # holds (R still reads both as NA)
  na <- writeBin(c(NA_real_, 0)[1L], raw(), endian = "little")
  skip_if(!identical(na, as.raw(c(0xa2, 0x07, 0, 0, 0, 0, 0xf0, 0x7f))),
          "R on this platform changes NaN payloads")
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
