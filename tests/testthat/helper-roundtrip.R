roundtrip_rdz <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(x, path)
  read_rdz(path)
}
