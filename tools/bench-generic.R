# The generic codec, forced (plan-c Stage C): rdz's streamed C writer and
# reader against qs2 and saveRDS(), one thread each. Run against an installed rdz:
#
#   Rscript tools/bench-generic.R
#
# Reports the median of RDZ_BENCH_ITERATIONS (default 5) runs of each write
# and read, the R heap each write allocates (bench's mem_alloc; malloc in C
# is not counted, and rdz's write holds one 1 MiB block there), and the file
# size.

library(rdz)
stopifnot(requireNamespace("bench"), requireNamespace("qs2"))
iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "5"))
n <- 1e6

datasets <- list(
  data_frame = data.frame(
    i = seq_len(n),
    d = seq_len(n) / 7,
    l = rep_len(c(TRUE, FALSE, NA), n),
    s = rep_len(sprintf("level-%03d", 1:500), n),
    f = factor(rep_len(c("a", "b", "c"), n)),
    stringsAsFactors = FALSE
  ),
  numeric_list = lapply(seq_len(100), function(k) seq_len(1e5) * k / 3),
  character = sprintf("id-%07d", seq_len(n))
)

dir <- tempfile("rdz-bench-")
dir.create(dir)
on.exit(unlink(dir, recursive = TRUE), add = TRUE)

writers <- list(
  rdz_c = list(
    write = function(x, p) write_rdz(x, p, mode = "r"),
    read = function(p) read_rdz(p)
  ),
  qs2 = list(
    write = function(x, p) qs2::qs_save(x, p, nthreads = 1L),
    read = function(p) qs2::qs_read(p, validate_checksum = TRUE, nthreads = 1L)
  ),
  rds = list(
    write = function(x, p) saveRDS(x, p, compress = FALSE),
    read = function(p) readRDS(p)
  )
)
writers <- Filter(Negate(is.null), writers)

rows <- list()
for (data_name in names(datasets)) {
  x <- datasets[[data_name]]
  for (format in names(writers)) {
    p <- file.path(dir, paste0(data_name, ".", format))
    w <- bench::mark(writers[[format]]$write(x, p), iterations = iterations,
                     check = FALSE, filter_gc = FALSE)
    r <- bench::mark(writers[[format]]$read(p), iterations = iterations,
                     check = FALSE, filter_gc = FALSE)
    stopifnot(identical(writers[[format]]$read(p), x))
    rows[[length(rows) + 1L]] <- data.frame(
      data = data_name,
      format = format,
      write_ms = round(as.numeric(w$median) * 1000, 1),
      read_ms = round(as.numeric(r$median) * 1000, 1),
      write_r_alloc_mb = round(as.numeric(w$mem_alloc) / 1024^2, 2),
      file_mb = round(file.size(p) / 1024^2, 2)
    )
  }
}
result <- do.call(rbind, rows)
print(result, row.names = FALSE)
cat("\n", R.version.string, " ", Sys.info()[["machine"]], "; ",
    utils::packageDescription("qs2")$Version, " (qs2)\n", sep = "")
