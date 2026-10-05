# The native integer and double codecs (plan-c Stage F) on the expanded
# benchmark suite's distributions: native against the generic codec (the
# Stage D path) and qs2, at one thread and eight. Median of
# RDZ_BENCH_ITERATIONS (default 5) elapsed times after a gc().
#
#   Rscript tools/bench-numeric.R

library(rdz)
stopifnot(requireNamespace("qs2"))
options(width = 200)
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
  round(stats::median(t) * 1000)
}

set.seed(20261005)
materialize <- function(x) { x[1L] <- x[1L]; x } # not ALTREP
integer_value <- sample.int(1000000L, size, replace = TRUE)
numeric_value <- stats::rnorm(size)
data <- list(
  int_random = integer_value,
  int_full_range = as.integer(floor(stats::runif(size, -.Machine$integer.max, .Machine$integer.max))),
  int_sequential = materialize(seq_len(size)),
  int_low_cardinality = sample(c(-1L, 0L, 1L, NA_integer_), size, replace = TRUE),
  int_missing = replace(integer_value, seq.int(1L, size, by = 20L), NA_integer_),
  int_all_missing = rep(NA_integer_, size),
  dbl_random = numeric_value,
  dbl_rounded = round(numeric_value, 2L),
  dbl_currency = round(stats::runif(size, -100000, 100000), 2L),
  dbl_monotonic = cumsum(stats::runif(size)),
  dbl_repeated = sample(c(-1, 0, 1, pi, NA_real_), size, replace = TRUE),
  dbl_all_missing = rep(NA_real_, size)
)

case <- function(x, threads, mode) {
  p <- file.path(dir, "x.rdz")
  old <- options(rdz.threads = threads)
  on.exit(options(old))
  w <- ms(function() write_rdz(x, p, mode = mode))
  r <- ms(function() read_rdz(p))
  stopifnot(identical(read_rdz(p), x))
  c(w, r, round(file.size(p) / 1024^2, 2))
}

qs <- function(x, threads) {
  p <- file.path(dir, "x.qs2")
  w <- ms(function() qs2::qs_save(x, p, nthreads = threads))
  r <- ms(function() qs2::qs_read(p, validate_checksum = TRUE, nthreads = threads))
  c(w, r, round(file.size(p) / 1024^2, 2))
}

rows <- list()
for (name in names(data)) {
  x <- data[[name]]
  for (threads in c(1L, 8L)) {
    native <- case(x, threads, "native")
    generic <- case(x, threads, "r")
    q <- qs(x, threads)
    rows[[length(rows) + 1L]] <- data.frame(
      data = name, threads = threads,
      native_w = native[[1]], native_r = native[[2]], native_mb = native[[3]],
      generic_w = generic[[1]], generic_r = generic[[2]], generic_mb = generic[[3]],
      qs2_w = q[[1]], qs2_r = q[[2]], qs2_mb = q[[3]]
    )
  }
}
print(do.call(rbind, rows), row.names = FALSE)
cat("\n", size, " values, ms and MB; ", R.version.string, " ", Sys.info()[["machine"]], ", ",
    parallel::detectCores(), " cores; qs2 ", utils::packageDescription("qs2")$Version, "\n",
    sep = "")
