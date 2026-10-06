# Content hashes (container-format.md, "Content hash"): a hash of the value
# rdz stores, the same whether computed from an object or read from the file
# that holds it.

#' Content Hash of an R Object
#'
#' `rdz_hash()` is the 128-bit hash of `x` as [write_rdz()] would store it,
#' and every file it writes records that hash in its directory, where
#' [rdz_info()] reads it as `content_hash` without touching the data. A value
#' gives the same hash on every platform and R version, whatever the level,
#' threads, block sizes or encodings chosen, and whether its vectors are
#' ALTREP or not; so a cache can key files by it, and compare an object with
#' a file without reading the file.
#'
#' Natively stored values are hashed from their canonical form: types,
#' lengths, attribute structure and values (doubles bit for bit, strings with
#' their encoding). Values written through R serialization are hashed, as
#' the digest package does, from R serialization version 2 without its
#' header (so ALTREP and the R version do not enter it); equal values then
#' hash equally only when R serializes them equally, which excludes, for
#' example, a closure and its byte-compiled copy. The hash identifies the
#' value as stored, so it depends on `mode`: a value written natively and the
#' same value written with `mode = "r"` hash differently. XXH3-128 is not a
#' cryptographic hash.
#'
#' The hash also records sharing: a vector of 4 KiB or more that appears
#' twice in `x` as one object is stored once ([write_rdz()], "Native and
#' generic"), and hashes as a reference to its first occurrence. So
#' `list(v, v)` and `list(v, v + 0)` are [identical()] but hash differently
#' when `v` is that large; build values the same way to get the same hash.
#'
#' @param x An R object.
#' @param mode As for [write_rdz()].
#' @returns The hash, 32 hexadecimal digits.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(mtcars, path)
#' identical(rdz_hash(mtcars), rdz_info(path)$content_hash)
#' unlink(path)
#' @export
rdz_hash <- function(x, mode = c("auto", "native", "r")) {
  mode <- match.arg(mode)
  if (!identical(mode, "r")) {
    h <- rdz_check(.Call(rdz_c_hash_native, x, identical(mode, "native")))
    if (is.character(h)) return(h)
  }
  .Call(rdz_c_hash_generic, x)
}

#' Verify an rdz File
#'
#' `rdz_verify()` checks a whole file without building any R object: the
#' header, trailer and directory, then every block's checksum, without
#' decompressing it. With `content = TRUE` it also reads the value and checks
#' it against the content hash the file records.
#'
#' @param path A path, or a raw vector holding an rdz file ([rdz_serialize()]).
#' @param content Whether to read the value and compare its [rdz_hash()] with
#'   the stored one.
#' @returns `path`, invisibly, when the file is sound; otherwise an
#'   `rdz_error` (an `rdz_format_error` for a corrupt file).
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(mtcars, path)
#' rdz_verify(path, content = TRUE)
#' unlink(path)
#' @export
rdz_verify <- function(path, content = FALSE) {
  path <- validate_existing_rdz_path(path)
  if (!is.logical(content) || length(content) != 1L || is.na(content)) {
    rdz_stop("`content` must be TRUE or FALSE.", call. = FALSE)
  }
  rdz_check(.Call(rdz_c_verify, path))
  if (content) {
    info <- rdz_info(path)
    if (is.na(info$content_hash)) {
      rdz_stop("The file records no content hash.", call. = FALSE)
    }
    mode <- if (identical(info$codec, "native_v1")) "native" else "r"
    if (!identical(rdz_hash(read_rdz(path), mode = mode), info$content_hash)) {
      stop(structure(
        class = c("rdz_format_error", "rdz_error", "error", "condition"),
        list(message = "invalid rdz file: the value does not match its content hash",
             call = NULL)
      ))
    }
  }
  invisible(path)
}
