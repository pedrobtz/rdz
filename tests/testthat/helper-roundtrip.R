roundtrip_rdz <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(x, path)
  read_rdz(path)
}

# Worker threads for a test: as asked off CRAN, at most two on CRAN (its
# policy caps a check at two cores).
test_threads <- function(n) {
  if (identical(Sys.getenv("NOT_CRAN"), "true")) n else pmin(n, 2L)
}
