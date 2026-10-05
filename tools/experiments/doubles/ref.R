library(fst)
dir <- commandArgs(TRUE)[1]
q <- tempfile(); f <- tempfile()
med <- function(fn) { t <- replicate(3, { gc(); system.time(fn())[[3]] }); median(t) }
cat(sprintf("%-12s %-10s %8s %9s %9s\n", "data", "format", "bits/val", "enc MB/s", "dec MB/s"))
for (nm in c("random", "rounded", "currency", "monotonic", "date", "posix_sec", "posix_ms", "prices", "sensor", "ratio")) {
  x <- readBin(file.path(dir, paste0(nm, ".bin")), "double", 4e6, endian = "little")
  mb <- length(x) * 8 / 1e6
  w <- med(function() qs2::qs_save(x, q, nthreads = 1)); r <- med(function() qs2::qs_read(q, nthreads = 1))
  cat(sprintf("%-12s %-10s %8.2f %9.0f %9.0f\n", nm, "qs2", file.size(q) * 8 / length(x), mb / w, mb / r))
  df <- data.frame(x = x); threads_fst(1)
  w <- med(function() write_fst(df, f, compress = 50)); r <- med(function() read_fst(f))
  cat(sprintf("%-12s %-10s %8.2f %9.0f %9.0f\n", nm, "fst50", file.size(f) * 8 / length(x), mb / w, mb / r))
  w <- med(function() write_fst(df, f, compress = 100)); r <- med(function() read_fst(f))
  cat(sprintf("%-12s %-10s %8.2f %9.0f %9.0f\n", nm, "fst100", file.size(f) * 8 / length(x), mb / w, mb / r))
}
