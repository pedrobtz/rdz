library(fastrds)
library(qs2)

iterations <- as.integer(Sys.getenv("FASTRDS_BENCH_ITERATIONS", "5"))
n <- as.integer(Sys.getenv("FASTRDS_BENCH_N", "1000000"))
output <- Sys.getenv("FASTRDS_BENCH_OUTPUT", tempfile(fileext = ".csv"))

strict <- function(original, restored) identical(original, restored)
equivalent <- function(original, restored) {
  isTRUE(all.equal(original, restored, check.environment = FALSE))
}
closure_equivalent <- function(original, restored) {
  is.function(restored) &&
    identical(formals(original), formals(restored)) &&
    identical(body(original), body(restored)) &&
    identical(original(c(1L, n)), restored(c(1L, n)))
}
environment_equivalent <- function(original, restored) {
  is.environment(restored) &&
    identical(as.list.environment(original, all.names = TRUE),
              as.list.environment(restored, all.names = TRUE))
}

cases <- list(
  numeric_random = list(group = "atomic", make = function() runif(n), validate = strict),
  numeric_repeated = list(group = "atomic", make = function() rep(c(0, 1, NA_real_), length.out = n), validate = strict),
  integer_random = list(group = "atomic", make = function() sample.int(100000L, n, TRUE), validate = strict),
  integer_sequence_altrep = list(group = "atomic", make = function() seq_len(n), validate = strict),
  logical_sparse = list(group = "atomic", make = function() sample(c(FALSE, TRUE, NA), n, TRUE, c(.98, .01, .01)), validate = strict),
  complex_random = list(group = "atomic", make = function() complex(real = rnorm(n), imaginary = rnorm(n)), validate = strict),
  raw_random = list(group = "atomic", make = function() as.raw(sample.int(256L, n, TRUE) - 1L), validate = strict),
  strings_repeated = list(group = "strings", make = function() sample(sprintf("group-%03d", 1:100), n, TRUE), validate = strict),
  strings_unique = list(group = "strings", make = function() sprintf("unique-value-%08d", seq_len(n)), validate = strict),
  factor_repeated = list(group = "strings", make = function() factor(sample(sprintf("level-%03d", 1:100), n, TRUE)), validate = strict),
  named_numeric = list(group = "strings", make = function() setNames(runif(n / 4L), sprintf("name-%08d", seq_len(n / 4L))), validate = strict),
  numeric_matrix = list(group = "array", make = function() matrix(rnorm(n), ncol = 100L), validate = strict),
  integer_matrix = list(group = "array", make = function() matrix(sample.int(1000L, n, TRUE), ncol = 100L), validate = strict),
  numeric_array = list(group = "array", make = function() array(runif(n), dim = c(100L, 100L, n / 10000L)), validate = strict),
  mixed_data_frame = list(group = "tabular", make = function() {
    rows <- n / 4L
    data.frame(
      id = sample.int(rows, rows, TRUE),
      value = rnorm(rows),
      flag = sample(c(TRUE, FALSE, NA), rows, TRUE),
      group = sample(sprintf("group-%03d", 1:200), rows, TRUE),
      stringsAsFactors = FALSE
    )
  }, validate = strict),
  wide_data_frame = list(group = "tabular", make = function() {
    rows <- n / 50L
    setNames(as.data.frame(replicate(50L, rnorm(rows), simplify = FALSE)),
             sprintf("column_%02d", 1:50))
  }, validate = strict),
  nested_list = list(group = "container", make = function() {
    leaves <- lapply(seq_len(40L), function(i) runif(n / 40L))
    list(first = leaves[1:10], second = list(leaves[11:25], leaves[26:40]))
  }, validate = strict),
  dates = list(group = "attributed", make = function() as.Date("2000-01-01") + seq_len(n), validate = strict),
  posixct = list(group = "attributed", make = function() as.POSIXct("2000-01-01", tz = "UTC") + seq_len(n), validate = strict),
  time_series = list(group = "attributed", make = function() ts(matrix(rnorm(n), ncol = 2L), frequency = 12), validate = strict),
  contingency_table = list(group = "attributed", make = function() {
    structure(sample.int(1000L, n, TRUE),
              dim = c(100L, 100L, n / 10000L),
              class = "table")
  }, validate = strict),
  language_in_list = list(group = "fallback", make = function() list(payload = runif(n), call = quote(mean(payload))), validate = strict),
  pairlist = list(group = "fallback", make = function() pairlist(payload = runif(n), call = quote(mean(payload))), validate = strict),
  closure = list(group = "fallback", make = function() local({
    payload <- runif(n)
    function(index) payload[index]
  }), validate = closure_equivalent),
  environment = list(group = "fallback", make = function() {
    environment <- new.env(parent = emptyenv())
    environment$numeric <- runif(n)
    environment$integer <- sample.int(1000L, n, TRUE)
    environment
  }, validate = environment_equivalent),
  s4_object = list(group = "fallback", make = function() {
    if (!methods::isClass("fastrds_benchmark_record")) {
      methods::setClass("fastrds_benchmark_record",
                        slots = c(values = "numeric", label = "character"))
    }
    methods::new("fastrds_benchmark_record", values = runif(n), label = "benchmark")
  }, validate = equivalent),
  linear_model = list(group = "fallback", make = function() {
    rows <- n / 5L
    frame <- data.frame(y = rnorm(rows), x = rnorm(rows),
                        group = factor(sample(letters[1:10], rows, TRUE)))
    lm(y ~ x + group, data = frame)
  }, validate = equivalent)
)

measure <- function(functions, iterations, validate = NULL) {
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

set.seed(20260627)
results <- list()
for (case_name in names(cases)) {
  case <- cases[[case_name]]
  object <- case$make()
  validate <- function(restored) case$validate(object, restored)
  directory <- tempfile(paste0("fastrds-", case_name, "-"))
  dir.create(directory)
  paths <- setNames(
    file.path(directory, c(
      "object.fastrds", "object.qs2", "object-throughput.qs2", "object.qdata"
    )),
    c("fastrds", "qs2_default", "qs2_throughput", "qdata")
  )
  clone <- function(x) unserialize(serialize(x, NULL, version = 3L))
  format_objects <- setNames(lapply(paths, function(path) clone(object)), names(paths))

  write_fastrds(format_objects[["fastrds"]], paths[["fastrds"]])
  qs_save(format_objects[["qs2_default"]], paths[["qs2_default"]])
  qs_save(format_objects[["qs2_throughput"]], paths[["qs2_throughput"]],
          compress_level = -1000L, shuffle = FALSE)

  stopifnot(validate(read_fastrds(paths[["fastrds"]])))
  stopifnot(validate(qs_read(paths[["qs2_default"]])))
  stopifnot(validate(qs_read(paths[["qs2_throughput"]])))

  qdata_ok <- tryCatch({
    suppressWarnings(qd_save(format_objects[["qdata"]], paths[["qdata"]]))
    restored <- suppressWarnings(qd_read(paths[["qdata"]]))
    validate(restored)
  }, error = function(error) FALSE)

  writers <- list(
    fastrds = function() write_fastrds(format_objects[["fastrds"]],
                                       paths[["fastrds"]]),
    qs2_default = function() qs_save(format_objects[["qs2_default"]],
                                     paths[["qs2_default"]]),
    qs2_throughput = function() qs_save(
      format_objects[["qs2_throughput"]], paths[["qs2_throughput"]],
      compress_level = -1000L, shuffle = FALSE
    )
  )
  readers <- list(
    fastrds = function() read_fastrds(paths[["fastrds"]]),
    qs2_default = function() qs_read(paths[["qs2_default"]]),
    qs2_throughput = function() qs_read(paths[["qs2_throughput"]])
  )
  if (qdata_ok) {
    writers$qdata <- function() qd_save(format_objects[["qdata"]],
                                        paths[["qdata"]])
    readers$qdata <- function() qd_read(paths[["qdata"]])
  }

  write_times <- measure(writers, iterations)
  read_times <- measure(readers, iterations, validate)
  result <- merge(write_times, read_times, by = "format",
                  suffixes = c("_write", "_read"), sort = FALSE)
  result$case <- case_name
  result$group <- case$group
  result$object_mib <- as.numeric(object.size(object)) / 1024^2
  result$file_mib <- as.numeric(file.info(paths[result$format])$size) / 1024^2
  result$fastrds_codec <- if (as.integer(readBin(
    paths[["fastrds"]], "raw", n = 9L
  )[[9L]]) == 1L) {
    "native"
  } else {
    "R serialization"
  }
  results[[case_name]] <- result[c(
    "case", "group", "object_mib", "fastrds_codec", "format", "file_mib",
    "median_ms_write", "p25_ms_write", "p75_ms_write",
    "median_ms_read", "p25_ms_read", "p75_ms_read"
  )]
  cat(sprintf("%-25s codec=%-15s qdata=%s\n", case_name,
              result$fastrds_codec[[1]], qdata_ok))

  unlink(directory, recursive = TRUE)
  rm(object, format_objects)
  gc(FALSE)
}

results <- do.call(rbind, results)
row.names(results) <- NULL
write.csv(results, output, row.names = FALSE)
cat("\nResults written to:", output, "\n")
