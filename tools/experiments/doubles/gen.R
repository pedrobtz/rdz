size <- 4e6
set.seed(20261005)
numeric_value <- stats::rnorm(size)
d <- list(
  random = numeric_value,
  rounded = round(numeric_value, 2L),
  currency = round(stats::runif(size, -100000, 100000), 2L),
  monotonic = cumsum(stats::runif(size)),
  repeated = sample(c(-1, 0, 1, pi, NA_real_), size, replace = TRUE),
  all_missing = rep(NA_real_, size),
  date = replace(as.numeric(as.Date("2000-01-01") + sample.int(9000, size, TRUE)), seq(1, size, 97), NA),
  posix_sec = as.numeric(1.7e9 + sample.int(3e7, size, TRUE)),
  posix_ms = round(1.7e9 + stats::runif(size, 0, 3e7), 3),
  prices = round(100 * exp(cumsum(stats::rnorm(size, 0, 0.001))), 2),
  sensor = round(20 + cumsum(stats::rnorm(size, 0, 0.05)), 1),
  ratio = stats::runif(size) / stats::runif(size, 1, 2)
)
for (n in names(d)) writeBin(d[[n]], file.path(commandArgs(TRUE)[1], paste0(n, ".bin")), endian = "little")
cat(names(d), "\n")
