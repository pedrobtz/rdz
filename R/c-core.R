# The C implementation's entry points (src/rdz_r.c). A C failure comes back
# as an "rdz_failure" string; rdz_check() raises it as a classed condition
# inheriting rdz_error:
#
#   rdz_format_error   the file is not a valid rdz container
#   rdz_limit_error    a count or length exceeds the format's limit
#   rdz_version_error  an unsupported container version
#   rdz_codec_error    an unsupported payload codec
#   rdz_io_error       the operating system refused a read or write
#   rdz_memory_error   an allocation failed

rdz_check <- function(result) {
  if (inherits(result, "rdz_failure")) {
    class <- switch(attr(result, "kind"),
      limit = "rdz_limit_error",
      version = "rdz_version_error",
      codec = "rdz_codec_error",
      io = "rdz_io_error",
      memory = "rdz_memory_error",
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

# Whether this build has the Rust reference implementation, which writing
# and reading need until the C port replaces them.
rdz_has_rust <- function() {
  .Call(rdz_c_has_rust)
}
