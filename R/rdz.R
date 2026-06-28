#' Write an R object using the rdz format
#'
#' Common atomic vectors, lists, matrices, and data frames use a native codec.
#' Other objects automatically use R's general serialization codec.
#'
#' @param object An R object.
#' @param file A file path.
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
#' @param file A file path.
#' @return The stored R object.
#' @export
read_rdz <- function(file) {
  if (length(file) != 1L || is.na(file)) {
    stop("`file` must be one non-missing path", call. = FALSE)
  }
  .Call(C_rdz_read, path.expand(as.character(file)))
}

#' @rdname write_rdz
#' @export
fast_save <- write_rdz

#' @rdname read_rdz
#' @export
fast_read <- read_rdz
