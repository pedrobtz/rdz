# Several named objects in one file, as save() and load() keep them, and as
# npz, JLD2 and HDF5 groups do: the root is a named list, and loading some
# reads only theirs (read_rdz(select =)).

#' Save and Load Named Objects
#'
#' `rdz_save()` writes named objects into one rdz file, as [save()] does;
#' `rdz_load()` assigns them back into an environment, all or only those
#' named. Loading some reads only their parts of the file. The file is an
#' ordinary rdz file holding a named list: [read_rdz()] reads it whole, and
#' [rdz_schema()] lists what it holds.
#'
#' @param ... Objects to save, as names or symbols (as for [save()]).
#' @param list A character vector naming further objects to save.
#' @param file A path to write or read (or, for `rdz_load()`, a raw vector
#'   from [rdz_serialize()]).
#' @param envir For `rdz_save()`, where to find the objects; for
#'   `rdz_load()`, where to assign them.
#' @param mode,skip_unchanged,metadata,compress,hash As for [write_rdz()].
#' @param names `NULL` (everything) or the names of the objects to load.
#' @returns `rdz_save()`: `file`, invisibly. `rdz_load()`: the names of the
#'   objects assigned, invisibly.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' a <- 1:3
#' b <- mtcars
#' rdz_save(a, b, file = path)
#' rm(a, b)
#' rdz_load(path, names = "b")
#' exists("b")
#' exists("a")
#' unlink(path)
#' @export
rdz_save <- function(..., list = character(), file, envir = parent.frame(),
                     mode = c("auto", "native", "r"), skip_unchanged = FALSE, metadata = NULL,
                     compress = getOption("rdz.compress", 1L), hash = getOption("rdz.hash", TRUE)) {
  dots <- as.list(substitute(list(...)))[-1L]
  named <- vapply(dots, function(d) {
    if (is.symbol(d)) return(as.character(d))
    if (is.character(d) && length(d) == 1L) return(d)
    rdz_stop("`...` must name objects, as symbols or strings.", call. = FALSE)
  }, "")
  if (!is.character(list) || anyNA(list)) {
    rdz_stop("`list` must be a character vector of object names.", call. = FALSE)
  }
  names <- c(named, list)
  if (!length(names)) rdz_stop("Nothing to save: name objects in `...` or `list`.", call. = FALSE)
  if (anyDuplicated(names)) rdz_stop("Each object can be saved once.", call. = FALSE)
  missing <- names[!vapply(names, exists, TRUE, envir = envir)]
  if (length(missing)) {
    rdz_stop("Object not found: ", paste(missing, collapse = ", "), call. = FALSE)
  }
  write_rdz(mget(names, envir = envir, inherits = TRUE), file, mode = match.arg(mode),
            skip_unchanged = skip_unchanged, metadata = metadata, compress = compress,
            hash = hash)
}

#' @rdname rdz_save
#' @export
rdz_load <- function(file, names = NULL, envir = parent.frame()) {
  if (!is.environment(envir)) rdz_stop("`envir` must be an environment.", call. = FALSE)
  if (!is.null(names) && (!is.character(names) || anyNA(names))) {
    rdz_stop("`names` must be NULL or a character vector.", call. = FALSE)
  }
  info <- rdz_info(file)
  if (identical(info$codec, "native_v1") && !identical(info$root_type, "list")) {
    rdz_stop("The file holds one object (of type ", info$root_type, "), not saved objects; ",
         "read it with read_rdz().", call. = FALSE)
  }
  values <- if (is.null(names)) read_rdz(file) else read_rdz(file, select = names)
  if (!is.list(values) || is.object(values) ||
      (length(values) && (is.null(base::names(values)) || any(!nzchar(base::names(values)))))) {
    rdz_stop("The file does not hold saved objects (a list naming each).", call. = FALSE)
  }
  list2env(values, envir = envir)
  invisible(base::names(values))
}
