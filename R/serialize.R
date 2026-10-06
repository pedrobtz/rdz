#' Write an R Object to an rdz File
#'
#' `write_rdz()` writes an object through the versioned `rdz` block container.
#' Plain logical vectors and logical vectors with a supported `names` attribute
#' use the native two-bit codec. Other objects use a whole-root, portable R XDR
#' stream in automatic mode, written in blocks of 1 MiB.
#'
#' Two options control how blocks are stored and how many threads do the
#' work. `options(rdz.preset = )` is `"balanced"` (the default; each block is
#' compressed with Zstandard at level 1), `"compact"` (level 6) or `"speed"`
#' (no compression). A block is stored compressed only when that makes it
#' smaller. `options(rdz.threads = )` sets the threads that compress and, in
#' [read_rdz()], decompress blocks; the default is 1. The file does not
#' depend on either: any setting reads any file, and the same object written
#' with any number of threads gives the same bytes.
#'
#' @param x An R object to serialize.
#' @param path A single, non-missing path to write.
#' @param mode Codec selection. `"auto"` uses a native codec only when the
#'   complete value is supported and otherwise uses whole-root R serialization;
#'   `"native"` rejects unsupported values; `"r"` forces R serialization.
#' @returns `path`, invisibly.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(list(answer = 42L), path)
#' read_rdz(path)
#' unlink(path)
#' @export
write_rdz <- function(x, path, mode = c("auto", "native", "r")) {
  path <- validate_rdz_path(path)
  mode <- match.arg(mode)
  directory <- dirname(path)
  if (!dir.exists(directory)) {
    stop("The destination directory does not exist: ", directory, call. = FALSE)
  }
  if (dir.exists(path)) {
    stop("`path` must not refer to a directory.", call. = FALSE)
  }

  if (!identical(mode, "r")) {
    native_written <- rdz_check(.Call(
      rdz_c_try_write_native, x, path, identical(mode, "native"), rdz_dictionary_policy(),
      rdz_settings()
    ))
    if (isTRUE(native_written)) {
      return(invisible(path))
    }
  }

  synopsis <- serialize(
    build_rdz_synopsis(x),
    connection = NULL,
    ascii = FALSE,
    xdr = TRUE,
    version = 3L
  )
  rdz_check(.Call(rdz_c_write_generic, x, synopsis, path, rdz_settings()))
  invisible(path)
}

#' Read an R Object from an rdz File
#'
#' `read_rdz()` validates the header, trailer, directory, block headers, bounds,
#' and block checksums before deserializing a generic payload.
#'
#' `select` reads some columns of a data frame or some elements of a list,
#' by name or by position, in the order given. From a natively written file
#' only the selected parts are read and decoded; a file written through R
#' serialization is read whole and then subset. A data frame keeps its row
#' names and class, and a list its names; the root's other attributes, which
#' may describe the parts left out (a data.table's key), are not kept, and
#' [rdz_attributes()] still reads them.
#'
#' @param path A single, non-missing path to read.
#' @param select `NULL` (everything), or the columns of a data frame or the
#'   elements of a list to read: distinct names, or distinct positive
#'   positions.
#' @returns The R object stored in `path`, or its selected part.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(c(TRUE, FALSE, NA), path)
#' read_rdz(path)
#' write_rdz(mtcars, path)
#' read_rdz(path, select = c("mpg", "wt"))
#' unlink(path)
#' @export
read_rdz <- function(path, select = NULL) {
  path <- validate_existing_rdz_path(path)
  index <- NULL
  if (!is.null(select)) {
    info <- rdz_info(path)
    if (identical(info$codec, "native_v1")) {
      index <- rdz_select_index(path, info, select)
    }
  }
  value <- rdz_check(.Call(rdz_c_read, path, rdz_settings(), index))
  if (!is.null(select) && is.null(index)) {
    value <- rdz_select_generic(value, select)
  }
  # A data.table's .internal.selfref is not stored (it is a pointer to the
  # object itself); data.table restores it, as it does for every reader.
  if (inherits(value, "data.table") && requireNamespace("data.table", quietly = TRUE)) {
    value <- data.table::setalloccol(value)
  }
  value
}

# The 0-based children `select` names in a native file's list or data frame
# root, reading only its names.
rdz_select_index <- function(path, info, select) {
  if (!info$root_type %in% c("list", "data.frame")) {
    stop("`select` needs a list or a data frame; the file holds ",
         if (nzchar(info$root_type)) info$root_type else "neither", ".", call. = FALSE)
  }
  n <- info$root_length
  names <- if ("names" %in% info$attribute_names) {
    rdz_check(.Call(rdz_c_read_native_attribute, path, "names"))
  }
  rdz_select_positions(select, n, names) - 1L
}

# The 1-based positions `select` names among n parts with these names.
rdz_select_positions <- function(select, n, names) {
  if (is.character(select)) {
    if (anyNA(select)) stop("`select` must not contain NA.", call. = FALSE)
    if (is.null(names)) stop("`select` names parts, but they have no names.", call. = FALSE)
    at <- match(select, names)
    if (anyNA(at)) {
      stop("Unknown in `select`: ", paste(select[is.na(at)], collapse = ", "), call. = FALSE)
    }
  } else if (is.numeric(select)) {
    if (anyNA(select) || any(select != trunc(select)) || any(select < 1) || any(select > n)) {
      stop("`select` positions must be whole numbers from 1 to ", n, ".", call. = FALSE)
    }
    at <- as.integer(select)
  } else {
    stop("`select` must be NULL, a character vector or a numeric vector.", call. = FALSE)
  }
  if (anyDuplicated(at)) stop("`select` must not repeat a part.", call. = FALSE)
  at
}

# The same selection from a value read whole (a generic file): the parts,
# with their names; a data frame's row names and class.
rdz_select_generic <- function(value, select) {
  if (typeof(value) != "list") {
    stop("`select` needs a list or a data frame.", call. = FALSE)
  }
  at <- rdz_select_positions(select, length(value), names(value))
  out <- unclass(value)[at]
  attributes(out) <- if (is.null(names(value))) NULL else list(names = names(value)[at])
  if (is.data.frame(value)) {
    attr(out, "row.names") <- .row_names_info(value, 0L)
    class(out) <- class(value)
  }
  out
}

#' Inspect an rdz Container Without Reading Its Payload
#'
#' `rdz_info()` reads the fixed header, closing trailer, and bounded directory.
#' For the transitional R-serialization codec, `synopsis` is informative and
#' exact attribute values still require [read_rdz()]. Data block checksums are
#' validated by [read_rdz()], not by this metadata-only operation. `writer` names
#' the implementation and version that wrote the file, or is `""` when the file
#' does not record one.
#'
#' @param path A single, non-missing path to inspect.
#' @returns A named list of container information and a bounded root synopsis.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(data.frame(value = 1:3), path)
#' rdz_info(path)
#' unlink(path)
#' @export
rdz_info <- function(path) {
  path <- validate_existing_rdz_path(path)
  info <- rdz_c_file_info(path)
  synopsis <- if (length(info$synopsis) == 0L) {
    NULL
  } else {
    tryCatch(
      unserialize(info$synopsis),
      error = function(error) {
        stop("The rdz root synopsis is invalid: ", conditionMessage(error), call. = FALSE)
      }
    )
  }
  info$synopsis <- synopsis
  native <- identical(info$codec, "native_v1")
  info$authoritative <- native
  info$exact_attributes <- native
  info$full_read_required_for_attributes <- !native
  info$schema <- if (native) {
    list(
      root_type = info$root_type,
      length = info$root_length,
      attribute_names = info$attribute_names
    )
  } else if (is.null(synopsis)) {
    NULL
  } else {
    list(
      root_type = synopsis$root_type,
      length = synopsis$length,
      attribute_names = synopsis$attribute_names
    )
  }
  info$integrity_checks <- c(
    "header_xxh3",
    "directory_xxh3",
    "directory_and_block_bounds"
  )
  class(info) <- "rdz_info"
  info
}

#' @rdname rdz_info
#' @param x An `rdz_info` object.
#' @param ... Additional arguments, currently unused.
#' @returns `x`, invisibly.
#' @export
print.rdz_info <- function(x, ...) {
  cat("<rdz_info>\n")
  cat("  codec: ", x$codec, " (version ", x$codec_version, ")\n", sep = "")
  cat("  container version: ", x$container_version, "\n", sep = "")
  if (nzchar(x$writer)) cat("  written by: ", x$writer, "\n", sep = "")
  cat("  blocks: ", x$block_count, " (maximum ", x$block_size, " bytes)\n", sep = "")
  cat("  payload: ", format(x$payload_bytes, big.mark = ","), " bytes\n", sep = "")
  cat(
    "  metadata: ",
    if (isTRUE(x$authoritative)) "authoritative native directory" else "non-authoritative synopsis",
    "\n",
    sep = ""
  )
  invisible(x)
}

#' Inspect an rdz Object Schema
#'
#' `rdz_schema()` reads the bounded directory without reading object data
#' blocks. Native schemas are authoritative; generic schemas are bounded
#' synopses and are marked non-authoritative.
#'
#' @param path A single, non-missing path to inspect.
#' @returns A named schema list.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(c(first = TRUE, second = NA), path)
#' rdz_schema(path)
#' unlink(path)
#' @export
rdz_schema <- function(path) {
  info <- rdz_info(path)
  structure(
    c(
      list(
        codec = info$codec,
        authoritative = info$authoritative,
        exact_attributes = info$exact_attributes
      ),
      info$schema,
      list(data_blocks_read = FALSE)
    ),
    class = "rdz_schema"
  )
}

#' Read rdz Object Attributes
#'
#' Native supported attributes are independently addressable and can be read
#' without decoding the root data blocks. Generic files require
#' `allow_full = TRUE`, which explicitly permits a complete object read.
#'
#' @param path A single, non-missing path to inspect.
#' @param object Object identifier. Phase 1 supports only the root, represented
#'   by `NULL` or `0`.
#' @param names Optional character vector selecting attribute names.
#' @param allow_full Whether generic files may be fully deserialized.
#' @returns A named list of attribute values.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(c(first = TRUE, second = FALSE), path)
#' rdz_attributes(path, names = "names")
#' unlink(path)
#' @export
rdz_attributes <- function(path, object = NULL, names = NULL, allow_full = FALSE) {
  path <- validate_existing_rdz_path(path)
  if (!is.null(object) && !identical(object, 0L) && !identical(object, 0)) {
    stop("Phase 1 supports attribute access only for the root object.", call. = FALSE)
  }
  if (!is.null(names) &&
      (!is.character(names) || anyNA(names) || any(!nzchar(names)))) {
    stop("`names` must be NULL or a character vector of non-empty names.", call. = FALSE)
  }
  if (!is.logical(allow_full) || length(allow_full) != 1L || is.na(allow_full)) {
    stop("`allow_full` must be TRUE or FALSE.", call. = FALSE)
  }

  info <- rdz_info(path)
  if (identical(info$codec, "native_v1")) {
    available <- info$schema$attribute_names
    requested <- if (is.null(names)) available else names
    unknown <- setdiff(requested, available)
    if (length(unknown)) {
      stop("Unknown attribute: ", paste(unknown, collapse = ", "), call. = FALSE)
    }
    if (!length(requested)) {
      return(list())
    }
    values <- lapply(requested, function(name) {
      rdz_check(.Call(rdz_c_read_native_attribute, path, name))
    })
    return(stats::setNames(values, requested))
  }
  if (!allow_full) {
    stop(
      "Exact generic attributes require a full read; set `allow_full = TRUE`.",
      call. = FALSE
    )
  }
  values <- attributes(read_rdz(path))
  if (is.null(values)) {
    values <- list()
  }
  available <- base::names(values)
  requested <- if (is.null(names)) available else names
  unknown <- setdiff(requested, available)
  if (length(unknown)) {
    stop("Unknown attribute: ", paste(unknown, collapse = ", "), call. = FALSE)
  }
  values[requested]
}

validate_rdz_path <- function(path) {
  if (!is.character(path) || length(path) != 1L || is.na(path) || !nzchar(path)) {
    stop("`path` must be a single, non-missing, non-empty string.", call. = FALSE)
  }
  path.expand(path)
}

validate_existing_rdz_path <- function(path) {
  path <- validate_rdz_path(path)
  if (!file.exists(path) || dir.exists(path)) {
    stop("The file does not exist: ", path, call. = FALSE)
  }
  path
}

build_rdz_synopsis <- function(x) {
  tryCatch(
    build_rdz_synopsis_impl(x),
    error = function(error) {
      list(
        synopsis_version = 1L,
        authoritative = FALSE,
        root_type = typeof(x),
        class = character(),
        length = NULL,
        dimensions = NULL,
        attribute_names = character(),
        truncated = TRUE,
        synopsis_error = TRUE,
        exact_attributes_require_full_read = TRUE
      )
    }
  )
}

build_rdz_synopsis_impl <- function(x) {
  root_length <- .Call(rdz_c_root_length, x)
  if (root_length < 0) {
    root_length <- NULL
  }
  classes <- attr(x, "class", exact = TRUE)
  if (!is.character(classes)) {
    classes <- character()
  }
  classes <- bound_rdz_strings(classes, count = 32L, bytes = 256L)

  dimensions <- attr(x, "dim", exact = TRUE)
  if (!is.integer(dimensions) && !is.double(dimensions)) {
    dimensions <- NULL
  }
  dimensions_truncated <- length(dimensions) > 32L
  dimensions <- utils::head(dimensions, 32L)

  attribute_names <- names(attributes(x))
  if (is.null(attribute_names)) {
    attribute_names <- character()
  }
  attribute_names <- bound_rdz_strings(attribute_names, count = 64L, bytes = 256L)

  list(
    synopsis_version = 1L,
    authoritative = FALSE,
    root_type = typeof(x),
    class = classes$values,
    length = root_length,
    dimensions = dimensions,
    attribute_names = attribute_names$values,
    truncated = classes$truncated || dimensions_truncated || attribute_names$truncated,
    synopsis_error = FALSE,
    exact_attributes_require_full_read = TRUE
  )
}

bound_rdz_strings <- function(x, count, bytes) {
  selected <- utils::head(x, count)
  values <- vapply(
    selected,
    truncate_rdz_string,
    character(1L),
    bytes = bytes,
    USE.NAMES = FALSE
  )
  shortened <- if (length(selected) == 0L) {
    FALSE
  } else {
    any(!vapply(
      seq_along(selected),
      function(index) rdz_strings_equal(selected[[index]], values[[index]]),
      logical(1L)
    ))
  }
  list(
    values = values,
    truncated = length(x) > count || shortened
  )
}

truncate_rdz_string <- function(value, bytes) {
  if (is.na(value)) {
    return(NA_character_)
  }
  if (identical(Encoding(value), "bytes")) {
    encoded <- charToRaw(value)
    if (length(encoded) > bytes) {
      encoded <- encoded[seq_len(bytes)]
    }
    output <- rawToChar(encoded)
    Encoding(output) <- "bytes"
    return(output)
  }

  value <- enc2utf8(value)
  if (nchar(value, type = "bytes") <= bytes) {
    return(value)
  }

  lower <- 0L
  upper <- min(nchar(value, type = "chars"), bytes)
  while (lower < upper) {
    middle <- (lower + upper + 1L) %/% 2L
    candidate <- substr(value, 1L, middle)
    if (nchar(candidate, type = "bytes") <= bytes) {
      lower <- middle
    } else {
      upper <- middle - 1L
    }
  }
  substr(value, 1L, lower)
}

rdz_strings_equal <- function(left, right) {
  if (is.na(left) || is.na(right)) {
    return(is.na(left) && is.na(right))
  }
  left_is_bytes <- identical(Encoding(left), "bytes")
  right_is_bytes <- identical(Encoding(right), "bytes")
  if (left_is_bytes != right_is_bytes) {
    return(FALSE)
  }
  if (!left_is_bytes) {
    left <- enc2utf8(left)
    right <- enc2utf8(right)
  }
  identical(charToRaw(left), charToRaw(right))
}
