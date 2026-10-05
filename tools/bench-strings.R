# String-dictionary experiment: RDZ plain/block/global/auto string encodings versus
# qdata, qs2, fst, and uncompressed base R across the share of distinct values.
#
# Run with `Rscript tools/bench-strings.R`. Optional environment variables:
#   RDZ_BENCH_N=1000000         elements per vector
#   RDZ_BENCH_ITERATIONS=5      timed repetitions per operation
#   RDZ_BENCH_OUTPUT=/path.csv  long-form results
#
# RDZ writes a logical vector whose `names` hold the strings (the only native
# character path today) and reads back only the `names` attribute, so the
# logical payload costs RDZ about 1 ms per write and nothing on read.
#
# Operations are timed in interleaved rounds (see `interleaved_ms()`). Two
# read conditions are reported:
#   warm: the source vector is alive, so every string is already in R's
#         global CHARSXP cache and each lookup is a hit.
#   cold: the source is dropped and `gc()` runs before every read, so the
#         reader must create each distinct CHARSXP, as in a fresh session.

devtools::load_all(".", quiet = TRUE, debug = FALSE)

n <- as.integer(Sys.getenv("RDZ_BENCH_N", "1000000"))
iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "5"))
output <- Sys.getenv("RDZ_BENCH_OUTPUT", "")

fst::threads_fst(1L)

string_pool <- function(distinct, width, prefix) {
  ids <- sample.int(1e9L, distinct)
  short <- sprintf("%s%09d", prefix, ids)
  if (width == "short") {
    return(short)
  }
  hex <- function() sample.int(.Machine$integer.max, distinct, replace = TRUE)
  sprintf(
    "%s-%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
    short, hex(), hex(), hex(), hex(), hex(), hex(), hex(), hex(), hex(), hex(), hex()
  )
}

make_strings <- function(n, unique_share, width, order, prefix) {
  distinct <- max(1L, as.integer(round(n * unique_share)))
  pool <- string_pool(distinct, width, prefix)
  x <- c(pool, pool[sample.int(distinct, n - distinct, replace = TRUE)])
  x <- if (order == "sorted") sort(x) else x[sample.int(n)]
  x[sample.int(n, n %/% 100L)] <- NA_character_
  x
}

formats <- list(
  rdz_plain = list(policy = "plain"),
  rdz_block = list(policy = "block"),
  rdz_global = list(policy = "global"),
  rdz_auto = list(policy = "auto"),
  qdata = list(
    write = function(x, path) qs2::qd_save(x, path, nthreads = 1L),
    read = function(path) qs2::qd_read(path, validate_checksum = TRUE, nthreads = 1L)
  ),
  qs2 = list(
    write = function(x, path) qs2::qs_save(x, path, nthreads = 1L),
    read = function(path) qs2::qs_read(path, validate_checksum = TRUE, nthreads = 1L)
  ),
  fst = list(
    write = function(x, path) fst::write_fst(data.frame(x = x, stringsAsFactors = FALSE), path),
    read = function(path) fst::read_fst(path)[["x"]]
  ),
  base_raw = list(
    write = function(x, path) saveRDS(x, path, compress = FALSE),
    read = function(path) readRDS(path)
  )
)

for (name in grep("^rdz_", names(formats), value = TRUE)) {
  local({
    policy <- formats[[name]]$policy
    formats[[name]]$write <<- function(x, path) {
      Sys.setenv(RDZ_STRING_DICT = policy)
      on.exit(Sys.unsetenv("RDZ_STRING_DICT"), add = TRUE)
      carrier <- logical(length(x))
      names(carrier) <- x
      write_rdz(carrier, path, mode = "native")
    }
    formats[[name]]$read <<- function(path) rdz_attributes(path, names = "names")$names
  })
}

cases <- expand.grid(
  unique_share = c(0.0001, 0.001, 0.01, 0.1, 0.5, 0.75, 0.9, 1),
  width = c("short", "long"),
  order = "random",
  stringsAsFactors = FALSE
)
cases <- rbind(
  cases,
  data.frame(unique_share = c(0.01, 0.1), width = "short", order = "sorted")
)

# Every operation is timed in rounds: each round runs every format once, in a
# freshly shuffled order. R's GC thresholds drift as a sequence of large
# allocations proceeds, so a fixed order would bias whichever format runs
# first; interleaving spreads that drift evenly.
interleaved_ms <- function(operations, before = function() NULL) {
  times <- matrix(NA_real_, iterations, length(operations), dimnames = list(NULL, names(operations)))
  for (round in seq_len(iterations)) {
    for (format in sample(names(operations))) {
      before()
      start <- bench::hires_time()
      value <- operations[[format]]()
      times[round, format] <- (bench::hires_time() - start) * 1000
      rm(value)
    }
  }
  apply(times, 2L, stats::median)
}

directory <- tempfile("rdz-bench-strings-")
dir.create(directory)
on.exit(unlink(directory, recursive = TRUE), add = TRUE)
set.seed(2026L)
rows <- list()

for (case_index in seq_len(nrow(cases))) {
  case <- cases[case_index, ]
  label <- sprintf("%g%% %s %s", 100 * case$unique_share, case$width, case$order)
  message("Case ", case_index, "/", nrow(cases), ": ", label)
  x <- make_strings(n, case$unique_share, case$width, case$order, sprintf("c%02d_", case_index))
  distinct <- length(unique(x))
  paths <- stats::setNames(file.path(directory, paste0(case_index, "-", names(formats))), names(formats))

  for (format in names(formats)) {
    formats[[format]]$write(x, paths[[format]])
    if (!identical(formats[[format]]$read(paths[[format]]), x)) {
      stop("Round trip failed for ", format, " in case ", label, call. = FALSE)
    }
  }
  writes <- lapply(names(formats), function(format) function() formats[[format]]$write(x, paths[[format]]))
  reads <- lapply(names(formats), function(format) function() formats[[format]]$read(paths[[format]]))
  names(writes) <- names(reads) <- names(formats)

  write_ms <- interleaved_ms(writes)
  read_warm_ms <- interleaved_ms(reads)
  rm(x, writes)
  read_cold_ms <- interleaved_ms(reads, before = function() gc(full = TRUE))

  rows[[case_index]] <- data.frame(
    case = label,
    unique_share = case$unique_share,
    distinct = distinct,
    width = case$width,
    order = case$order,
    format = names(formats),
    write_ms = unname(write_ms),
    read_warm_ms = unname(read_warm_ms),
    read_cold_ms = unname(read_cold_ms),
    size_mib = unname(file.size(paths) / 2^20)
  )
  unlink(paths)
}

results <- do.call(rbind, rows)
rownames(results) <- NULL

pivot <- function(metric) {
  table <- stats::reshape(
    results[, c("case", "format", metric)],
    idvar = "case", timevar = "format", direction = "wide"
  )
  names(table) <- sub(paste0("^", metric, "\\."), "", names(table))
  rownames(table) <- NULL
  table
}

cat(sprintf(
  "\nn = %s, iterations = %d, one thread, %s, R %s, qs2 %s, fst %s\n",
  format(n, big.mark = ","), iterations, Sys.info()[["machine"]],
  getRversion(), packageVersion("qs2"), packageVersion("fst")
))
for (metric in c("read_cold_ms", "read_warm_ms", "write_ms", "size_mib")) {
  cat("\n", metric, "\n", sep = "")
  print(pivot(metric), digits = 3, row.names = FALSE)
}

if (nzchar(output)) {
  utils::write.csv(results, output, row.names = FALSE)
  message("Wrote long-form results to ", output)
}
