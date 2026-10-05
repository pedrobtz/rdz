# Writes the Rust reference corpus (plan-c.md section 2) into
# tests/testthat/fixtures/rust/: one .rdz file per spec in
# tests/testthat/helper-rust-fixtures.R, manifest.tsv, and info.rds holding
# what the Rust rdz_info() reports for each file.
#
# Run from the package root, after installing rdz from a clean checkout of the
# commit being recorded:
#
#   R CMD INSTALL . && Rscript tools/make-rust-fixtures.R

source(file.path("tests", "testthat", "helper-rust-fixtures.R"))

git <- function(...) system2("git", c(...), stdout = TRUE)
if (length(git("status", "--porcelain", "--", "src", "R", "DESCRIPTION")) > 0L) {
  stop("Commit the implementation before generating the corpus.", call. = FALSE)
}
commit <- git("rev-parse", "HEAD")

dir <- file.path("tests", "testthat", "fixtures", "rust")
unlink(dir, recursive = TRUE)
dir.create(dir, recursive = TRUE)

with_policy <- function(policy, code) {
  old <- Sys.getenv("RDZ_STRING_DICT", unset = NA)
  Sys.setenv(RDZ_STRING_DICT = policy)
  on.exit(if (is.na(old)) Sys.unsetenv("RDZ_STRING_DICT") else Sys.setenv(RDZ_STRING_DICT = old))
  force(code)
}

rows <- list()
infos <- list()
for (spec in rust_fixture_specs()) {
  path <- file.path(dir, paste0(spec$name, ".rdz"))
  with_policy(spec$policy, rdz::write_rdz(spec$value(), path, mode = spec$mode))
  info <- rdz::rdz_info(path)
  blocks <- rdz_block_encodings(path)
  rows[[spec$name]] <- data.frame(
    name = spec$name,
    mode = spec$mode,
    policy = spec$policy,
    codec = info$codec,
    blocks = info$block_count,
    encodings = paste(sort(unique(blocks$encoding)), collapse = " "),
    bytes = file.size(path),
    sha256 = unname(tools::sha256sum(path)),
    commit = commit,
    spec = spec$spec
  )
  infos[[spec$name]] <- unclass(info)
}

manifest <- do.call(rbind, rows)
write.table(manifest, file.path(dir, "manifest.tsv"), sep = "\t", quote = FALSE,
            row.names = FALSE)
saveRDS(infos, file.path(dir, "info.rds"), version = 3L)
cat(sprintf("%d fixtures, %.1f MB, from %s\n", nrow(manifest),
            sum(manifest$bytes) / 1e6, commit))
