library(bench)
library(rdz)
library(qs2)

iterations <- as.integer(Sys.getenv("RDZ_BENCH_ITERATIONS", "5"))
n <- as.integer(Sys.getenv("RDZ_BENCH_N", "2000000"))

run_benchmark <- function(name, object, include_qdata = TRUE) {
  directory <- tempfile(paste0("rdz-bench-", name, "-"))
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE))
  paths <- setNames(
    file.path(
      directory,
      c(
        "object-speed.rdz",
        "object-balanced.rdz",
        "object.qs2",
        "object-fast.qs2",
        "object.qdata"
      )
    ),
    c("rdz_speed", "rdz_balanced", "qs2", "qs2_fast", "qdata")
  )

  write_rdz(object, paths[["rdz_speed"]])
  write_rdz(object, paths[["rdz_balanced"]], preset = "balanced")
  qs_save(object, paths[["qs2"]])
  qs_save(object, paths[["qs2_fast"]], compress_level = -1000L, shuffle = FALSE)
  if (include_qdata) {
    qd_save(object, paths[["qdata"]])
  }
  stopifnot(identical(object, read_rdz(paths[["rdz_speed"]])))
  stopifnot(identical(object, read_rdz(paths[["rdz_balanced"]])))

  if (include_qdata) {
    writes <- mark(
      rdz_speed = write_rdz(object, paths[["rdz_speed"]]),
      rdz_balanced = write_rdz(
        object,
        paths[["rdz_balanced"]],
        preset = "balanced"
      ),
      qs2 = qs_save(object, paths[["qs2"]]),
      qs2_fast = qs_save(
        object,
        paths[["qs2_fast"]],
        compress_level = -1000L,
        shuffle = FALSE
      ),
      qdata = qd_save(object, paths[["qdata"]]),
      iterations = iterations,
      check = FALSE,
      memory = FALSE
    )
    reads <- mark(
      rdz_speed = read_rdz(paths[["rdz_speed"]]),
      rdz_balanced = read_rdz(paths[["rdz_balanced"]]),
      qs2 = qs_read(paths[["qs2"]]),
      qs2_fast = qs_read(paths[["qs2_fast"]]),
      qdata = qd_read(paths[["qdata"]]),
      iterations = iterations,
      check = FALSE,
      memory = FALSE
    )
  } else {
    paths <- paths[names(paths) != "qdata"]
    writes <- mark(
      rdz_speed = write_rdz(object, paths[["rdz_speed"]]),
      rdz_balanced = write_rdz(
        object,
        paths[["rdz_balanced"]],
        preset = "balanced"
      ),
      qs2 = qs_save(object, paths[["qs2"]]),
      qs2_fast = qs_save(
        object,
        paths[["qs2_fast"]],
        compress_level = -1000L,
        shuffle = FALSE
      ),
      iterations = iterations,
      check = FALSE,
      memory = FALSE
    )
    reads <- mark(
      rdz_speed = read_rdz(paths[["rdz_speed"]]),
      rdz_balanced = read_rdz(paths[["rdz_balanced"]]),
      qs2 = qs_read(paths[["qs2"]]),
      qs2_fast = qs_read(paths[["qs2_fast"]]),
      iterations = iterations,
      check = FALSE,
      memory = FALSE
    )
  }

  cat(
    "\n",
    name,
    " (",
    format(object.size(object), units = "MiB"),
    ")\n",
    sep = ""
  )
  print(
    data.frame(
      package = ifelse(startsWith(names(paths), "rdz"), "rdz", "qs2"),
      format = names(paths),
      size_mib = unname(file.info(paths)$size / 1024^2)
    ),
    row.names = FALSE
  )
  cat("write\n")
  print(writes[, c("expression", "median", "itr/sec")])
  cat("read\n")
  print(reads[, c("expression", "median", "itr/sec")])
}

set.seed(1)
run_benchmark(
  "atomic-list",
  list(
    numeric = runif(n),
    integer = sample.int(1000L, n, replace = TRUE),
    logical = sample(c(TRUE, FALSE, NA), n, replace = TRUE)
  )
)

run_benchmark("low-cardinality-numeric", rep(c(0, 1, NA_real_), length.out = n))

run_benchmark("date-sequence", as.Date("2000-01-01") + seq_len(n))

run_benchmark(
  "mixed-data-frame",
  data.frame(
    id = sample.int(n, n, replace = TRUE),
    value = rnorm(n),
    flag = sample(c(TRUE, FALSE, NA), n, replace = TRUE),
    group = sample(sprintf("group-%03d", 1:200), n, replace = TRUE)
  )
)

string_n <- min(n, 1000000L)
run_benchmark(
  "high-cardinality-strings",
  sprintf("unique-value-%08d", seq_len(string_n))
)

run_benchmark(
  "general-fallback",
  list(
    call = quote(mean(value)),
    numeric = runif(n),
    integer = sample.int(1000L, n, replace = TRUE),
    logical = sample(c(TRUE, FALSE, NA), n, replace = TRUE)
  ),
  include_qdata = FALSE
)
