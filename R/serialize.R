#' Write an R Object to a File
#'
#' Initial implementation: delegates to [base::saveRDS()].
#'
#' @param x An R object to serialise.
#' @param path Path of the file to write to.
#' @returns `path`, invisibly.
#' @export
write_rdz <- function(x, path) {
  base::saveRDS(x, path)
  invisible(path)
}

#' Read an R Object from a File
#'
#' Initial implementation: delegates to [base::readRDS()].
#'
#' @param path Path of the file to read from.
#' @returns The R object stored in `path`.
#' @export
read_rdz <- function(path) {
  base::readRDS(path)
}
