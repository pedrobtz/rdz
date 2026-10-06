#' Write an R Object to an rdz File
#'
#' `write_rdz()` writes an object through the versioned `rdz` block container.
#' Logical, integer, double and character vectors, factors, lists and data
#' frames, nested and with their attributes, are stored natively; anything
#' else (in automatic mode) goes whole through R serialization, streamed in
#' blocks of 1 MiB. Every file records the object's content hash
#' ([rdz_hash()]).
#'
#' `compress` is the Zstandard level each block is compressed at: `0` stores
#' every block raw (the fastest to write and read), `1` (the default) is fast
#' and already small, and higher levels, up to `19`, trade write time for
#' size; reads stay fast at any level. Below level 6 a block is stored
#' compressed only when that saves at least an eighth of it, so data that
#' barely compresses reads at memory speed; from level 6, whenever it is
#' smaller. `options(rdz.threads = )` sets the threads that compress and, in
#' [read_rdz()], decompress blocks; the default is 1. The file does not
#' depend on either: any setting reads any file, and the same object written
#' with any number of threads gives the same bytes.
#'
#' @section Native and generic:
#' In automatic mode (`mode = "auto"`), a value is written natively when every
#' part of it can be; otherwise the whole value goes through R serialization.
#' Native files support selective reads (`select`, `rows`) and inspection
#' below the root ([rdz_schema()], [rdz_attributes()]); generic files are read
#' whole. A value is written generically when it holds, anywhere:
#' * a type other than logical, integer, double, character or list (complex
#'   and raw vectors, environments, functions, calls, S4 objects);
#' * a string longer than 1 MiB, or a non-ASCII string the session cannot
#'   convert to UTF-8 losslessly (unmarked strings in the C locale);
#' * a non-ASCII attribute name, or names, row names or a class carrying
#'   attributes of their own;
#' * a factor with names, or a data frame column of another length than its
#'   rows (a matrix of two or more columns);
#' * nesting deeper than 1,000 levels, or more than 1,000,000 parts.
#'
#' Automatic mode also writes generically a value of at least 1,024 parts that
#' average less than 1 KiB of data each, where the native directory would
#' cost more than the data. `mode = "native"` writes those natively, and
#' raises an `rdz_unsupported_error` (see [rdz-errors]) for the others.
#'
#' A vector of 4 KiB or more that appears more than once in `x` (the same
#' object, as after `y <- x`) is stored once and read back shared.
#'
#' @section Replacing files:
#' The file is written to a temporary file beside `path` and renamed over it
#' at the end, so readers see the old file or the new one, never a partial
#' one, and an error or interrupt leaves `path` as it was. rdz does not ask
#' the operating system to flush the file to disk (no `fsync`), so after a
#' power failure the file may be missing or empty. On file systems whose
#' rename cannot replace a file (some Windows shares), the old file is moved
#' aside first, and a crash at that moment can leave it under a name ending
#' in `.backup`.
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
#' @param compress The Zstandard level of each block, a whole number from `0`
#'   (none) to `19`; by default `options(rdz.compress)`, else `1`.
#' @param hash Whether to record the content hash ([rdz_hash()]), from
#'   `options(rdz.hash)`, `TRUE` by default. Computing it reads the value once
#'   more (about a tenth of a write's time); without it the file is just as
#'   valid and as readable, but `skip_unchanged`, `rdz_verify(content = TRUE)`
#'   and `rdz_info()$content_hash` (then `NA`) have nothing to use.
#' @returns `path`, invisibly.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(list(answer = 42L), path)
#' read_rdz(path)
#' unlink(path)
#' @export
write_rdz <- function(x, path, mode = c("auto", "native", "r"), skip_unchanged = FALSE,
                      metadata = NULL, compress = getOption("rdz.compress", 1L),
                      hash = getOption("rdz.hash", TRUE)) {
  path <- validate_rdz_path(path)
  mode <- match.arg(mode)
  section <- rdz_metadata_section(metadata)
  if (!is.logical(skip_unchanged) || length(skip_unchanged) != 1L || is.na(skip_unchanged)) {
    rdz_stop("`skip_unchanged` must be TRUE or FALSE.", call. = FALSE)
  }
  hash <- rdz_hash_flag(hash)
  if (missing(compress)) compress <- rdz_legacy_compress(compress)
  compress <- rdz_compress_level(compress)
  if (skip_unchanged && !hash) {
    rdz_stop("`skip_unchanged = TRUE` needs `hash = TRUE`: it compares content hashes.",
             call. = FALSE)
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
    rdz_stop("The destination directory does not exist: ", directory, class = "rdz_io_error", call. = FALSE)
  }
  if (dir.exists(path)) {
    rdz_stop("`path` must not refer to a directory.", call. = FALSE)
  }

  rdz_write_to(x, path, mode, section, hash, compress)
  invisible(path)
}

rdz_hash_flag <- function(hash) {
  if (!is.logical(hash) || length(hash) != 1L || is.na(hash)) {
    rdz_stop("`hash` (or `options(rdz.hash)`) must be TRUE or FALSE.", call. = FALSE)
  }
  hash
}

# Writes x to path, or with a NULL path into a raw vector, which it returns:
# natively when it can (and mode allows), else through R serialization.
rdz_write_to <- function(x, path, mode, section, hash = TRUE,
                         compress = getOption("rdz.compress", 1L)) {
  settings <- c(rdz_settings(compress), as.integer(hash))
  if (!identical(mode, "r")) {
    native <- rdz_check(.Call(
      rdz_c_try_write_native, x, path, identical(mode, "native"), rdz_dictionary_policy(),
      settings, section
    ))
    if (is.raw(native)) return(native)
    if (isTRUE(native)) return(invisible(NULL))
  }
  synopsis <- serialize(build_rdz_synopsis(x), connection = NULL, ascii = FALSE, xdr = TRUE,
                        version = 3L)
  # what rdz_info() will accept: if a field were ever ALTREP or other than a
  # plain vector, the minimal synopsis instead of a file its reader refuses
  if (!rdz_synopsis_plain(synopsis)) {
    synopsis <- serialize(rdz_minimal_synopsis(x), connection = NULL, ascii = FALSE,
                          xdr = TRUE, version = 3L)
  }
  rdz_check(.Call(rdz_c_write_generic, x, synopsis, path, settings, section))
}

#' Serialize an R Object to a Raw Vector
#'
#' `rdz_serialize()` gives the bytes [write_rdz()] would write, as a raw
#' vector, for a database column, a key-value store such as Redis, or a
#' socket; `rdz_unserialize()` reads them back. Everything else takes a raw
#' vector where it takes a path: [read_rdz()] (`select` and `rows` included),
#' [rdz_info()], [rdz_schema()], [rdz_attributes()] and [rdz_verify()].
#'
#' @inheritParams write_rdz
#' @param bytes A raw vector holding an rdz file.
#' @param select,rows As for [read_rdz()].
#' @returns `rdz_serialize()`: a raw vector. `rdz_unserialize()`: the object.
#' @examples
#' bytes <- rdz_serialize(mtcars)
#' identical(rdz_unserialize(bytes), mtcars)
#' rdz_unserialize(bytes, select = "mpg", rows = 1:3)
#' @export
rdz_serialize <- function(x, mode = c("auto", "native", "r"), metadata = NULL,
                          compress = getOption("rdz.compress", 1L),
                          hash = getOption("rdz.hash", TRUE)) {
  mode <- match.arg(mode)
  if (missing(compress)) compress <- rdz_legacy_compress(compress)
  rdz_write_to(x, NULL, mode, rdz_metadata_section(metadata), rdz_hash_flag(hash),
               rdz_compress_level(compress))
}

#' @rdname rdz_serialize
#' @export
rdz_unserialize <- function(bytes, select = NULL, rows = NULL) {
  if (!is.raw(bytes)) rdz_stop("`bytes` must be a raw vector.", call. = FALSE)
  read_rdz(bytes, select = select, rows = rows)
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
#' @param path A path, or a raw vector holding an rdz file ([rdz_serialize()]).
#' @param select `NULL` (everything), or the columns of a data frame or the
#'   elements of a list to read: distinct names, or distinct positive
#'   positions.
#' @param rows `NULL` (all), or the rows of a data frame or elements of a
#'   vector to read: positive positions, in any order. From a natively
#'   written file only the blocks covering `range(rows)` are read; each column
#'   is then taken with `[`, so a Date or factor column keeps its class. Stored
#'   row names are taken too; automatic ones stay automatic (`1:length(rows)`).
#'   A matrix, array or time series root is read whole and gives its
#'   elements (`x[rows]`), as for any vector.
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
    rdz_check(.Call(rdz_c_read, path, rdz_settings(0L), index, window)),
    rdz_limit_error = function(e) rdz_rows_refused(path, rows, e),
    rdz_unsupported_error = function(e) rdz_rows_refused(path, rows, e)
  )
  if (is.null(read)) { # a window the reader refuses: the value whole
    window <- NULL
    read <- rdz_check(.Call(rdz_c_read, path, rdz_settings(0L), index, NULL))
  }
  value <- read[[1L]]
  if (!read[[2L]] && !is.null(select)) value <- rdz_select_generic(value, select)
  # a windowed read starts after row window[1]; a whole one at row 1
  if (!is.null(rows)) value <- rdz_rows_take(value, rows, if (read[[2L]] && !is.null(window)) window[[1L]] else 0)
  value
}

# Words the reader's refusal of `rows` for people (the directory says why),
# or, for a root that has rows but a window the reader cannot apply (a
# matrix, a vector shared with a list column), NULL: read it whole.
rdz_rows_refused <- function(path, rows, e) {
  if (is.null(rows)) stop(e)
  root <- rdz_directory(path)$objects[1L, ]
  if (!root$type_name %in% c("data.frame", "logical", "integer", "double", "character",
                             "factor")) {
    rdz_stop("`rows` needs a data frame or a vector; use `select` for a list's elements.",
         call. = FALSE)
  }
  if (length(rows) && max(rows) > root$length) {
    rdz_stop("`rows` must be at most ", root$length, ".", call. = FALSE)
  }
  if (inherits(e, "rdz_unsupported_error")) return(NULL)
  stop(e)
}

rdz_rows_check <- function(rows) {
  if (!is.numeric(rows) || anyNA(rows) || any(rows < 1) || any(rows != trunc(rows))) {
    rdz_stop("`rows` must be positive whole numbers.", call. = FALSE)
  }
  rows
}

# The rows of a value read whole or over a window starting after row `lo`:
# a vector's elements (through `[`, so its class's method applies), or a
# data frame's rows, each column taken the same way. A native read windows
# the row names and the vector columns; a column read whole (a list) is
# longer. Row names: stored ones are taken too; automatic ones stay
# automatic.
rdz_rows_take <- function(value, rows, lo) {
  if (!is.data.frame(value)) {
    if (is.list(value) && !is.object(value)) {
      rdz_stop("`rows` needs a data frame or a vector; use `select` for a list's elements.",
           call. = FALSE)
    }
    if (max(c(rows, 0)) > length(value) + lo) rdz_stop("`rows` must be at most ", length(value) + lo, ".", call. = FALSE)
    return(value[rows - lo])
  }
  n <- .row_names_info(value, 2L) # rows read: the window's, or all of them
  if (max(c(rows, 0)) > n + lo) rdz_stop("`rows` must be at most ", n + lo, ".", call. = FALSE)
  cols <- lapply(unclass(value), function(col) {
    rdz_rows_of(col, if (NROW(col) == n) rows - lo else rows)
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
  # the frame's other attributes (a data.table's selfref among them) are
  # kept whole, as `[.data.frame` keeps them
  extra <- attributes(value)
  extra <- extra[setdiff(names(extra), c("names", "row.names", "class"))]
  for (a in names(extra)) attr(out, a) <- extra[[a]]
  out
}

# Rows `i` of a column, as `[.data.frame` takes them: a matrix's or a data
# frame's rows, anything else's elements (an array of three or more
# dimensions included).
rdz_rows_of <- function(col, i) {
  if (length(dim(col)) == 2L) col[i, , drop = FALSE] else col[i]
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
    rdz_stop("`metadata` must be NULL or a named character vector (or a named list of single ",
         "strings), without NA.", call. = FALSE)
  }
  keys <- enc2utf8(names(metadata))
  if (anyNA(keys) || any(!nzchar(keys)) || anyDuplicated(keys)) {
    rdz_stop("`metadata` names must be distinct and non-empty.", call. = FALSE)
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
  if (length(out) > 64 * 1024) rdz_stop("`metadata` exceeds 64 KiB.", call. = FALSE)
  out
}

# The 0-based children `select` names in a native file's list or data frame
# root, reading only its names.
rdz_select_index <- function(path, info, select) {
  if (!info$root_type %in% c("list", "data.frame")) {
    rdz_stop("`select` needs a list or a data frame; the file holds ",
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
    if (anyNA(select)) rdz_stop("`select` must not contain NA.", call. = FALSE)
    if (is.null(names)) rdz_stop("`select` names parts, but they have no names.", call. = FALSE)
    at <- match(select, names)
    if (anyNA(at)) {
      rdz_stop("Unknown in `select`: ", paste(select[is.na(at)], collapse = ", "), call. = FALSE)
    }
  } else if (is.numeric(select)) {
    if (anyNA(select) || any(select != trunc(select)) || any(select < 1) || any(select > n)) {
      rdz_stop("`select` positions must be whole numbers from 1 to ", n, ".", call. = FALSE)
    }
    at <- as.integer(select)
  } else {
    rdz_stop("`select` must be NULL, a character vector or a numeric vector.", call. = FALSE)
  }
  if (anyDuplicated(at)) rdz_stop("`select` must not repeat a part.", call. = FALSE)
  at
}

# The same selection from a value read whole (a generic file): the parts,
# with their names; a data frame's row names and class.
rdz_select_generic <- function(value, select) {
  if (typeof(value) != "list") {
    rdz_stop("`select` needs a list or a data frame.", call. = FALSE)
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
#' @param path A path, or a raw vector holding an rdz file ([rdz_serialize()]).
#' @returns A list of class `rdz_info`:
#'   * `container_version`, `codec` (`"native_v1"` or `"r_serial_v3"`),
#'     `codec_id`, `codec_version`: the file's format versions and payload
#'     codec.
#'   * `block_size` (the largest decoded block, in bytes), `block_count`,
#'     `object_count`, `attribute_count`: the directory's counts.
#'   * `payload_bytes` (stored block bytes) and `file_bytes`.
#'   * `root_type`, `root_length`, `attribute_names`: the root's type (a native
#'     type such as `"data.frame"`, or for a generic file R's [typeof()]), its
#'     length (a data frame's number of columns) and its attributes' names.
#'   * `writer`: the implementation and version that wrote the file, such as
#'     `"rdz 0.1.0"`, or `""` when the file does not record one.
#'   * `content_hash`: the value's hash ([rdz_hash()]), 32 hexadecimal digits,
#'     or `NA` when the file records none.
#'   * `metadata`: the user metadata [write_rdz()] recorded, a named character
#'     vector (empty when there is none).
#'   * `synopsis`: for a generic file, the bounded description of the root
#'     recorded when it was written (`NULL` for a native file).
#'   * `authoritative`, `exact_attributes`: whether the directory describes
#'     the value exactly (native files) rather than through the synopsis;
#'     `full_read_required_for_attributes` is their opposite.
#'   * `schema`: `root_type`, `length` and `attribute_names` as one list.
#'   * `integrity_checks`: the checks `rdz_info()` makes: the header's and the
#'     directory's checksums and every offset and length in the directory.
#'     Block checksums are checked by [read_rdz()] and [rdz_verify()].
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(data.frame(value = 1:3), path)
#' rdz_info(path)
#' unlink(path)
#' @export
rdz_info <- function(path) {
  path <- validate_existing_rdz_path(path)
  info <- rdz_c_file_info(path)
  synopsis <- if (length(info$synopsis) == 0L) NULL else rdz_synopsis_read(info$synopsis)
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
    rdz_stop("`path` must be a single, non-missing, non-empty string.", call. = FALSE)
  }
  path.expand(path)
}

# A path to an existing file, or a raw vector holding one (rdz_serialize()).
validate_existing_rdz_path <- function(path) {
  if (is.raw(path)) return(path)
  path <- validate_rdz_path(path)
  if (!file.exists(path) || dir.exists(path)) {
    rdz_stop("The file does not exist: ", path, class = "rdz_io_error", call. = FALSE)
  }
  path
}

# A generic file's synopsis, read without trusting it: the stream must hold
# only what build_rdz_synopsis() writes (vectors, strings, symbols, pairlists
# of attributes, NULL), which unserialize() builds without loading a
# namespace or running a hook, and the result must have the synopsis' shape.
# Anything else is a format error.
rdz_synopsis_read <- function(bytes) {
  invalid <- function(why) {
    rdz_check(structure(paste("invalid rdz file: the root synopsis", why),
                        class = "rdz_failure", kind = "format"))
  }
  if (!rdz_synopsis_plain(bytes)) invalid("holds more than plain vectors")
  value <- tryCatch(unserialize(bytes), error = function(e) NULL)
  ok <- is.list(value) && !is.object(value) &&
    is.character(value$root_type) && length(value$root_type) == 1L &&
    (is.null(value$length) || (is.numeric(value$length) && length(value$length) == 1L)) &&
    is.character(value$attribute_names) && is.character(value$class)
  if (!ok) invalid("is not a synopsis")
  value
}

# Whether an R serialization stream (XDR, version 2 or 3) holds only NULL,
# logical, integer, double, character and list vectors, their strings, and
# attribute pairlists with symbol tags (repeated symbols as references).
rdz_synopsis_plain <- function(bytes) {
  n <- length(bytes)
  at <- 0
  int <- function() {
    if (at + 4 > n) rdz_stop("short")
    v <- readBin(bytes[at + 1:4], "integer", size = 4L, endian = "big")
    at <<- at + 4
    v
  }
  skip <- function(k) {
    if (k < 0 || at + k > n) rdz_stop("short")
    at <<- at + k
  }
  symbols <- 0
  item <- function(depth) {
    if (depth > 64L) rdz_stop("deep")
    flags <- int()
    type <- bitwAnd(flags, 0xFF)
    has_attr <- bitwAnd(flags, 0x200) != 0
    has_tag <- bitwAnd(flags, 0x400) != 0
    if (type == 254L) return(invisible()) # NULL
    if (type == 255L) { # a reference: only to a symbol already read
      ref <- bitwShiftR(flags, 8L)
      if (ref == 0L) ref <- int()
      if (ref < 1L || ref > symbols) rdz_stop("reference")
      return(invisible())
    }
    if (type == 1L) { # a symbol: its name, a string
      symbols <<- symbols + 1
      item(depth + 1L)
      return(invisible())
    }
    if (type == 2L) { # a pairlist, iteratively along its tail
      repeat {
        if (has_attr) item(depth + 1L)
        if (has_tag) item(depth + 1L)
        item(depth + 1L)
        flags <- int()
        type <- bitwAnd(flags, 0xFF)
        if (type == 254L) return(invisible())
        if (type != 2L) rdz_stop("type")
        has_attr <- bitwAnd(flags, 0x200) != 0
        has_tag <- bitwAnd(flags, 0x400) != 0
      }
    }
    if (type == 9L) { # a string
      len <- int()
      if (len != -1L) skip(len)
      return(invisible())
    }
    if (!type %in% c(10L, 13L, 14L, 16L, 19L)) rdz_stop("type")
    len <- int()
    if (len == -1L) rdz_stop("long") # a synopsis is never a long vector
    if (len < 0L) rdz_stop("length")
    if (type == 10L || type == 13L) skip(4 * len)
    else if (type == 14L) skip(8 * len)
    else for (k in seq_len(len)) item(depth + 1L)
    if (has_attr) item(depth + 1L)
    invisible()
  }
  tryCatch({
    if (n < 14 || !identical(bytes[1:2], charToRaw("X\n"))) rdz_stop("header")
    at <- 2
    version <- int()
    int()
    int()
    if (version == 3L) skip(int()) # the native encoding's name
    else if (version != 2L) rdz_stop("version")
    item(0L)
    at == n
  }, error = function(e) FALSE)
}

build_rdz_synopsis <- function(x) {
  tryCatch(build_rdz_synopsis_impl(x), error = function(error) rdz_minimal_synopsis(x))
}

rdz_minimal_synopsis <- function(x) {
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
