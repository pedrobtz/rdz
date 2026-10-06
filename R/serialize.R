#' Write an R Object to an rdz File
#'
#' `write_rdz()` writes an object through the versioned `rdz` block container.
#' Logical, integer, double and character vectors, factors, lists and data
#' frames, nested and with their attributes, are stored natively; anything
#' else (in automatic mode) goes whole through R serialization, streamed in
#' blocks of 1 MiB. Every file records the object's content hash
#' ([rdz_hash()]).
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
#' @param skip_unchanged Whether to leave an existing file untouched (its
#'   bytes and modification time) when it already holds `x` with the same
#'   `metadata`: when its stored content hash equals [rdz_hash()] of `x` under
#'   the same `mode`. For caches and build tools that key on files.
#' @param metadata `NULL`, or user metadata to record with `x`: a named
#'   character vector (or a named list of single strings), up to 1,024
#'   distinct non-empty names and 64 KiB in all, such as a source, a code
#'   version or a cache key's inputs. [rdz_info()] reads it back, as
#'   `metadata`, without reading `x`; it is not part of the content hash.
#' @returns `path`, invisibly.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(list(answer = 42L), path)
#' read_rdz(path)
#' unlink(path)
#' @export
write_rdz <- function(x, path, mode = c("auto", "native", "r"), skip_unchanged = FALSE,
                      metadata = NULL) {
  path <- validate_rdz_path(path)
  mode <- match.arg(mode)
  section <- rdz_metadata_section(metadata)
  if (!is.logical(skip_unchanged) || length(skip_unchanged) != 1L || is.na(skip_unchanged)) {
    stop("`skip_unchanged` must be TRUE or FALSE.", call. = FALSE)
  }
  if (skip_unchanged && file.exists(path) && !dir.exists(path)) {
    stored <- tryCatch(rdz_info(path), error = function(e) NULL)
    if (!is.null(stored) && !is.na(stored$content_hash) &&
        identical(stored$metadata, rdz_metadata_vector(metadata)) &&
        identical(stored$content_hash, rdz_hash(x, mode = mode))) {
      return(invisible(path))
    }
  }
  directory <- dirname(path)
  if (!dir.exists(directory)) {
    stop("The destination directory does not exist: ", directory, call. = FALSE)
  }
  if (dir.exists(path)) {
    stop("`path` must not refer to a directory.", call. = FALSE)
  }

  rdz_write_to(x, path, mode, section)
  invisible(path)
}

# Writes x to path, or with a NULL path into a raw vector, which it returns:
# natively when it can (and mode allows), else through R serialization.
rdz_write_to <- function(x, path, mode, section) {
  if (!identical(mode, "r")) {
    native <- rdz_check(.Call(
      rdz_c_try_write_native, x, path, identical(mode, "native"), rdz_dictionary_policy(),
      rdz_settings(), section
    ))
    if (is.raw(native)) return(native)
    if (isTRUE(native)) return(invisible(NULL))
  }
  synopsis <- serialize(
    build_rdz_synopsis(x),
    connection = NULL,
    ascii = FALSE,
    xdr = TRUE,
    version = 3L
  )
  rdz_check(.Call(rdz_c_write_generic, x, synopsis, path, rdz_settings(), section))
}

#' Serialize an R Object to a Raw Vector
#'
#' `rdz_serialize()` gives the bytes [write_rdz()] would write, as a raw
#' vector, for a database column, a key-value store such as Redis, or a
#' socket; `rdz_unserialize()` reads them back. Everything else takes a raw
#' vector where it takes a path: [read_rdz()] (`select` included),
#' [rdz_info()], [rdz_schema()], [rdz_attributes()] and [rdz_verify()].
#'
#' @inheritParams write_rdz
#' @param bytes A raw vector holding an rdz file.
#' @param select As for [read_rdz()].
#' @returns `rdz_serialize()`: a raw vector. `rdz_unserialize()`: the object.
#' @examples
#' bytes <- rdz_serialize(mtcars)
#' identical(rdz_unserialize(bytes), mtcars)
#' rdz_unserialize(bytes, select = "mpg")
#' @export
rdz_serialize <- function(x, mode = c("auto", "native", "r"), metadata = NULL) {
  mode <- match.arg(mode)
  rdz_write_to(x, NULL, mode, rdz_metadata_section(metadata))
}

#' @rdname rdz_serialize
#' @export
rdz_unserialize <- function(bytes, select = NULL) {
  if (!is.raw(bytes)) stop("`bytes` must be a raw vector.", call. = FALSE)
  read_rdz(bytes, select = select)
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
#' A data.table's `.internal.selfref`, a pointer to the table itself, is never
#' stored: every data.table comes back as from [readRDS()], marked as loaded
#' from disk, and data.table rebuilds it by itself at the first change, at
#' the root or nested in lists. rdz never calls or loads data.table. Tibbles
#' need nothing rebuilt.
#'
#' @param path A single, non-missing path to read.
#' @param select `NULL` (everything), or the columns of a data frame or the
#'   elements of a list to read: distinct names, or distinct positive
#'   positions.
#' @param rows `NULL` (all), or the rows of a data frame or elements of a
#'   vector to read: positive positions, in any order. From a natively
#'   written file only the blocks covering `range(rows)` are read; each column
#'   is then taken with `[`, so a Date or factor column keeps its class. Stored
#'   row names are taken too; automatic ones stay automatic (`1:length(rows)`).
#' @returns The R object stored in `path`, or its selected part.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(c(TRUE, FALSE, NA), path)
#' read_rdz(path)
#' write_rdz(mtcars, path)
#' read_rdz(path, select = c("mpg", "wt"))
#' unlink(path)
#' @export
read_rdz <- function(path, select = NULL, rows = NULL) {
  path <- validate_existing_rdz_path(path)
  index <- NULL
  window <- NULL
  if (!is.null(select)) {
    info <- rdz_info(path)
    if (identical(info$codec, "native_v1")) index <- rdz_select_index(path, info, select)
  }
  if (!is.null(rows)) {
    # the window covering the rows; the C reader checks it against the root
    # (a generic file ignores it)
    rows <- rdz_rows_check(rows)
    window <- if (length(rows)) c(min(rows) - 1, max(rows)) else c(0, 0)
  }
  read <- tryCatch(
    rdz_check(.Call(rdz_c_read, path, rdz_settings(), index, window)),
    rdz_limit_error = function(e) rdz_rows_refused(path, rows, e),
    rdz_unsupported_error = function(e) rdz_rows_refused(path, rows, e)
  )
  value <- read[[1L]]
  if (!read[[2L]] && !is.null(select)) value <- rdz_select_generic(value, select)
  if (!is.null(rows)) value <- rdz_rows_take(value, rows, if (is.null(window)) 0 else window[[1L]])
  value
}

# Words the reader's refusal of `rows` for people (the directory says why).
rdz_rows_refused <- function(path, rows, e) {
  if (is.null(rows)) stop(e)
  root <- rdz_directory(path)$objects[1L, ]
  if (!root$type_name %in% c("data.frame", "logical", "integer", "double", "character",
                             "factor")) {
    stop("`rows` needs a data frame or a vector; use `select` for a list's elements.",
         call. = FALSE)
  }
  if (length(rows) && max(rows) > root$length) {
    stop("`rows` must be at most ", root$length, ".", call. = FALSE)
  }
  stop(e)
}

rdz_rows_check <- function(rows) {
  if (!is.numeric(rows) || anyNA(rows) || any(rows < 1) || any(rows != trunc(rows))) {
    stop("`rows` must be positive whole numbers.", call. = FALSE)
  }
  rows
}

# The rows of a value read whole or over a window starting after row `lo`:
# a vector's elements (through `[`, so its class's method applies), or a
# data frame's rows, each column taken the same way. A column read over the
# window (a vector) is shorter than the frame's rows was; a column read
# whole (a list, a data frame) is not. Row names: stored ones are taken
# too; automatic ones stay automatic.
rdz_rows_take <- function(value, rows, lo) {
  if (!is.data.frame(value)) {
    if (is.list(value) && !is.object(value)) {
      stop("`rows` needs a data frame or a vector; use `select` for a list's elements.",
           call. = FALSE)
    }
    if (max(c(rows, 0)) > length(value) + lo) stop("`rows` must be at most ", length(value), ".", call. = FALSE)
    return(value[rows - lo])
  }
  n <- .row_names_info(value, 2L) # rows read
  cols <- lapply(unclass(value), function(col) {
    whole <- (is.list(col) && !is.object(col)) || is.data.frame(col) || NROW(col) != n
    if (whole) col[rows] else col[rows - lo]
  })
  stored <- .row_names_info(value, 0L)
  out <- cols
  attributes(out) <- list(names = names(value))
  attr(out, "row.names") <- if (is.integer(stored) && length(stored) == 2L && is.na(stored[[1L]])) {
    .set_row_names(length(rows))
  } else {
    stored[rows - lo]
  }
  class(out) <- class(value)
  if (inherits(value, "data.table")) {
    attr(out, ".internal.selfref") <- attr(value, ".internal.selfref", exact = TRUE)
  }
  out
}

# User metadata as rdz_info() returns it: a named UTF-8 character vector
# (empty when there is none).
rdz_metadata_vector <- function(metadata) {
  if (is.null(metadata) || !length(metadata)) {
    return(stats::setNames(character(), character()))
  }
  ok <- (is.character(metadata) || is.list(metadata)) && !is.null(names(metadata)) &&
    all(vapply(metadata, function(v) is.character(v) && length(v) == 1L && !is.na(v), TRUE))
  if (!ok) {
    stop("`metadata` must be NULL or a named character vector (or a named list of single ",
         "strings), without NA.", call. = FALSE)
  }
  keys <- enc2utf8(names(metadata))
  if (anyNA(keys) || any(!nzchar(keys)) || anyDuplicated(keys)) {
    stop("`metadata` names must be distinct and non-empty.", call. = FALSE)
  }
  values <- enc2utf8(vapply(metadata, identity, ""))
  out <- stats::setNames(values, keys)
  Encoding(out) <- "UTF-8"
  out
}

# The metadata section the writer records: a u32 count, then each key and
# value as a u32 length and UTF-8 bytes (container-format.md); NULL for none.
rdz_metadata_section <- function(metadata) {
  v <- rdz_metadata_vector(metadata)
  if (!length(v)) return(NULL)
  u32 <- function(n) writeBin(as.integer(n), raw(), size = 4L, endian = "little")
  str <- function(s) { b <- charToRaw(s); c(u32(length(b)), b) }
  out <- c(u32(length(v)), unlist(lapply(seq_along(v), function(i) c(str(names(v)[i]), str(v[[i]])))))
  if (length(out) > 64 * 1024) stop("`metadata` exceeds 64 KiB.", call. = FALSE)
  out
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
    # a data.table's, as read: data.table rebuilds it at the first change
    if (inherits(value, "data.table")) {
      attr(out, ".internal.selfref") <- attr(value, ".internal.selfref", exact = TRUE)
    }
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

validate_rdz_path <- function(path) {
  if (!is.character(path) || length(path) != 1L || is.na(path) || !nzchar(path)) {
    stop("`path` must be a single, non-missing, non-empty string.", call. = FALSE)
  }
  path.expand(path)
}

# A path to an existing file, or a raw vector holding one (rdz_serialize()).
validate_existing_rdz_path <- function(path) {
  if (is.raw(path)) return(path)
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
