library(rdz)
library(fst)

iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "5"))
n <- as.integer(Sys.getenv("RDZ_FST_N", "1000000"))
output <- Sys.getenv("RDZ_BENCH_OUTPUT", tempfile(fileext = ".csv"))
fst_threads_requested <- as.integer(Sys.getenv("RDZ_FST_THREADS", "1"))

if (length(fst_threads_requested) != 1L || is.na(fst_threads_requested) ||
    fst_threads_requested < 0L) {
  stop("RDZ_FST_THREADS must be a single non-negative integer",
       call. = FALSE)
}

fst::threads_fst(fst_threads_requested)
fst_threads_effective <- fst::threads_fst()
cat("fst threads: requested", fst_threads_requested,
    "effective", fst_threads_effective, "\n")

measure <- function(functions, validate = NULL) {
  timings <- setNames(lapply(functions, function(x) numeric()), names(functions))
  for (iteration in seq_len(iterations)) {
    for (name in sample(names(functions))) {
      gc(FALSE)
      started <- bench::hires_time()
      value <- functions[[name]]()
      elapsed <- bench::hires_time() - started
      if (!is.null(validate) && !validate(value)) {
        stop("round-trip validation failed for ", name, call. = FALSE)
      }
      timings[[name]] <- c(timings[[name]], elapsed)
      rm(value)
    }
  }
  data.frame(
    format = names(timings),
    median_ms = vapply(timings, median, numeric(1)) * 1000,
    p25_ms = vapply(timings, quantile, numeric(1), probs = .25) * 1000,
    p75_ms = vapply(timings, quantile, numeric(1), probs = .75) * 1000,
    row.names = NULL
  )
}

assert_native_rdz <- function(path) {
  header <- readBin(path, "raw", n = 9L)
  if (length(header) != 9L ||
      !rawToChar(header[seq_len(8L)]) %in% c("RDZFILE1", "RDZFILE2") ||
      as.integer(header[[9L]]) != 1L) {
    stop("benchmark object did not use the native rdz codec", call. = FALSE)
  }
}

run_benchmark <- function(name, make) {
  object <- make()
  directory <- tempfile(paste0("rdz-fst-", name, "-"))
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE))
  paths <- setNames(
    file.path(directory, c(
      "speed.rdz", "balanced.rdz", "uncompressed.fst", "default.fst"
    )),
    c("rdz_speed", "rdz_balanced", "fst_uncompressed", "fst_default")
  )
  clone <- function(x) unserialize(serialize(x, NULL, version = 3L))
  objects <- setNames(lapply(paths, function(path) clone(object)), names(paths))
  validate <- function(restored) identical(object, restored)

  write_rdz(objects[["rdz_speed"]], paths[["rdz_speed"]])
  write_rdz(objects[["rdz_balanced"]], paths[["rdz_balanced"]],
                preset = "balanced")
  write_fst(objects[["fst_uncompressed"]], paths[["fst_uncompressed"]],
            compress = 0)
  write_fst(objects[["fst_default"]], paths[["fst_default"]])

  assert_native_rdz(paths[["rdz_speed"]])
  assert_native_rdz(paths[["rdz_balanced"]])

  stopifnot(validate(read_rdz(paths[["rdz_speed"]])))
  stopifnot(validate(read_rdz(paths[["rdz_balanced"]])))
  stopifnot(validate(read_fst(paths[["fst_uncompressed"]], as.data.table = FALSE)))
  stopifnot(validate(read_fst(paths[["fst_default"]], as.data.table = FALSE)))

  writers <- list(
    rdz_speed = function() write_rdz(
      objects[["rdz_speed"]], paths[["rdz_speed"]]
    ),
    rdz_balanced = function() write_rdz(
      objects[["rdz_balanced"]], paths[["rdz_balanced"]],
      preset = "balanced"
    ),
    fst_uncompressed = function() write_fst(
      objects[["fst_uncompressed"]], paths[["fst_uncompressed"]], compress = 0
    ),
    fst_default = function() write_fst(
      objects[["fst_default"]], paths[["fst_default"]]
    )
  )
  readers <- list(
    rdz_speed = function() read_rdz(paths[["rdz_speed"]]),
    rdz_balanced = function() read_rdz(paths[["rdz_balanced"]]),
    fst_uncompressed = function() read_fst(
      paths[["fst_uncompressed"]], as.data.table = FALSE
    ),
    fst_default = function() read_fst(paths[["fst_default"]],
                                      as.data.table = FALSE)
  )

  write_times <- measure(writers)
  read_times <- measure(readers, validate)
  result <- merge(write_times, read_times, by = "format",
                  suffixes = c("_write", "_read"), sort = FALSE)
  result$case <- name
  result$rows <- nrow(object)
  result$columns <- ncol(object)
  result$object_mib <- as.numeric(object.size(object)) / 1024^2
  result$file_mib <- as.numeric(file.info(paths[result$format])$size) / 1024^2
  result$fst_threads_requested <- fst_threads_requested
  result$fst_threads_effective <- fst_threads_effective
  result[c(
    "case", "rows", "columns", "object_mib", "format", "file_mib",
    "fst_threads_requested", "fst_threads_effective",
    "median_ms_write", "p25_ms_write", "p75_ms_write",
    "median_ms_read", "p25_ms_read", "p75_ms_read"
  )]
}

set.seed(20260627)
cases <- list(
  mixed = function() data.frame(
    id = sample.int(n, n, replace = TRUE),
    value = rnorm(n),
    flag = sample(c(TRUE, FALSE, NA), n, replace = TRUE),
    group = sample(sprintf("group-%03d", 1:200), n, replace = TRUE),
    category = factor(sample(sprintf("level-%02d", 1:20), n, replace = TRUE)),
    date = as.Date("2000-01-01") + seq_len(n)
  ),
  wide_numeric = function() {
    rows <- n %/% 10L
    setNames(as.data.frame(replicate(40L, rnorm(rows), simplify = FALSE)),
             sprintf("numeric_%02d", 1:40))
  },
  repeated = function() data.frame(
    numeric = rep(c(0, 1, NA_real_), length.out = n),
    integer = rep(seq_len(100L), length.out = n),
    logical = rep(c(FALSE, FALSE, TRUE, NA), length.out = n),
    group = rep(sprintf("group-%03d", 1:100), length.out = n),
    date = as.Date("2000-01-01") + seq_len(n)
  ),
  unique_strings = function() {
    rows <- min(n, 500000L)
    id <- integer(rows)
    id[] <- seq_len(rows)
    data.frame(
      id = id,
      value = runif(rows),
      label = sprintf("unique-value-%08d", seq_len(rows))
    )
  },
  temporal = function() data.frame(
    date = as.Date("2000-01-01") + seq_len(n),
    timestamp = as.POSIXct("2000-01-01", tz = "UTC") + seq_len(n),
    value = runif(n)
  )
)

results <- lapply(names(cases), function(name) {
  cat("benchmarking", name, "\n")
  run_benchmark(name, cases[[name]])
})
results <- do.call(rbind, results)
row.names(results) <- NULL
write.csv(results, output, row.names = FALSE)
cat("Results written to:", output, "\n")
