# The block pipeline (plan-c Stage D): equal-budget thread scaling against
# qs2 on large vectors, the presets, and small objects. Run against an
# installed rdz:
#
#   Rscript tools/bench-pipeline.R
#
# Medians of RDZ_BENCH_ITERATIONS (default 5) elapsed times. rdz writes through the generic
# codec (mode "r"), the path every object takes until its native codec lands.

library(rdz)
stopifnot(requireNamespace("qs2"))
iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "5"))
dir <- tempfile("rdz-bench-")
dir.create(dir)
on.exit(unlink(dir, recursive = TRUE), add = TRUE)

# A collection before each run, so that the garbage of building the data is
# not charged to whichever run triggers it.
median_ms <- function(expr_fn) {
  times <- vapply(seq_len(iterations), function(i) {
    gc(verbose = FALSE)
    system.time(expr_fn())[["elapsed"]]
  }, numeric(1L))
  round(stats::median(times) * 1000, 1)
}

rdz_case <- function(x, threads, preset = "balanced") {
  p <- file.path(dir, "x.rdz")
  old <- options(rdz.threads = threads, rdz.preset = preset)
  on.exit(options(old), add = TRUE)
  w <- median_ms(function() write_rdz(x, p, mode = "r"))
  r <- median_ms(function() read_rdz(p))
  c(write_ms = w, read_ms = r, file_mb = round(file.size(p) / 1024^2, 2))
}

qs2_case <- function(x, threads) {
  p <- file.path(dir, "x.qs2")
  w <- median_ms(function() qs2::qs_save(x, p, nthreads = threads))
  r <- median_ms(function() qs2::qs_read(p, validate_checksum = TRUE, nthreads = threads))
  c(write_ms = w, read_ms = r, file_mb = round(file.size(p) / 1024^2, 2))
}

set.seed(42)
large <- list(
  doubles = round(cumsum(rnorm(2e7)), 2),       # 160 MB, compressible
  integers = sample.int(1000L, 4e7, TRUE),       # 160 MB
  strings = sprintf("item-%06d", sample.int(1e5, 5e6, TRUE))
)

rows <- list()
for (name in names(large)) {
  for (threads in c(1L, 2L, 4L, 8L)) {
    rows[[length(rows) + 1L]] <- c(data = name, format = "rdz", threads = threads,
                                   rdz_case(large[[name]], threads))
    rows[[length(rows) + 1L]] <- c(data = name, format = "qs2", threads = threads,
                                   qs2_case(large[[name]], threads))
  }
}
cat("Equal-budget scaling (balanced preset):\n")
print(as.data.frame(do.call(rbind, rows)), row.names = FALSE)

rows <- list()
for (preset in c("speed", "balanced", "compact")) {
  rows[[length(rows) + 1L]] <- c(preset = preset, rdz_case(large$doubles, 1L, preset))
}
cat("\nPresets, one thread, the doubles:\n")
print(as.data.frame(do.call(rbind, rows)), row.names = FALSE)

small <- list(n1e3 = runif(1e3), n1e5 = runif(1e5), list = as.list(1:100))
rows <- list()
for (name in names(small)) {
  for (setting in list(c("speed", 1L), c("balanced", 1L), c("balanced", 8L))) {
    rows[[length(rows) + 1L]] <- c(data = name, preset = setting[[1L]], threads = setting[[2L]],
                                   rdz_case(small[[name]], as.integer(setting[[2L]]), setting[[1L]]))
  }
}
cat("\nSmall objects:\n")
print(as.data.frame(do.call(rbind, rows)), row.names = FALSE)
cat("\n", R.version.string, " ", Sys.info()[["machine"]], ", ", parallel::detectCores(),
    " cores; qs2 ", utils::packageDescription("qs2")$Version, "\n", sep = "")
