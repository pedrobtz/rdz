pkgload::load_all(".", quiet = TRUE)
source("tools/bench-helpers.R")

set.seed(2026)

size <- 1e6

run_benchmark <- function(x) {
  list(
    write = bench_write(x),
    read = bench_read(x)
  )
}

benchmarks <- list(
  logical = run_benchmark(
    sample(c(FALSE, TRUE), size, replace = TRUE)
  ),
  integer = run_benchmark(
    sample.int(size, size, replace = TRUE)
  ),
  numeric = run_benchmark(
    runif(size)
  )
)

for (type in names(benchmarks)) {
  cat("\n", type, " vector\n", sep = "")
  cat("\nWrite\n")
  print(benchmarks[[type]]$write, width = Inf)
  cat("\nRead\n")
  print(benchmarks[[type]]$read, width = Inf)
}
