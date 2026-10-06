# rdz against fst, one thread each, on the column types and logical
# distributions where the two were compared while tuning the codecs (plan-c,
# after Stage S): medians of bench::mark() for writing and reading a
# one-column data frame (and a mixed one), at rdz level 0 against fst 0 and
# rdz level 1 (the default) against fst 50 (its default). Writes a CSV and a
# markdown table; benchmarks.yaml runs it on a GitHub Linux runner.
#
#   Rscript tools/bench-vs-fst.R [rows] [out.csv] [out.md]

suppressPackageStartupMessages({
  library(rdz)
  library(fst)
})
args <- commandArgs(trailingOnly = TRUE)
n <- if (length(args) >= 1L) as.numeric(args[[1L]]) else 5e6
csv <- if (length(args) >= 2L) args[[2L]] else "bench-vs-fst.csv"
md <- if (length(args) >= 3L) args[[3L]] else "bench-vs-fst.md"
iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "7"))
threads_fst(1L)
options(rdz.threads = 1L)

set.seed(20261006)
lgl <- function(p) sample(c(FALSE, TRUE, NA), n, TRUE, prob = p)
cols <- list(
  lgl_random = lgl(c(1, 1, 1)),
  lgl_sparse = lgl(c(0.985, 0.01, 0.005)),
  lgl_runs = rep(c(FALSE, TRUE, NA), each = ceiling(n / 3))[seq_len(n)],
  int_seq = seq_len(n),
  int_small = sample.int(100L, n, TRUE),
  factor = factor(sample(letters, n, TRUE)),
  dbl_random = runif(n),
  dbl_prices = round(runif(n, 1, 1000), 2),
  dbl_time = as.numeric(as.POSIXct("2026-01-01", tz = "UTC")) + cumsum(rexp(n, 10)),
  chr_dict = sample(c("north", "south", "east", "west", NA), n, TRUE),
  chr_unique = sprintf("id%08d", sample.int(n))
)
cols$mixed <- as.data.frame(cols[c("int_small", "dbl_prices", "chr_dict", "factor",
                                    "lgl_random", "dbl_time")])

dir <- tempfile("bench-")
dir.create(dir)
p <- file.path(dir, "x.rdz")
q <- file.path(dir, "x.fst")
ms <- function(f) { # a function, so that every iteration runs it
  b <- bench::mark(f(), iterations = iterations, check = FALSE, filter_gc = FALSE)
  round(as.numeric(stats::median(b$time[[1L]])) * 1000, 1)
}
rows <- list()
for (setting in list(c(0L, 0L), c(1L, 50L))) {
  level <- setting[[1L]]
  fst_level <- setting[[2L]]
  for (name in names(cols)) {
    df <- if (is.data.frame(cols[[name]])) cols[[name]] else data.frame(x = cols[[name]])
    rdz_w <- ms(function() write_rdz(df, p, compress = level))
    rdz_w_nohash <- ms(function() write_rdz(df, p, compress = level, hash = FALSE))
    rdz_r <- ms(function() read_rdz(p))
    stopifnot(identical(read_rdz(p), df))
    fst_w <- ms(function() write_fst(df, q, compress = fst_level))
    fst_r <- ms(function() read_fst(q))
    rows[[length(rows) + 1L]] <- data.frame(
      setting = sprintf("rdz %d / fst %d", level, fst_level), data = name,
      rdz_write = rdz_w, rdz_write_nohash = rdz_w_nohash, fst_write = fst_w,
      rdz_read = rdz_r, fst_read = fst_r,
      rdz_mb = round(file.size(p) / 2^20, 2), fst_mb = round(file.size(q) / 2^20, 2)
    )
  }
}
result <- do.call(rbind, rows)
result$write <- ifelse(result$rdz_write <= result$fst_write, "rdz", "fst")
result$read <- ifelse(result$rdz_read <= result$fst_read, "rdz", "fst")
utils::write.csv(result, csv, row.names = FALSE)

header <- sprintf(
  "%s rows, one thread, median of %d (ms); rdz %s, fst %s, %s, %s %s, kernel %s",
  format(n, big.mark = ",", scientific = FALSE), iterations,
  utils::packageDescription("rdz")$Version, utils::packageDescription("fst")$Version,
  R.version.string, Sys.info()[["sysname"]], Sys.info()[["machine"]],
  .Call(rdz:::rdz_c_logical_kernel, NA)
)
table <- c(
  paste0("| ", paste(names(result), collapse = " | "), " |"),
  paste0("|", paste(rep("---", ncol(result)), collapse = "|"), "|"),
  apply(result, 1L, function(r) paste0("| ", paste(trimws(r), collapse = " | "), " |"))
)
writeLines(c("## rdz against fst", "", header, "", table), md)
options(width = 200)
cat(header, "\n\n")
print(result, row.names = FALSE)
unlink(dir, recursive = TRUE)
