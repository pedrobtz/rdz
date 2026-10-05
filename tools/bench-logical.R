# The native logical codec (plan-c Stage E) on roadmap Phase 1's six
# distributions: the C implementation against the Rust one it replaces
# (identical bytes required), with qs2 for reference. One thread, median of
# RDZ_BENCH_ITERATIONS (default 5) elapsed times after a gc(). Needs an rdz
# built with the Rust reference.
#
#   Rscript tools/bench-logical.R

library(rdz)
options(width = 200)
stopifnot(rdz:::rdz_has_rust(), requireNamespace("qs2"))
iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "5"))
size <- as.integer(Sys.getenv("RDZ_BENCH_SIZE", "10000000"))
dir <- tempfile("rdz-bench-")
dir.create(dir)
on.exit(unlink(dir, recursive = TRUE), add = TRUE)

ms <- function(f) {
  t <- vapply(seq_len(iterations), function(i) {
    gc(verbose = FALSE)
    system.time(f())[["elapsed"]]
  }, numeric(1L))
  round(stats::median(t) * 1000, 1)
}

set.seed(20261005)
data <- list(
  random = sample(c(FALSE, TRUE, NA), size, replace = TRUE),
  sparse = sample(c(FALSE, TRUE, NA), size, replace = TRUE, prob = c(0.985, 0.01, 0.005)),
  mostly_true = sample(c(FALSE, TRUE, NA), size, replace = TRUE, prob = c(0.01, 0.985, 0.005)),
  mostly_missing = sample(c(FALSE, TRUE, NA), size, replace = TRUE, prob = c(0.01, 0.01, 0.98)),
  runs = rep(c(FALSE, TRUE, NA), each = ceiling(size / 3))[seq_len(size)],
  alternating = rep(c(FALSE, TRUE, NA), length.out = size)
)

rows <- list()
for (name in names(data)) {
  x <- data[[name]]
  c_path <- file.path(dir, "c.rdz")
  rust_path <- file.path(dir, "rust.rdz")
  qs_path <- file.path(dir, "x.qs2")
  c_write <- ms(function() write_rdz(x, c_path, mode = "native"))
  rust_write <- ms(function() rdz:::rdz_try_write_native(x, rust_path, TRUE))
  stopifnot(identical(readBin(c_path, "raw", file.size(c_path)),
                      readBin(rust_path, "raw", file.size(rust_path))))
  c_read <- ms(function() read_rdz(c_path))
  rust_read <- ms(function() rdz:::rdz_read(rust_path)$value)
  stopifnot(identical(read_rdz(c_path), x))
  qs_write <- ms(function() qs2::qs_save(x, qs_path, nthreads = 1L))
  qs_read <- ms(function() qs2::qs_read(qs_path, validate_checksum = TRUE, nthreads = 1L))
  rows[[name]] <- data.frame(
    data = name,
    c_write_ms = c_write, rust_write_ms = rust_write, qs2_write_ms = qs_write,
    c_read_ms = c_read, rust_read_ms = rust_read, qs2_read_ms = qs_read,
    rdz_mb = round(file.size(c_path) / 1024^2, 2),
    qs2_mb = round(file.size(qs_path) / 1024^2, 2)
  )
}
print(do.call(rbind, rows), row.names = FALSE)
cat("\n", size, " values; kernel ", .Call(rdz:::rdz_c_logical_kernel, NA), "; ",
    R.version.string, " ", Sys.info()[["machine"]], "; qs2 ",
    utils::packageDescription("qs2")$Version, "\n", sep = "")
