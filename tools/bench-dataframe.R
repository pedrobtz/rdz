# Data frames (plan-c Stage H): rdz at each preset against qdata, qs2 and
# fst, one thread and eight; full reads and writes, and fst's one-column
# read reported apart (rdz reads whole objects). Median of
# RDZ_BENCH_ITERATIONS (default 5) elapsed times after a gc().
#
#   Rscript tools/bench-dataframe.R

library(rdz)
stopifnot(requireNamespace("qs2"), requireNamespace("fst"))
options(width = 200)
iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "5"))
rows <- as.integer(Sys.getenv("RDZ_BENCH_ROWS", "5000000"))
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
df <- data.frame(
  logical = sample(c(TRUE, FALSE, NA), rows, replace = TRUE),
  integer = sample.int(1000000L, rows, replace = TRUE),
  double = round(stats::rnorm(rows), 3),
  character = sample(sprintf("item-%05d", 1:20000), rows, replace = TRUE),
  factor = factor(sample(c("north", "south", "east", "west"), rows, replace = TRUE)),
  stringsAsFactors = FALSE
)

out <- list()
add <- function(format, threads, write, read, path, one_column = NA) {
  out[[length(out) + 1L]] <<- data.frame(
    format = format, threads = threads, write_ms = write, read_ms = read,
    mb = round(file.size(path) / 1024^2, 1), one_column_read_ms = one_column
  )
}
for (threads in c(1L, 8L)) {
  for (preset in c("speed", "balanced", "compact")) {
    p <- file.path(dir, "x.rdz")
    old <- options(rdz.preset = preset, rdz.threads = threads)
    w <- ms(function() write_rdz(df, p))
    r <- ms(function() read_rdz(p))
    stopifnot(identical(read_rdz(p), df))
    options(old)
    add(paste0("rdz ", preset), threads, w, r, p)
  }
  p <- file.path(dir, "x.qd")
  add("qdata", threads, ms(function() qs2::qd_save(df, p, nthreads = threads)),
      ms(function() qs2::qd_read(p, nthreads = threads)), p)
  p <- file.path(dir, "x.qs2")
  add("qs2", threads, ms(function() qs2::qs_save(df, p, nthreads = threads)),
      ms(function() qs2::qs_read(p, validate_checksum = TRUE, nthreads = threads)), p)
  p <- file.path(dir, "x.fst")
  fst::threads_fst(threads)
  add("fst (50)", threads, ms(function() fst::write_fst(df, p, compress = 50)),
      ms(function() fst::read_fst(p)), p,
      ms(function() fst::read_fst(p, columns = "double")))
}
print(do.call(rbind, out), row.names = FALSE)
cat("\n", rows, " rows x 5 columns (", round(utils::object.size(df) / 1024^2), " MB in memory); ",
    R.version.string, " ", Sys.info()[["machine"]], ", ", parallel::detectCores(), " cores; qs2 ",
    utils::packageDescription("qs2")$Version, ", fst ", utils::packageDescription("fst")$Version,
    "\n", sep = "")
