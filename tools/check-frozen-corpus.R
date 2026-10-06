# The frozen corpus only grows (container-format.md, "Compatibility and
# extension"): every manifest row a release wrote (its writer has no
# "(development)") at the base commit is still there, unchanged, at HEAD.
# The tests check each file against its row's size and SHA-256, so an
# unchanged row is an unchanged file. From the package root:
#
#   Rscript tools/check-frozen-corpus.R <base-commit>

args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 1L) stop("usage: Rscript tools/check-frozen-corpus.R <base-commit>")
path <- "tests/testthat/fixtures/v0.1.0/manifest.tsv"
read_manifest <- function(lines) {
  if (!length(lines)) return(NULL)
  utils::read.delim(text = paste(lines, collapse = "\n"), colClasses = "character",
                    quote = "")
}
base <- read_manifest(suppressWarnings(
  system2("git", c("show", paste0(args[[1L]], ":", path)), stdout = TRUE, stderr = FALSE)
))
head <- read_manifest(readLines(path))
if (is.null(base)) {
  cat("no frozen corpus at", args[[1L]], "\n")
  quit(status = 0L)
}
released <- base[!grepl("(development)", base$writer, fixed = TRUE), , drop = FALSE]
key <- function(m) do.call(paste, c(unname(as.list(m)), sep = "\t"))
missing <- released$name[!key(released) %in% key(head[, names(released), drop = FALSE])]
if (length(missing)) {
  cat("Frozen fixtures changed or removed since ", args[[1L]], ":\n  ",
      paste(missing, collapse = "\n  "), "\n", sep = "")
  quit(status = 1L)
}
cat(nrow(released), "frozen fixtures unchanged;", nrow(head) - nrow(base), "added\n")
