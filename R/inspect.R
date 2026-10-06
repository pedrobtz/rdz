# Inspection of native files below the root (metadata-access.md): the
# directory as tables, objects found by path, attributes and schemas read
# from the directory and the blocks of the objects asked for, never the data
# of the rest.

rdz_type_names <- c("NULL", "logical", "integer", "double", "character", "factor", "list",
                    "data.frame")

# The directory of a native file: objects and attributes, as data frames.
rdz_directory <- function(path) {
  d <- rdz_check(.Call(rdz_c_directory, path))
  objects <- as.data.frame(d$objects)
  objects$type_name <- rdz_type_names[objects$type + 1L]
  list(objects = objects, attributes = as.data.frame(d$attributes, stringsAsFactors = FALSE))
}

# Objects (0-based ids) read alone, each with everything below it.
rdz_read_objects <- function(path, ids) {
  if (!length(ids)) return(list())
  rdz_check(.Call(rdz_c_read_objects, path, as.integer(ids), rdz_settings()))
}

# The id of the object at `object`, a path from the root like a `[[` index:
# names or positions, one per level; NULL or 0 is the root.
rdz_object_id <- function(path, dir, object) {
  if (is.null(object) || (length(object) == 1L && is.numeric(object) && object == 0)) {
    return(0L)
  }
  steps <- as.list(object)
  ok <- vapply(steps, function(s) {
    length(s) == 1L && !is.na(s) && (is.character(s) || (is.numeric(s) && s >= 1 && s == trunc(s)))
  }, logical(1L))
  if (!length(steps) || !all(ok)) {
    stop("`object` must be NULL, 0, or a path of names and positive positions.", call. = FALSE)
  }
  objects <- dir$objects
  attrs <- dir$attributes
  id <- 0L
  where <- ""
  for (s in steps) {
    o <- objects[id + 1L, ]
    if (!o$type_name %in% c("list", "data.frame")) {
      stop("`object` goes below ", if (nzchar(where)) where else "the root",
           ", which is not a list or a data frame.", call. = FALSE)
    }
    if (is.character(s)) {
      names_id <- attrs$value_object[attrs$owner == id & attrs$kind == 1L]
      names <- if (length(names_id)) rdz_read_objects(path, names_id)[[1L]]
      at <- match(s, names)
      if (is.na(at)) stop("No `", s, "` in ", if (nzchar(where)) where else "the root", ".",
                          call. = FALSE)
      where <- paste0(where, "$", s)
    } else {
      at <- as.integer(s)
      if (at > o$child_count) {
        stop("Position ", at, " is past the ", o$child_count, " parts of ",
             if (nzchar(where)) where else "the root", ".", call. = FALSE)
      }
      where <- paste0(where, "[[", at, "]]")
    }
    id <- o$first_child + at - 1L
  }
  id
}

# An object's attributes as R lists them, each with the object holding its
# value or, for those the codecs imply, the value itself.
rdz_attribute_entries <- function(dir, id) {
  o <- dir$objects[id + 1L, ]
  a <- dir$attributes[dir$attributes$owner == id, , drop = FALSE]
  entries <- list()
  add <- function(name, object = NA_integer_, value = NULL) {
    entries[[name]] <<- list(object = object, value = value)
  }
  if (o$type_name == "factor") {
    add("levels", object = o$first_child)
    add("class", value = if (bitwAnd(o$flags, 1L)) c("ordered", "factor") else "factor")
  } else if (o$type_name == "data.frame") {
    add("names", object = a$value_object[a$kind == 1L])
    if (any(a$kind == 2L)) add("row.names", object = a$value_object[a$kind == 2L])
    else add("row.names", value = seq_len(o$length))
    if (any(a$kind == 4L)) add("class", object = a$value_object[a$kind == 4L])
    else add("class", value = "data.frame")
  }
  for (k in seq_len(nrow(a))) {
    if (o$type_name == "data.frame" && a$kind[k] != 8L) next # listed above
    add(a$name[k], object = a$value_object[k])
  }
  entries
}

#' Inspect an rdz Object Schema
#'
#' `rdz_schema()` reads the bounded directory without reading object data
#' blocks. Native schemas are authoritative; generic schemas are bounded
#' synopses and are marked non-authoritative.
#'
#' For a native file, `objects` describes the stored object as a tree: one row
#' per part (the root, each list element and data frame column, recursively),
#' with its path from the root, type, class, length, attribute names and the
#' stored bytes of everything below it. Only the directory and the parts'
#' names and classes are read, never their data.
#'
#' @param path A single, non-missing path to inspect.
#' @param recursive Whether `objects` describes every level (`TRUE`) or the
#'   root and its own parts.
#' @returns A named schema list.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(list(a = 1:3, b = data.frame(d = Sys.Date() + 0:1, x = c(1.5, 2))), path)
#' rdz_schema(path)
#' unlink(path)
#' @export
rdz_schema <- function(path, recursive = TRUE) {
  path <- validate_existing_rdz_path(path)
  if (!is.logical(recursive) || length(recursive) != 1L || is.na(recursive)) {
    stop("`recursive` must be TRUE or FALSE.", call. = FALSE)
  }
  info <- rdz_info(path)
  out <- c(
    list(
      codec = info$codec,
      authoritative = info$authoritative,
      exact_attributes = info$exact_attributes
    ),
    info$schema,
    list(data_blocks_read = FALSE)
  )
  if (identical(info$codec, "native_v1")) out$objects <- rdz_schema_objects(path, recursive)
  structure(out, class = "rdz_schema")
}

rdz_schema_objects <- function(path, recursive) {
  dir <- rdz_directory(path)
  objects <- dir$objects
  attrs <- dir$attributes
  n <- nrow(objects)
  # stored bytes of each object with everything below it (parents come first)
  total <- objects$stored_bytes
  for (i in rev(seq_len(n))[-n]) {
    p <- objects$parent[i] + 1L
    total[p] <- total[p] + total[i]
  }
  # the parts: the root and the children, level by level
  depth <- integer(n)
  part <- objects$role %in% c(0L, 4L)
  for (i in seq_len(n)[-1L]) depth[i] <- depth[objects$parent[i] + 1L] + 1L
  keep <- which(part & (recursive | depth <= 1L))
  # depth first, as a tree reads: each part followed by everything below it
  kids <- split(keep[-1L], objects$parent[keep[-1L]])
  order <- integer()
  stack <- 1L
  while (length(stack)) {
    i <- stack[[1L]]
    stack <- c(kids[[as.character(i - 1L)]], stack[-1L])
    order <- c(order, i)
  }
  keep <- order
  # the containers' names and the parts' classes: small objects, read at once
  containers <- keep[objects$type_name[keep] %in% c("list", "data.frame")]
  names_at <- vapply(containers - 1L, function(id) {
    v <- attrs$value_object[attrs$owner == id & attrs$kind == 1L]
    if (length(v)) v else NA_integer_
  }, integer(1L))
  class_at <- vapply(keep - 1L, function(id) {
    v <- attrs$value_object[attrs$owner == id & (attrs$kind == 4L | (attrs$kind == 8L & attrs$name == "class"))]
    if (length(v)) v else NA_integer_
  }, integer(1L))
  dim_at <- vapply(keep - 1L, function(id) {
    v <- attrs$value_object[attrs$owner == id & attrs$kind == 8L & attrs$name == "dim"]
    if (length(v)) v else NA_integer_
  }, integer(1L))
  want <- unique(c(names_at[!is.na(names_at)], class_at[!is.na(class_at)],
                   dim_at[!is.na(dim_at)]))
  read <- stats::setNames(rdz_read_objects(path, want), want)
  child_names <- list()
  for (k in seq_along(containers)) {
    if (!is.na(names_at[k])) child_names[[as.character(containers[k] - 1L)]] <- read[[as.character(names_at[k])]]
  }
  path_of <- character(n)
  for (i in keep) {
    if (i == 1L) next
    p <- objects$parent[i]
    pos <- i - 1L - objects$first_child[p + 1L] + 1L
    nm <- child_names[[as.character(p)]]
    step <- if (!is.null(nm) && !is.na(nm[pos]) && nzchar(nm[pos])) paste0("$", nm[pos]) else paste0("[[", pos, "]]")
    path_of[i] <- paste0(path_of[p + 1L], step)
  }
  class_of <- vapply(seq_along(keep), function(k) {
    i <- keep[k]
    if (!is.na(class_at[k])) return(paste(read[[as.character(class_at[k])]], collapse = "/"))
    if (!is.na(dim_at[k])) {
      return(if (length(read[[as.character(dim_at[k])]]) == 2L) "matrix/array" else "array")
    }
    switch(objects$type_name[i],
      factor = if (bitwAnd(objects$flags[i], 1L)) "ordered/factor" else "factor",
      data.frame = "data.frame",
      double = "numeric",
      objects$type_name[i]
    )
  }, character(1L))
  attr_names <- vapply(keep - 1L, function(id) {
    paste(names(rdz_attribute_entries(dir, id)), collapse = ", ")
  }, character(1L))
  shape <- vapply(seq_along(keep), function(k) {
    i <- keep[k]
    if (objects$type_name[i] == "data.frame") {
      return(paste(objects$length[i], "x", objects$child_count[i]))
    }
    if (!is.na(dim_at[k])) return(paste(read[[as.character(dim_at[k])]], collapse = " x "))
    format(if (objects$type_name[i] == "list") objects$child_count[i] else objects$length[i])
  }, character(1L))
  data.frame(
    path = path_of[keep],
    depth = depth[keep],
    type = objects$type_name[keep],
    class = class_of,
    length = ifelse(objects$type_name[keep] == "list", objects$child_count[keep], objects$length[keep]),
    columns = ifelse(objects$type_name[keep] == "data.frame", objects$child_count[keep], NA_integer_),
    shape = shape,
    attributes = attr_names,
    stored_bytes = total[keep],
    id = keep - 1L,
    stringsAsFactors = FALSE
  )
}

#' @export
print.rdz_schema <- function(x, ...) {
  cat("<rdz_schema> ", x$codec, if (isTRUE(x$authoritative)) "" else " (synopsis)", "\n", sep = "")
  if (is.null(x$objects)) {
    cat("  root: ", x$root_type, ", length ", x$length, "\n", sep = "")
    return(invisible(x))
  }
  o <- x$objects
  for (i in seq_len(nrow(o))) {
    label <- if (o$depth[i] == 0L) "<root>" else sub("^.*(\\$[^$\\[]*|\\[\\[[0-9]+\\]\\])$", "\\1", o$path[i])
    shape <- o$shape[i]
    attrs <- if (nzchar(o$attributes[i])) paste0("  [", o$attributes[i], "]") else ""
    cat(strrep("  ", o$depth[i] + 1L), label, ": ", o$class[i], " ", shape,
        "  (", format(o$stored_bytes[i], big.mark = ","), " B)", attrs, "\n", sep = "")
  }
  invisible(x)
}

#' Read rdz Object Attributes
#'
#' Native attributes are independently addressable: `rdz_attributes()` reads
#' the directory and the requested attributes' own blocks, for the root or for
#' any part below it, never the data of the object that holds them. Generic
#' files require `allow_full = TRUE`, which explicitly permits a complete
#' object read.
#'
#' @param path A single, non-missing path to inspect.
#' @param object The object whose attributes to read: `NULL` or `0` for the
#'   root, or a path from the root as for `[[`, one name or position per
#'   level, such as `c("sales", "date")` or `list("models", 2)`.
#' @param names Optional character vector selecting attribute names.
#' @param allow_full Whether generic files may be fully deserialized.
#' @returns A named list of attribute values.
#' @examples
#' path <- tempfile(fileext = ".rdz")
#' write_rdz(list(when = as.POSIXct("2026-10-06 12:00", tz = "UTC")), path)
#' rdz_attributes(path, object = "when")
#' unlink(path)
#' @export
rdz_attributes <- function(path, object = NULL, names = NULL, allow_full = FALSE) {
  path <- validate_existing_rdz_path(path)
  if (!is.null(names) &&
      (!is.character(names) || anyNA(names) || any(!nzchar(names)))) {
    stop("`names` must be NULL or a character vector of non-empty names.", call. = FALSE)
  }
  if (!is.logical(allow_full) || length(allow_full) != 1L || is.na(allow_full)) {
    stop("`allow_full` must be TRUE or FALSE.", call. = FALSE)
  }
  info <- rdz_info(path)
  if (identical(info$codec, "native_v1")) {
    dir <- rdz_directory(path)
    id <- rdz_object_id(path, dir, object)
    entries <- rdz_attribute_entries(dir, id)
    requested <- if (is.null(names)) base::names(entries) else names
    unknown <- setdiff(requested, base::names(entries))
    if (length(unknown)) {
      stop("Unknown attribute: ", paste(unknown, collapse = ", "), call. = FALSE)
    }
    entries <- entries[requested]
    stored <- vapply(entries, function(e) e$object, integer(1L))
    read <- rdz_read_objects(path, stored[!is.na(stored)])
    values <- lapply(entries, function(e) e$value)
    values[!is.na(stored)] <- read
    return(stats::setNames(values, requested))
  }
  if (!allow_full) {
    stop(
      "Exact generic attributes require a full read; set `allow_full = TRUE`.",
      call. = FALSE
    )
  }
  value <- read_rdz(path)
  if (!is.null(object) && !(length(object) == 1L && is.numeric(object) && object == 0)) {
    for (s in as.list(object)) value <- value[[s]]
  }
  values <- attributes(value)
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
