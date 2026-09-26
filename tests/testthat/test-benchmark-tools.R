test_that("read benchmark matrix reports median milliseconds", {
  skip_if_not_installed("bench")

  benchmark_helper <- test_path("..", "..", "tools", "bench-helpers.R")
  skip_if_not(
    file.exists(benchmark_helper),
    "development benchmark helper is not available in the installed package"
  )

  benchmark_environment <- new.env(parent = globalenv())
  sys.source(
    benchmark_helper,
    envir = benchmark_environment
  )

  result <- benchmark_environment$benchmark_read_matrix(
    objects = list(integer = 1:10),
    formats = c("rdz", "base", "base_uncompressed"),
    iterations = 1L,
    progress = FALSE
  )

  expect_s3_class(result, "data.frame")
  expect_identical(names(result), c("object_type", "rdz", "base", "base_uncompressed"))
  expect_identical(result$object_type, "integer")
  expect_true(all(is.finite(unlist(result[c("rdz", "base", "base_uncompressed")]))))
  expect_identical(attr(result, "unit"), "milliseconds")
  expect_identical(attr(result, "parameters")$threads, 1L)
  expect_identical(
    unname(attr(result, "parameters")$checksum_policy[["rdz"]]),
    "always validate CRC32"
  )

  details <- attr(result, "details")
  expect_s3_class(details, "data.frame")
  expect_identical(details$format, c("rdz", "base", "base_uncompressed"))
  expect_true(all(details$status == "ok"))
  expect_equal(
    details$median_ms,
    unlist(result[c("rdz", "base", "base_uncompressed")]),
    ignore_attr = TRUE
  )
})

test_that("fst benchmarks atomic vectors through one-column data frames", {
  skip_if_not_installed("bench")
  skip_if_not_installed("fst")

  benchmark_helper <- test_path("..", "..", "tools", "bench-helpers.R")
  skip_if_not(
    file.exists(benchmark_helper),
    "development benchmark helper is not available in the installed package"
  )

  benchmark_environment <- new.env(parent = globalenv())
  sys.source(benchmark_helper, envir = benchmark_environment)
  previous_threads <- fst::threads_fst()

  result <- benchmark_environment$benchmark_read_matrix(
    objects = list(
      integer = c(1L, NA_integer_, 3L),
      factor = factor(c("a", NA_character_, "b"))
    ),
    formats = "fst",
    iterations = 1L,
    progress = FALSE
  )

  expect_true(all(is.finite(result$fst)))
  expect_true(all(attr(result, "details")$status == "ok"))
  expect_identical(fst::threads_fst(), previous_threads)
})

test_that("paired benchmark reports latency throughput size and allocations", {
  skip_if_not_installed("bench")

  benchmark_helper <- test_path("..", "..", "tools", "bench-helpers.R")
  skip_if_not(file.exists(benchmark_helper), "development benchmark helper is unavailable")
  benchmark_environment <- new.env(parent = globalenv())
  sys.source(benchmark_helper, envir = benchmark_environment)

  result <- benchmark_environment$benchmark_serialization_matrix(
    objects = list(integer = c(1L, NA_integer_, 3L)),
    formats = c("rdz", "base_uncompressed"),
    iterations = 1L,
    warmups = 1L,
    memory = TRUE,
    progress = FALSE
  )

  expect_s3_class(result, "rdz_benchmark_result")
  expect_identical(names(result$read_ms), c("object_type", "rdz", "base_uncompressed"))
  expect_identical(names(result$write_ms), names(result$read_ms))
  expect_true(all(is.finite(unlist(result$read_ms[-1L]))))
  expect_true(all(is.finite(unlist(result$write_ms[-1L]))))
  expect_true(all(is.finite(unlist(result$file_mib[-1L]))))
  expect_true(all(is.finite(unlist(result$file_to_object_ratio[-1L]))))
  expect_true(all(is.finite(unlist(result$file_ratio_to_base_uncompressed[-1L]))))
  expect_true(all(is.finite(unlist(result$read_file_mib_per_sec[-1L]))))
  expect_true(all(is.finite(unlist(result$write_object_mib_per_sec[-1L]))))
  expect_true(all(is.finite(unlist(result$read_alloc_mib[-1L]))))
  expect_true(all(is.finite(unlist(result$write_alloc_mib[-1L]))))
  expect_identical(unique(result$details$operation), c("read", "write"))
  expect_true(all(result$details$status == "ok"))
  expect_identical(result$parameters$read$warmups, 1L)
  expect_identical(result$backend_settings$threads, c(1L, 1L))
  expect_true(all(c("cpu_model", "memory_bytes", "rustc_version") %in% names(result$environment)))
})

test_that("cold-cache reads require and invoke a labeled hook outside timing", {
  skip_if_not_installed("bench")

  benchmark_helper <- test_path("..", "..", "tools", "bench-helpers.R")
  skip_if_not(file.exists(benchmark_helper), "development benchmark helper is unavailable")
  benchmark_environment <- new.env(parent = globalenv())
  sys.source(benchmark_helper, envir = benchmark_environment)

  expect_error(
    benchmark_environment$benchmark_read_matrix(
      objects = list(integer = 1:3),
      formats = "base_uncompressed",
      iterations = 1L,
      cache_mode = "cold_hook",
      progress = FALSE
    ),
    "cold_cache_hook"
  )

  calls <- 0L
  result <- benchmark_environment$benchmark_read_matrix(
    objects = list(integer = 1:3),
    formats = "base_uncompressed",
    iterations = 2L,
    warmups = 0L,
    cache_mode = "cold_hook",
    cold_cache_hook = function(path) {
      calls <<- calls + 1L
      invisible(file.info(path)$size)
    },
    cold_cache_label = "test hook; no cache eviction",
    progress = FALSE
  )

  expect_identical(calls, 2L)
  expect_identical(attr(result, "parameters")$cache_mode, "cold_hook")
  expect_identical(
    attr(result, "parameters")$cold_cache_label,
    "test hook; no cache eviction"
  )
})

test_that("multi-size and expanded suites expose required workload families", {
  skip_if_not_installed("bench")

  benchmark_helper <- test_path("..", "..", "tools", "bench-helpers.R")
  skip_if_not(file.exists(benchmark_helper), "development benchmark helper is unavailable")
  benchmark_environment <- new.env(parent = globalenv())
  sys.source(benchmark_helper, envir = benchmark_environment)

  suite <- benchmark_environment$benchmark_serialization_suite(
    sizes = c(tiny = 3L, small = 5L),
    formats = "base_uncompressed",
    iterations = 1L,
    warmups = 0L,
    memory = FALSE,
    progress = FALSE
  )

  expect_s3_class(suite, "rdz_benchmark_suite")
  expect_identical(suite$size_labels, c("tiny", "small"))
  expect_true(all(c("size_label", "target_size") %in% names(suite$details)))

  expanded <- benchmark_environment$benchmark_read_matrix(
    size = 5L,
    object_suite = "expanded",
    formats = "base_uncompressed",
    iterations = 1L,
    warmups = 0L,
    progress = FALSE
  )
  expect_true(all(c(
    "logical_sparse",
    "integer_sequential",
    "numeric_exceptions",
    "character_high_cardinality",
    "factor_high_cardinality",
    "data_frame_mixed"
  ) %in% expanded$object_type))
})
