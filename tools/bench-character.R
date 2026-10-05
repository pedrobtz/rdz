# Native character vectors and factors (plan-c Stage G) on the expanded
# benchmark suite's distributions: native against the generic codec (the
# Stage D path) and qs2, at one thread and eight. Median of
# RDZ_BENCH_ITERATIONS (default 5) elapsed times after a gc().
#
#   Rscript tools/bench-character.R

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
size <- as.integer(Sys.getenv("RDZ_BENCH_SIZE", "2000000"))
latin1 <- iconv("\u00c4rende", from = "UTF-8", to = "latin1")
utf8 <- enc2utf8("\u65e5\u672c\u8a9e")
bytes <- rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9)))
Encoding(bytes) <- "bytes"
high <- sprintf("id-%010d", seq_len(size))
data <- list(
  chr_high_cardinality = high,
  chr_low_cardinality = sample(sprintf("category-%02d", 1:40), size, replace = TRUE),
  chr_repeated_long = sample(c(strrep("alpha", 40L), strrep("beta", 50L), NA), size, replace = TRUE),
  chr_unique_long = paste0(high, "-", strrep("x", 64L)),
  chr_mixed = sample(c("", "short", strrep("long", 64L), latin1, utf8, bytes, NA), size, replace = TRUE),
  fct_low_cardinality = factor(sample(c("a", "b", "c", NA), size, replace = TRUE)),
  fct_high_cardinality = factor(sample(sprintf("level-%06d", 1:100000), size, replace = TRUE))
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
