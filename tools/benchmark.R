# Run directly with `Rscript tools/benchmark.R`.
# Optional environment variables:
#   RDZ_BENCH_SIZE=100000
#   RDZ_BENCH_SIZES=1000,100000,1000000
#   RDZ_BENCH_ITERATIONS=10
#   RDZ_BENCH_WARMUPS=1
#   RDZ_BENCH_THREADS=1
#   RDZ_BENCH_SUITE=representative
#   RDZ_BENCH_MEMORY=true
#   RDZ_BENCH_OUTPUT=/path/to/details.csv
#   RDZ_BENCH_RDS_OUTPUT=/path/to/complete-result.rds

devtools::load_all(".", quiet = TRUE, debug = FALSE, recompile = TRUE)
source("tools/bench-helpers.R")

parse_boolean <- function(name, default) {
  value <- tolower(Sys.getenv(name, default))
  if (!value %in% c("true", "false")) {
    stop(name, " must be true or false", call. = FALSE)
  }
  identical(value, "true")
}

size <- as.integer(Sys.getenv("RDZ_BENCH_SIZE", "100000"))
sizes_text <- Sys.getenv("RDZ_BENCH_SIZES", "")
iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "10"))
warmups <- as.integer(Sys.getenv("RDZ_BENCH_WARMUPS", "1"))
threads <- as.integer(Sys.getenv("RDZ_BENCH_THREADS", "1"))
object_suite <- Sys.getenv("RDZ_BENCH_SUITE", "representative")
memory <- parse_boolean("RDZ_BENCH_MEMORY", "true")
output <- Sys.getenv("RDZ_BENCH_OUTPUT", "")
rds_output <- Sys.getenv("RDZ_BENCH_RDS_OUTPUT", "")

common <- list(
  object_suite = object_suite,
  iterations = iterations,
  warmups = warmups,
  threads = threads,
  build_mode = "devtools::load_all(debug=FALSE, recompile=TRUE)",
  memory = memory,
  progress = TRUE
)

if (nzchar(sizes_text)) {
  sizes <- as.integer(strsplit(sizes_text, ",", fixed = TRUE)[[1L]])
  names(sizes) <- if (length(sizes) == 3L) {
    c("small", "medium", "throughput")
  } else {
    paste0("size_", sizes)
  }
  result <- do.call(
    benchmark_serialization_suite,
    c(list(sizes = sizes), common)
  )
} else {
  result <- do.call(
    benchmark_serialization_matrix,
    c(list(size = size), common)
  )
}

print(result, digits = 5)

if (nzchar(output)) {
  utils::write.csv(result$details, output, row.names = FALSE)
  message("Wrote long-form benchmark details to ", output)
}

if (nzchar(rds_output)) {
  saveRDS(result, rds_output, compress = FALSE)
  message("Wrote complete benchmark result to ", rds_output)
}
