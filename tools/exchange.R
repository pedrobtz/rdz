# Cross-platform fixture exchange (plan-c Stage I, roadmap Phase 8). Each CI
# job writes the frozen corpus's specs on its own platform; every job then
# reads every platform's files. From the package root, with rdz installed:
#
#   Rscript tools/exchange.R write DIR          # this platform's files
#   Rscript tools/exchange.R check DIR [DIR...]  # every platform's files
#
# `check` requires every file to read to its spec's value, and every
# uncompressed (speed) native file to be the same bytes on every platform
# and the same as the frozen corpus's but the writer field: the format has
# no platform in it. The files in frozen_wider_codes (their small integer
# codes are now stored as bytes or halfwords) must instead be larger than
# the corpus's. Compressed files may differ only if the platforms' zstd
# builds do, and generic files if their R versions do (R's serialization
# header records it); that is reported, not failed.

source(file.path("tests", "testthat", "helper-frozen-fixtures.R"))
args <- commandArgs(trailingOnly = TRUE)
stopifnot(length(args) >= 2L, args[[1L]] %in% c("write", "check"))
specs <- frozen_fixture_specs()
corpus <- file.path("tests", "testthat", "fixtures", "v0.1.0")

but_writer <- function(path) {
  bytes <- readBin(path, "raw", file.size(path))
  bytes[21:32] <- as.raw(0L)
  bytes
}

if (args[[1L]] == "write") {
  dir.create(args[[2L]], recursive = TRUE, showWarnings = FALSE)
  for (spec in specs) write_frozen_fixture(spec, file.path(args[[2L]], paste0(spec$name, ".rdz")))
  writeLines(c(R.version.string, Sys.info()[["sysname"]], Sys.info()[["machine"]],
               .Platform$endian, rdz:::rdz_zstd_version()),
             file.path(args[[2L]], "platform.txt"))
  cat("wrote", length(specs), "files to", args[[2L]], "\n")
  quit(status = 0L)
}

dirs <- args[-1L]
failures <- 0L
manifest <- utils::read.delim(file.path(corpus, "manifest.tsv"), colClasses = "character",
                              quote = "")
fail <- function(...) {
  cat("FAIL:", ..., "\n")
  failures <<- failures + 1L
}
for (d in dirs) cat("==>", d, ":", paste(readLines(file.path(d, "platform.txt")), collapse = " | "), "\n")
for (spec in specs) {
  file <- paste0(spec$name, ".rdz")
  value <- spec$value()
  bytes <- list()
  for (d in dirs) {
    path <- file.path(d, file)
    got <- tryCatch(rdz::read_rdz(path), error = function(e) e)
    if (inherits(got, "error")) {
      fail(d, file, conditionMessage(got))
    } else if (!identical(got, value)) {
      fail(d, file, "does not read to its spec's value")
    }
    bytes[[d]] <- but_writer(path)
  }
  # the content hash names the value: the same on every platform, and the
  # corpus's
  hashes <- vapply(dirs, function(d) rdz::rdz_info(file.path(d, file))$content_hash, "")
  want <- manifest$content_hash[manifest$name == spec$name]
  if (!all(hashes == want)) fail(file, "content hash differs:", paste(unique(hashes), collapse = " "))
  same <- all(vapply(bytes, identical, logical(1L), bytes[[1L]]))
  if (identical(spec$preset, "speed") && identical(spec$codec, "native_v1")) {
    if (!same) fail(file, "differs between platforms")
    frozen <- but_writer(file.path(corpus, file))
    if (spec$name %in% frozen_wider_codes) {
      if (length(bytes[[1L]]) <= length(frozen)) fail(file, "is not written with wider codes")
    } else if (!identical(bytes[[1L]], frozen)) {
      fail(file, "differs from the frozen corpus")
    }
  } else if (!same) {
    cat("note:", file, "differs between platforms (zstd or R versions)\n")
  }
}
cat(length(specs), "specs,", length(dirs), "platforms,", failures, "failures\n")
quit(status = if (failures) 1L else 0L)
