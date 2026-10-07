#' Errors rdz Raises
#'
#' Every error rdz raises is a condition of class `rdz_error`, and of one of
#' these classes, so `tryCatch(..., rdz_error = )` catches them all and a
#' handler can tell them apart:
#'
#' * `rdz_argument_error`: an argument is not what the function takes (a
#'   path that is not a string, `rows` that are not positive whole numbers,
#'   an unknown `select` name, an invalid option).
#' * `rdz_format_error`: the file is not a valid rdz file: a checksum does not
#'   match, a length or offset is out of bounds, a record is malformed, or
#'   the value does not match its content hash.
#' * `rdz_limit_error`: a count or length exceeds the format's limits.
#' * `rdz_version_error`: the file uses a container version this rdz does not
#'   read.
#' * `rdz_codec_error`: the file's payload codec is not one this rdz reads.
#' * `rdz_io_error`: the operating system refused a read or a write.
#' * `rdz_memory_error`: an allocation failed.
#' * `rdz_unsupported_error`: `mode = "native"` was given a value the native
#'   codecs do not take (see [write_rdz()], "Native and generic").
#'
#' Tests and code should test the class, never the message.
#'
#' @name rdz-errors
#' @aliases rdz_error
#' @keywords internal
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' writeBin(as.raw(1:10), path)
#' tryCatch(read_rdz(path), rdz_error = function(e) class(e)[[1L]])
#' unlink(path)
NULL

# The C implementation's entry points (src/rdz_r.c). A C failure comes back
# as an "rdz_failure" string; rdz_check() raises it as a classed condition
# inheriting rdz_error (?rdz-errors). R's own argument checks raise
# rdz_argument_error through rdz_stop().

rdz_stop <- function(..., class = "rdz_argument_error", call. = FALSE) {
  stop(structure(
    class = c(class, "rdz_error", "error", "condition"),
    list(message = paste0(...), call = NULL)
  ))
}

# A request the file cannot serve (rdz_request() in src/rdz_r.c), worded as
# R's own argument checks word it: an rdz_argument_error.
rdz_request_stop <- function(result) {
  data <- attr(result, "data")
  switch(as.character(result),
    select_root = rdz_stop("`select` needs a list or a data frame; the file holds ",
                           if (nzchar(data)) data else "neither", ".", call. = FALSE),
    select_type = rdz_stop("`select` must be NULL, a character vector or a numeric vector.",
                           call. = FALSE),
    select_range = rdz_stop("`select` positions must be whole numbers from 1 to ", data, ".",
                            call. = FALSE),
    select_na = rdz_stop("`select` must not contain NA.", call. = FALSE),
    select_no_names = rdz_stop("`select` names parts, but they have no names.", call. = FALSE),
    select_unknown = rdz_stop("Unknown in `select`: ", paste(data, collapse = ", "),
                              call. = FALSE),
    select_repeat = rdz_stop("`select` must not repeat a part.", call. = FALSE),
    rows_root = rdz_stop(
      "`rows` needs a data frame or a vector; use `select` for a list's elements.",
      call. = FALSE
    ),
    rows_range = rdz_stop("`rows` must be at most ", data, ".", call. = FALSE),
    rdz_stop("rdz cannot serve this request.", call. = FALSE)
  )
}

rdz_check <- function(result) {
  if (inherits(result, "rdz_failure")) {
    if (identical(attr(result, "kind"), "request")) rdz_request_stop(result)
    class <- switch(attr(result, "kind"),
      limit = "rdz_limit_error",
      version = "rdz_version_error",
      codec = "rdz_codec_error",
      io = "rdz_io_error",
      memory = "rdz_memory_error",
      unsupported = "rdz_unsupported_error",
      "rdz_format_error"
    )
    stop(structure(
      class = c(class, "rdz_error", "error", "condition"),
      list(message = as.character(result), call = NULL)
    ))
  }
  result
}

rdz_c_file_info <- function(path) {
  rdz_check(.Call(rdz_c_info, path))
}

# The settings the C writer and reader take: c(zstd level, threads, block
# size). Options, documented in ?write_rdz:
#
#   rdz.compress  the zstd level of each block, 0 (none) to 19; default 1
#   rdz.threads   worker threads for compression and decompression (default 1)
#
# rdz.block_size (bytes, default 1 MiB) exists for tests that need many small
# blocks; it is not part of the interface.
rdz_settings <- function(compress = getOption("rdz.compress", 1L)) {
  compress <- rdz_compress_level(compress)
  threads <- getOption("rdz.threads", 1L)
  if (!is.numeric(threads) || length(threads) != 1L || is.na(threads) ||
        threads < 1 || threads > 256 || threads != floor(threads)) {
    rdz_stop("`options(rdz.threads)` must be a whole number from 1 to 256.", call. = FALSE)
  }
  block_size <- getOption("rdz.block_size", 0L)
  c(compress, as.integer(threads), as.integer(block_size))
}

# The level a call that does not give `compress` uses when the session set
# the option that #44 replaced, options(rdz.preset): its preset's level, with
# a deprecation warning once a session (rdz.compress, when set, wins).
rdz_state <- new.env(parent = emptyenv())

rdz_legacy_compress <- function(compress) {
  preset <- getOption("rdz.preset")
  if (is.null(preset) || !is.null(getOption("rdz.compress"))) return(compress)
  levels <- c(speed = 0L, balanced = 1L, compact = 6L)
  if (!is.character(preset) || length(preset) != 1L || !preset %in% names(levels)) {
    rdz_stop('`options(rdz.preset)` is replaced by `options(rdz.compress)`, a level from ',
             '0 to 19 (it was "speed", "balanced" or "compact").', call. = FALSE)
  }
  if (!isTRUE(rdz_state$preset_warned)) {
    rdz_state$preset_warned <- TRUE
    warning(structure(
      class = c("rdz_deprecated_warning", "deprecatedWarning", "warning", "condition"),
      list(message = sprintf(paste0("`options(rdz.preset = \"%s\")` is deprecated: use ",
                                    "`options(rdz.compress = %d)` or `compress = %d`."),
                             preset, levels[[preset]], levels[[preset]]),
           call = NULL)
    ))
  }
  levels[[preset]]
}

# A zstd level, 0 to 19 (20 to 22 need hundreds of MB a thread).
rdz_compress_level <- function(compress) {
  if (!is.numeric(compress) || length(compress) != 1L || is.na(compress) ||
        compress < 0 || compress > 19 || compress != floor(compress)) {
    rdz_stop("`compress` (or `options(rdz.compress)`) must be a whole number from 0 to 19.",
             call. = FALSE)
  }
  as.integer(compress)
}

# zstd's version as compiled into rdz.
rdz_zstd_version <- function() {
  .Call(rdz_c_zstd_version)
}

# RDZ_STRING_DICT selects how native character values store repeats:
# "auto" (default: a dictionary when a sample suggests under 75% distinct
# values), "plain", "block" or "global". An experiment knob, not interface.
rdz_dictionary_policy <- function() {
  switch(Sys.getenv("RDZ_STRING_DICT", "auto"),
    plain = 0L,
    block = 1L,
    global = 2L,
    3L
  )
}
