#' Write an R object using the rdz format
#'
#' Common atomic vectors, lists, matrices, and data frames use a native codec.
#' Other objects automatically use R's general serialization codec.
#'
#' @param object An R object.
#' @param file A file path, conventionally ending in `.rdz`.
#' @param codec One of `"auto"`, `"native"`, or `"r"`. `"native"` errors
#'   rather than falling back when the object is unsupported.
#' @param preset `"speed"` stores native numeric and logical vectors directly.
#'   `"balanced"` packs logical and small-range integer vectors and adaptively
#'   encodes low-cardinality, exact constant-delta, and compressible
#'   high-cardinality numeric vectors. Packed numeric indexes, XOR-delta numeric
#'   byte lanes, and flat string payloads use independent LZ4 blocks only when
#'   their selection criteria are satisfied.
#' @return `file`, invisibly.
#' @export
write_rdz <- function(object, file, codec = c("auto", "native", "r"),
                      preset = c("speed", "balanced")) {
  codec <- match.arg(codec)
  preset <- match.arg(preset)
  if (length(file) != 1L || is.na(file)) {
    stop("`file` must be one non-missing path", call. = FALSE)
  }
  file <- path.expand(as.character(file))
  .Call(
    C_rdz_save,
    object,
    file,
    match(codec, c("auto", "native", "r")) - 1L,
    match(preset, c("speed", "balanced")) - 1L
  )
  invisible(file)
}

#' Read an object written by rdz
#'
#' @param file A file path, conventionally ending in `.rdz`.
#' @return The stored R object.
#' @export
read_rdz <- function(file) {
  if (length(file) != 1L || is.na(file)) {
    stop("`file` must be one non-missing path", call. = FALSE)
  }
  .Call(C_rdz_read, path.expand(as.character(file)))
}

#' Explain how an object would be serialized
#'
#' Runs the same codec-selection path as [write_rdz()] and returns one row
#' for each native node. The encoded bytes are discarded rather than written to
#' a user-visible file. Because selection is exact, this function performs the
#' same scanning and compression work as serialization.
#'
#' @param object An R object.
#' @param codec One of `"auto"`, `"native"`, or `"r"`.
#' @param preset One of `"speed"` or `"balanced"`.
#' @return A data frame with one row per native node and columns `path`,
#'   `relation`, `name`, `index`, `depth`, `type`, `length`, `codec`,
#'   `strategy`, and `encoded_bytes`. Element paths use positional `[[i]]`
#'   notation and attribute paths use `@name`; list names are reported in the
#'   `name` column. A container's byte count includes its attributes and
#'   descendants, and the root count is the complete file size. R-serialization
#'   fallback is represented by one root row.
#' @export
explain_rdz <- function(object, codec = c("auto", "native", "r"),
                        preset = c("speed", "balanced")) {
  codec <- match.arg(codec)
  preset <- match.arg(preset)
  .Call(
    C_rdz_explain,
    object,
    match(codec, c("auto", "native", "r")) - 1L,
    match(preset, c("speed", "balanced")) - 1L
  )
}

#' @rdname write_rdz
#' @export
write_fastrds <- write_rdz

#' @rdname read_rdz
#' @export
read_fastrds <- read_rdz

#' @rdname explain_rdz
#' @export
explain_fastrds <- explain_rdz

#' @rdname write_rdz
#' @export
fast_save <- write_rdz

#' @rdname read_rdz
#' @export
fast_read <- read_rdz
