# Writes the frozen 0.1.0 corpus (tests/testthat/fixtures/v0.1.0/) from the
# specs in tests/testthat/helper-frozen-fixtures.R, with the installed rdz,
# and its manifest. A file once written is data: this writes only the specs
# that have no file yet (an addition, such as Stage J's decimals) and keeps
# every existing file and manifest row as it is. From the package root:
#
#   Rscript tools/make-frozen-fixtures.R

source(file.path("tests", "testthat", "helper-frozen-fixtures.R"))
dir <- file.path("tests", "testthat", "fixtures", "v0.1.0")
dir.create(dir, recursive = TRUE, showWarnings = FALSE)
manifest_path <- file.path(dir, "manifest.tsv")
old <- if (file.exists(manifest_path)) {
  utils::read.delim(manifest_path, colClasses = "character", quote = "")
} else {
  NULL
}

encodings <- function(path) {
  bytes <- readBin(path, "raw", file.size(path))
  le <- function(at, w) sum(as.numeric(bytes[at + seq_len(w)]) * 256^(seq_len(w) - 1L))
  dir <- le(length(bytes) - 40L + 8L, 8L)
  first <- dir + le(dir + 6L, 2L) + 48L * le(dir + 16L, 4L) + 32L * le(dir + 20L, 4L)
  n <- le(dir + 24L, 4L)
  e <- vapply(seq_len(n) - 1L, function(k) le(first + 64L * k + 48L, 2L), numeric(1L))
  c <- vapply(seq_len(n) - 1L, function(k) le(first + 64L * k + 50L, 2L), numeric(1L))
  c(paste(sort(unique(e)), collapse = " "), paste(sort(unique(c)), collapse = " "))
}

rows <- lapply(frozen_fixture_specs(), function(spec) {
  path <- file.path(dir, paste0(spec$name, ".rdz"))
  if (file.exists(path)) { # never rewritten
    stopifnot(!is.null(old), spec$name %in% old$name)
    return(old[old$name == spec$name, , drop = FALSE])
  }
  write_frozen_fixture(spec, path)
  info <- rdz::rdz_info(path)
  stopifnot(identical(info$codec, spec$codec), identical(rdz::read_rdz(path), spec$value()))
  e <- encodings(path)
  data.frame(name = spec$name, preset = spec$preset, codec = info$codec,
             bytes = as.character(file.size(path)), sha256 = unname(tools::sha256sum(path)),
             encodings = e[[1L]], compressions = e[[2L]], writer = info$writer,
             content_hash = info$content_hash)
})
manifest <- do.call(rbind, rows)
utils::write.table(manifest, file.path(dir, "manifest.tsv"), sep = "\t", quote = FALSE,
                   row.names = FALSE)
cat(nrow(manifest), "files,", sum(as.numeric(manifest$bytes)), "bytes\n")
