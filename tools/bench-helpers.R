benchmark_command_output <- function(command, args = character()) {
  tryCatch(
    {
      output <- suppressWarnings(system2(command, args, stdout = TRUE, stderr = FALSE))
      if (!is.null(attr(output, "status")) && attr(output, "status") != 0L) {
        return(NA_character_)
      }
      value <- paste(output, collapse = "\n")
      if (nzchar(value)) value else NA_character_
    },
    error = function(condition) NA_character_
  )
}

benchmark_darwin_hardware <- function() {
  output <- benchmark_command_output("system_profiler", "SPHardwareDataType")
  if (is.na(output)) {
    return(list(
      cpu_model = NA_character_,
      memory_bytes = NA_real_,
      physical_cores = NA_integer_
    ))
  }
  lines <- trimws(strsplit(output, "\n", fixed = TRUE)[[1L]])
  cpu <- grep("^(Chip|Processor Name):", lines, value = TRUE)
  speed <- grep("^Processor Speed:", lines, value = TRUE)
  memory <- grep("^Memory:", lines, value = TRUE)
  cores <- grep("^Total Number of Cores:", lines, value = TRUE)
  cpu_model <- if (length(cpu)) {
    sub("^[^:]+:[[:space:]]*", "", cpu[[1L]])
  } else {
    NA_character_
  }
  if (length(speed)) {
    cpu_model <- paste(
      cpu_model,
      sub("^[^:]+:[[:space:]]*", "", speed[[1L]])
    )
  }
  memory_bytes <- if (length(memory)) {
    value <- strsplit(sub("^Memory:[[:space:]]*", "", memory[[1L]]), "[[:space:]]+")[[1L]]
    if (length(value) >= 2L) {
      multiplier <- switch(
        toupper(value[[2L]]),
        KB = 1024,
        MB = 1024^2,
        GB = 1024^3,
        TB = 1024^4,
        NA_real_
      )
      suppressWarnings(as.numeric(value[[1L]]) * multiplier)
    } else {
      NA_real_
    }
  } else {
    NA_real_
  }
  physical_cores <- if (length(cores)) {
    suppressWarnings(as.integer(sub("^[^:]+:[[:space:]]*", "", cores[[1L]])))
  } else {
    NA_integer_
  }
  list(
    cpu_model = cpu_model,
    memory_bytes = memory_bytes,
    physical_cores = physical_cores
  )
}

benchmark_machine_metadata <- function(dir) {
  system <- Sys.info()
  os <- unname(system[["sysname"]])
  darwin_hardware <- if (identical(os, "Darwin")) {
    benchmark_darwin_hardware()
  } else {
    list(
      cpu_model = NA_character_,
      memory_bytes = NA_real_,
      physical_cores = NA_integer_
    )
  }
  cpu_model <- switch(
    os,
    Darwin = {
      value <- benchmark_command_output("sysctl", c("-n", "machdep.cpu.brand_string"))
      if (is.na(value)) darwin_hardware$cpu_model else value
    },
    Linux = {
      cpu_lines <- tryCatch(readLines("/proc/cpuinfo", warn = FALSE), error = function(e) character())
      model <- sub("^[^:]+:[[:space:]]*", "", grep("^model name", cpu_lines, value = TRUE))
      if (length(model)) model[[1L]] else unname(system[["machine"]])
    },
    Windows = Sys.getenv("PROCESSOR_IDENTIFIER", unname(system[["machine"]])),
    unname(system[["machine"]])
  )
  if (is.na(cpu_model) || !nzchar(cpu_model)) {
    cpu_model <- unname(system[["machine"]])
  }
  memory_bytes <- switch(
    os,
    Darwin = {
      value <- suppressWarnings(as.numeric(benchmark_command_output("sysctl", c("-n", "hw.memsize"))))
      if (is.na(value)) darwin_hardware$memory_bytes else value
    },
    Linux = {
      memory_lines <- tryCatch(readLines("/proc/meminfo", warn = FALSE), error = function(e) character())
      total <- grep("^MemTotal:", memory_lines, value = TRUE)
      if (length(total)) {
        suppressWarnings(as.numeric(gsub("[^0-9]", "", total[[1L]])) * 1024)
      } else {
        NA_real_
      }
    },
    NA_real_
  )
  git_commit <- benchmark_command_output("git", c("rev-parse", "HEAD"))
  git_status <- benchmark_command_output("git", c("status", "--porcelain"))
  logical_cores <- parallel::detectCores(logical = TRUE)
  physical_cores <- parallel::detectCores(logical = FALSE)
  if (is.na(physical_cores) && identical(os, "Darwin")) {
    physical_cores <- darwin_hardware$physical_cores
  }

  list(
    timestamp_utc = format(Sys.time(), tz = "UTC", usetz = TRUE),
    bench_version = as.character(utils::packageVersion("bench")),
    r_version = as.character(getRversion()),
    platform = R.version$platform,
    os = os,
    os_release = unname(system[["release"]]),
    machine = unname(system[["machine"]]),
    cpu_model = cpu_model,
    logical_cores = logical_cores,
    physical_cores = physical_cores,
    memory_bytes = memory_bytes,
    rustc_version = benchmark_command_output("rustc", "--version"),
    cargo_version = benchmark_command_output("cargo", "--version"),
    filesystem_path = normalizePath(dir, winslash = "/", mustWork = TRUE),
    filesystem_description = benchmark_command_output("df", c("-P", normalizePath(dir))),
    git_commit = git_commit,
    git_dirty = !is.na(git_status) && nzchar(git_status)
  )
}

#' Benchmark one serialization operation across R formats
#'
#' Creates one file per object and format, validates one read outside the timed
#' expressions, and returns a wide object-type by format matrix. Cells contain
#' median operation time in milliseconds as measured by `bench::mark()`.
#'
#' Detailed long-form results are stored in `attr(result, "details")`; package
#' versions and run parameters are stored in attributes of the returned data
#' frame. Unsupported, unavailable, failed, or lossy combinations are `NA_real_`
#' in the matrix and carry an explanation in the details table.
#' Atomic-vector `fst` cases use `as.data.frame()` to write a one-column table
#' and extract that column after reading; list vectors remain unsupported.
#'
#' @param objects Named list of R objects. When `NULL`, representative logical,
#'   integer, numeric, character, factor, list, and data-frame cases are made
#'   outside the timed expressions.
#' @param size Number of elements or data-frame rows used by default cases.
#' @param seed Random seed used only when constructing default cases.
#' @param object_suite `representative` for the compact seven-case matrix or
#'   `expanded` for the type-specific distributions required by the validation
#'   methodology.
#' @param formats Formats to compare: `rdz`, `qs2`, `qdata`, `fst`, `base`, and
#'   `base_uncompressed`. The `base` backend uses the `saveRDS()` default gzip
#'   compression; `base_uncompressed` uses `compress = FALSE`.
#' @param iterations Fixed number of `bench::mark()` iterations per expression.
#' @param operation Operation to measure: `read` or `write`.
#' @param warmups Number of untimed operations performed after validation.
#' @param threads Equal thread budget used by backends that expose a thread
#'   setting. The default matrix is deliberately single-threaded.
#' @param dir Existing directory in which an isolated temporary run directory is
#'   created.
#' @param memory Whether `bench::mark()` should measure allocations.
#' @param filter_gc Whether iterations containing garbage collection are removed.
#' @param validate Whether to require an identical round trip before timing.
#' @param fail_on_error Whether unavailable or failed combinations stop the run.
#' @param progress Whether to emit one progress message per object.
#'
#' @param cache_mode Read-cache policy. `warm` uses repeated reads of the same
#'   prepared file. `cold_hook` invokes `cold_cache_hook(path)` outside every
#'   one-iteration timed read; the hook is responsible for genuinely evicting or
#'   bypassing the relevant cache.
#' @param cold_cache_hook Function used by `cache_mode = "cold_hook"`.
#' @param cold_cache_label Machine-readable description of the hook's behavior.
#' @param build_mode Description of how RDZ was built for this run.
#'
#' @return A data frame with object/case rows and format columns. Numeric cells
#'   are median operation milliseconds.
benchmark_operation_matrix <- function(
  operation = c("read", "write"),
  objects = NULL,
  size = 100000L,
  seed = 2026L,
  object_suite = c("representative", "expanded"),
  formats = c("rdz", "qs2", "qdata", "fst", "base", "base_uncompressed"),
  iterations = 10L,
  warmups = 1L,
  threads = 1L,
  dir = tempdir(),
  memory = FALSE,
  filter_gc = FALSE,
  validate = TRUE,
  fail_on_error = FALSE,
  progress = interactive(),
  cache_mode = c("warm", "cold_hook"),
  cold_cache_hook = NULL,
  cold_cache_label = NULL,
  build_mode = "caller-managed"
) {
  operation <- match.arg(operation)
  cache_mode <- match.arg(cache_mode)
  object_suite <- match.arg(object_suite)

  if (!requireNamespace("bench", quietly = TRUE)) {
    stop("Package `bench` is required to run this benchmark.", call. = FALSE)
  }

  is_whole_number <- function(x) {
    is.numeric(x) && length(x) == 1L && !is.na(x) && is.finite(x) &&
      x == floor(x)
  }

  if (!is_whole_number(size) || size < 1 || size > .Machine$integer.max) {
    stop("`size` must be a positive integer no larger than `.Machine$integer.max`.", call. = FALSE)
  }
  if (!is_whole_number(seed) || seed < 0 || seed > .Machine$integer.max) {
    stop("`seed` must be a non-negative integer no larger than `.Machine$integer.max`.", call. = FALSE)
  }
  if (!is_whole_number(iterations) || iterations < 1 || iterations > .Machine$integer.max) {
    stop("`iterations` must be a positive integer.", call. = FALSE)
  }
  if (!is_whole_number(warmups) || warmups < 0 || warmups > .Machine$integer.max) {
    stop("`warmups` must be a non-negative integer.", call. = FALSE)
  }
  if (!is_whole_number(threads) || threads < 1 || threads > .Machine$integer.max) {
    stop("`threads` must be a positive integer.", call. = FALSE)
  }
  if (operation == "write" && cache_mode != "warm") {
    stop("Cold-cache hooks apply only to read benchmarks.", call. = FALSE)
  }
  if (cache_mode == "cold_hook") {
    if (!is.function(cold_cache_hook)) {
      stop("`cold_cache_hook` must be a function for cold-cache reads.", call. = FALSE)
    }
    if (!is.character(cold_cache_label) || length(cold_cache_label) != 1L ||
        is.na(cold_cache_label) || !nzchar(cold_cache_label)) {
      stop("`cold_cache_label` must describe the cold-cache hook.", call. = FALSE)
    }
  }
  if (!is.character(dir) || length(dir) != 1L || is.na(dir) || !dir.exists(dir)) {
    stop("`dir` must name one existing directory.", call. = FALSE)
  }
  if (!is.character(build_mode) || length(build_mode) != 1L ||
      is.na(build_mode) || !nzchar(build_mode)) {
    stop("`build_mode` must be a non-empty string.", call. = FALSE)
  }
  for (argument in c("memory", "filter_gc", "validate", "fail_on_error", "progress")) {
    value <- get(argument, inherits = FALSE)
    if (!is.logical(value) || length(value) != 1L || is.na(value)) {
      stop("`", argument, "` must be `TRUE` or `FALSE`.", call. = FALSE)
    }
  }

  known_formats <- c("rdz", "qs2", "qdata", "fst", "base", "base_uncompressed")
  if (!is.character(formats) || !length(formats) || anyNA(formats) ||
      any(!nzchar(formats)) || anyDuplicated(formats)) {
    stop("`formats` must be a non-empty character vector without missing or duplicate values.", call. = FALSE)
  }
  unknown_formats <- setdiff(formats, known_formats)
  if (length(unknown_formats)) {
    stop(
      "Unknown format", if (length(unknown_formats) == 1L) "" else "s", ": ",
      paste(unknown_formats, collapse = ", "),
      call. = FALSE
    )
  }

  size <- as.integer(size)
  seed <- as.integer(seed)
  iterations <- as.integer(iterations)
  warmups <- as.integer(warmups)
  threads <- as.integer(threads)

  using_default_objects <- is.null(objects)
  if (using_default_objects) {
    had_seed <- exists(".Random.seed", envir = .GlobalEnv, inherits = FALSE)
    if (had_seed) {
      previous_seed <- get(".Random.seed", envir = .GlobalEnv, inherits = FALSE)
    }
    on.exit({
      if (had_seed) {
        assign(".Random.seed", previous_seed, envir = .GlobalEnv)
      } else if (exists(".Random.seed", envir = .GlobalEnv, inherits = FALSE)) {
        rm(".Random.seed", envir = .GlobalEnv)
      }
    }, add = TRUE)

    set.seed(seed)
    logical_value <- sample(
      c(FALSE, TRUE, NA),
      size,
      replace = TRUE,
      prob = c(0.49, 0.49, 0.02)
    )
    integer_value <- sample.int(max(100L, min(size, 100000L)), size, replace = TRUE)
    numeric_value <- stats::runif(size, min = -10000, max = 10000)
    character_value <- sample(
      c(sprintf("value-%03d", seq_len(100L)), NA_character_),
      size,
      replace = TRUE
    )
    factor_value <- factor(character_value)

    objects <- list(
      logical = logical_value,
      integer = integer_value,
      numeric = numeric_value,
      character = character_value,
      factor = factor_value,
      list = list(
        logical = logical_value,
        integer = integer_value,
        numeric = numeric_value,
        character = character_value
      ),
      data_frame = data.frame(
        logical = logical_value,
        integer = integer_value,
        numeric = numeric_value,
        character = character_value,
        factor = factor_value,
        stringsAsFactors = FALSE
      )
    )

    if (object_suite == "expanded") {
      logical_sparse <- sample(
        c(FALSE, TRUE, NA),
        size,
        replace = TRUE,
        prob = c(0.985, 0.01, 0.005)
      )
      logical_runs <- rep(
        c(FALSE, TRUE, NA),
        each = max(1L, as.integer(ceiling(size / 3)))
      )[seq_len(size)]
      logical_mostly_true <- sample(
        c(FALSE, TRUE, NA),
        size,
        replace = TRUE,
        prob = c(0.01, 0.985, 0.005)
      )
      logical_mostly_missing <- sample(
        c(FALSE, TRUE, NA),
        size,
        replace = TRUE,
        prob = c(0.01, 0.01, 0.98)
      )
      logical_alternating <- rep(c(FALSE, TRUE, NA), length.out = size)
      integer_sequential <- seq_len(size)
      integer_full_range <- as.integer(floor(stats::runif(
        size,
        min = -.Machine$integer.max,
        max = .Machine$integer.max
      )))
      integer_low_cardinality <- sample(c(-1L, 0L, 1L, NA_integer_), size, replace = TRUE)
      integer_missing <- integer_value
      integer_missing[seq.int(1L, size, by = 20L)] <- NA_integer_
      integer_all_missing <- rep(NA_integer_, size)
      numeric_rounded <- round(numeric_value, digits = 2L)
      numeric_currency <- round(stats::runif(size, min = -100000, max = 100000), digits = 2L)
      numeric_monotonic <- cumsum(stats::runif(size, min = 0, max = 1))
      numeric_repeated <- sample(c(-1, 0, 1, pi, NA_real_), size, replace = TRUE)
      numeric_exceptions <- rep(
        c(NA_real_, NaN, Inf, -Inf, -0, 0, .Machine$double.xmin),
        length.out = size
      )
      numeric_missing <- numeric_value
      numeric_missing[seq.int(1L, size, by = 20L)] <- NA_real_
      numeric_all_missing <- rep(NA_real_, size)
      character_high_cardinality <- sprintf("id-%010d", seq_len(size))
      character_repeated_long <- sample(
        c(strrep("alpha", 40L), strrep("beta", 50L), NA_character_),
        size,
        replace = TRUE
      )
      character_unique_long <- paste0(character_high_cardinality, "-", strrep("x", 64L))
      factor_high_cardinality <- factor(character_high_cardinality)
      latin1 <- iconv("Ärende", from = "UTF-8", to = "latin1")
      utf8 <- enc2utf8("日本語")
      bytes <- rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9)))
      Encoding(bytes) <- "bytes"
      character_mixed <- sample(
        c("", "short", strrep("long", 64L), latin1, utf8, bytes, NA_character_),
        size,
        replace = TRUE
      )
      many_narrow_columns <- stats::setNames(
        rep(list(integer_low_cardinality), 20L),
        sprintf("column_%02d", seq_len(20L))
      )

      objects <- list(
        logical_random = logical_value,
        logical_sparse = logical_sparse,
        logical_mostly_true = logical_mostly_true,
        logical_mostly_missing = logical_mostly_missing,
        logical_runs = logical_runs,
        logical_alternating = logical_alternating,
        integer_random = integer_value,
        integer_full_range = integer_full_range,
        integer_sequential = integer_sequential,
        integer_low_cardinality = integer_low_cardinality,
        integer_missing = integer_missing,
        integer_all_missing = integer_all_missing,
        numeric_random = numeric_value,
        numeric_rounded = numeric_rounded,
        numeric_currency = numeric_currency,
        numeric_monotonic = numeric_monotonic,
        numeric_repeated = numeric_repeated,
        numeric_exceptions = numeric_exceptions,
        numeric_missing = numeric_missing,
        numeric_all_missing = numeric_all_missing,
        character_low_cardinality = character_value,
        character_high_cardinality = character_high_cardinality,
        character_repeated_long = character_repeated_long,
        character_unique_long = character_unique_long,
        character_mixed = character_mixed,
        factor_low_cardinality = factor_value,
        factor_high_cardinality = factor_high_cardinality,
        list_mixed = list(
          logical = logical_value,
          integer = integer_value,
          numeric = numeric_value,
          character = character_value
        ),
        data_frame_numeric = data.frame(
          random = numeric_value,
          rounded = numeric_rounded,
          monotonic = numeric_monotonic
        ),
        data_frame_character = data.frame(
          low_cardinality = character_value,
          high_cardinality = character_high_cardinality,
          factor_low = factor_value,
          factor_high = factor_high_cardinality,
          stringsAsFactors = FALSE
        ),
        data_frame_mixed = data.frame(
          logical = logical_value,
          integer = integer_value,
          numeric = numeric_value,
          character = character_value,
          factor = factor_value,
          stringsAsFactors = FALSE
        ),
        data_frame_many_narrow = as.data.frame(
          many_narrow_columns,
          optional = TRUE,
          stringsAsFactors = FALSE
        )
      )
    }
  }

  if (!is.list(objects) || !length(objects)) {
    stop("`objects` must be a non-empty named list.", call. = FALSE)
  }
  object_names <- names(objects)
  if (is.null(object_names) || anyNA(object_names) || any(!nzchar(object_names)) ||
      anyDuplicated(object_names)) {
    stop("`objects` must have unique, non-empty, non-missing names.", call. = FALSE)
  }

  r_object_types <- vapply(objects, function(object) {
    if (is.data.frame(object)) {
      "data_frame"
    } else if (is.factor(object)) {
      "factor"
    } else if (is.list(object)) {
      "list"
    } else if (is.logical(object)) {
      "logical"
    } else if (is.integer(object)) {
      "integer"
    } else if (is.double(object)) {
      "numeric"
    } else if (is.character(object)) {
      "character"
    } else {
      typeof(object)
    }
  }, character(1))
  distributions <- if (!using_default_objects) {
    rep("custom", length(objects))
  } else if (object_suite == "representative") {
    rep("representative", length(objects))
  } else {
    vapply(seq_along(objects), function(index) {
      prefix <- paste0(r_object_types[[index]], "_")
      if (startsWith(object_names[[index]], prefix)) {
        substring(object_names[[index]], nchar(prefix) + 1L)
      } else {
        object_names[[index]]
      }
    }, character(1))
  }
  case_metadata <- data.frame(
    object_type = object_names,
    r_object_type = unname(r_object_types),
    distribution = distributions,
    element_count = vapply(objects, length, numeric(1)),
    row_count = vapply(objects, function(object) {
      if (is.data.frame(object)) nrow(object) else NA_real_
    }, numeric(1)),
    column_count = vapply(objects, function(object) {
      if (is.data.frame(object)) ncol(object) else NA_real_
    }, numeric(1)),
    object_size_bytes = vapply(objects, function(object) {
      as.numeric(utils::object.size(object))
    }, numeric(1)),
    stringsAsFactors = FALSE
  )

  backends <- list(
    rdz = list(
      package = "rdz",
      extension = ".rdz",
      supports = function(x) TRUE,
      write = function(x, path) rdz::write_rdz(x, path),
      write_call = function(path, template) {
        substitute(rdz::write_rdz(object, PATH), list(PATH = path))
      },
      read = function(path, template) rdz::read_rdz(path),
      read_call = function(path, template) substitute(rdz::read_rdz(PATH), list(PATH = path))
    ),
    qs2 = list(
      package = "qs2",
      extension = ".qs2",
      supports = function(x) TRUE,
      write = function(x, path) qs2::qs_save(x, path, nthreads = threads),
      write_call = function(path, template) {
        substitute(
          qs2::qs_save(object, PATH, nthreads = THREADS),
          list(PATH = path, THREADS = threads)
        )
      },
      read = function(path, template) {
        qs2::qs_read(path, validate_checksum = TRUE, nthreads = threads)
      },
      read_call = function(path, template) {
        substitute(
          qs2::qs_read(PATH, validate_checksum = TRUE, nthreads = THREADS),
          list(PATH = path, THREADS = threads)
        )
      }
    ),
    qdata = list(
      package = "qs2",
      extension = ".qdata",
      supports = function(x) TRUE,
      write = function(x, path) qs2::qd_save(x, path, nthreads = threads),
      write_call = function(path, template) {
        substitute(
          qs2::qd_save(object, PATH, nthreads = THREADS),
          list(PATH = path, THREADS = threads)
        )
      },
      read = function(path, template) {
        qs2::qd_read(path, validate_checksum = TRUE, nthreads = threads)
      },
      read_call = function(path, template) {
        substitute(
          qs2::qd_read(PATH, validate_checksum = TRUE, nthreads = THREADS),
          list(PATH = path, THREADS = threads)
        )
      }
    ),
    fst = list(
      package = "fst",
      extension = ".fst",
      supports = function(x) {
        is.data.frame(x) || (
          is.null(dim(x)) &&
            (
              is.logical(x) || is.integer(x) || is.double(x) ||
                is.character(x) || is.factor(x)
            )
        )
      },
      write = function(x, path) {
        value <- if (is.data.frame(x)) x else as.data.frame(x)
        fst::write_fst(value, path)
      },
      write_call = function(path, template) {
        if (is.data.frame(template)) {
          substitute(fst::write_fst(object, PATH), list(PATH = path))
        } else {
          substitute(fst::write_fst(as.data.frame(object), PATH), list(PATH = path))
        }
      },
      read = function(path, template) {
        value <- fst::read_fst(path)
        if (is.data.frame(template)) value else value[[1L]]
      },
      read_call = function(path, template) {
        if (is.data.frame(template)) {
          substitute(fst::read_fst(PATH), list(PATH = path))
        } else {
          substitute(fst::read_fst(PATH)[[1L]], list(PATH = path))
        }
      }
    ),
    base = list(
      package = "base",
      extension = ".rds",
      supports = function(x) TRUE,
      write = function(x, path) base::saveRDS(x, path),
      write_call = function(path, template) {
        substitute(base::saveRDS(object, PATH), list(PATH = path))
      },
      read = function(path, template) base::readRDS(path),
      read_call = function(path, template) substitute(base::readRDS(PATH), list(PATH = path))
    ),
    base_uncompressed = list(
      package = "base",
      extension = ".uncompressed.rds",
      supports = function(x) TRUE,
      write = function(x, path) base::saveRDS(x, path, compress = FALSE),
      write_call = function(path, template) {
        substitute(base::saveRDS(object, PATH, compress = FALSE), list(PATH = path))
      },
      read = function(path, template) base::readRDS(path),
      read_call = function(path, template) substitute(base::readRDS(PATH), list(PATH = path))
    )
  )

  package_available <- vapply(formats, function(format) {
    package <- backends[[format]]$package
    package == "base" || requireNamespace(package, quietly = TRUE)
  }, logical(1))

  package_versions <- vapply(formats, function(format) {
    package <- backends[[format]]$package
    if (package == "base") {
      return(as.character(getRversion()))
    }
    if (!package_available[[format]]) {
      return(NA_character_)
    }
    as.character(utils::packageVersion(package))
  }, character(1))

  unavailable <- formats[!package_available]
  if (length(unavailable) && fail_on_error) {
    stop(
      "Unavailable benchmark format", if (length(unavailable) == 1L) "" else "s", ": ",
      paste(unavailable, collapse = ", "),
      call. = FALSE
    )
  }

  if ("fst" %in% formats && package_available[["fst"]]) {
    previous_fst_threads <- fst::threads_fst()
    fst::threads_fst(threads)
    on.exit(fst::threads_fst(previous_fst_threads), add = TRUE)
  }

  run_dir <- tempfile(
    pattern = paste0("rdz-", operation, "-matrix-"),
    tmpdir = path.expand(dir)
  )
  if (!dir.create(run_dir, recursive = FALSE)) {
    stop("Failed to create benchmark directory: ", run_dir, call. = FALSE)
  }
  on.exit(unlink(run_dir, recursive = TRUE, force = TRUE), add = TRUE)

  matrix <- data.frame(object_type = object_names, stringsAsFactors = FALSE)
  for (format in formats) {
    matrix[[format]] <- NA_real_
  }

  details <- list()

  add_detail <- function(
    object_type,
    format,
    status,
    reason = NA_character_,
    warning = NA_character_,
    file_size = NA_real_
  ) {
    case_index <- match(object_type, case_metadata$object_type)
    details[[length(details) + 1L]] <<- data.frame(
      object_type = object_type,
      r_object_type = case_metadata$r_object_type[[case_index]],
      distribution = case_metadata$distribution[[case_index]],
      element_count = case_metadata$element_count[[case_index]],
      row_count = case_metadata$row_count[[case_index]],
      column_count = case_metadata$column_count[[case_index]],
      format = format,
      package = backends[[format]]$package,
      package_version = unname(package_versions[[format]]),
      operation = operation,
      status = status,
      median_seconds = NA_real_,
      median_ms = NA_real_,
      itr_per_sec = NA_real_,
      mem_alloc_bytes = NA_real_,
      n_itr = NA_integer_,
      n_gc = NA_integer_,
      file_size_bytes = file_size,
      object_size_bytes = case_metadata$object_size_bytes[[case_index]],
      file_to_object_ratio = NA_real_,
      file_mib_per_sec = NA_real_,
      object_mib_per_sec = NA_real_,
      warning = warning,
      reason = reason,
      stringsAsFactors = FALSE
    )
    length(details)
  }

  for (object_index in seq_along(objects)) {
    object_type <- object_names[[object_index]]
    object <- objects[[object_index]]
    object_size <- as.numeric(utils::object.size(object))
    if (progress) {
      message("Benchmarking ", operation, " for `", object_type, "`...")
    }

    object_dir <- file.path(run_dir, sprintf("%02d-%s", object_index, make.names(object_type)))
    if (!dir.create(object_dir, recursive = FALSE)) {
      stop("Failed to create benchmark case directory: ", object_dir, call. = FALSE)
    }

    benchmark_calls <- list()
    benchmark_paths <- list()
    detail_indices <- integer()

    for (format in formats) {
      backend <- backends[[format]]

      if (!package_available[[format]]) {
        add_detail(object_type, format, "unavailable", "required package is not installed")
        next
      }
      if (!isTRUE(backend$supports(object))) {
        add_detail(object_type, format, "unsupported", "format does not support this R object type")
        next
      }

      path <- file.path(object_dir, paste0(format, backend$extension))
      warnings <- character()
      setup_error <- NULL
      valid <- tryCatch(
        withCallingHandlers(
          {
            backend$write(object, path)
            restored <- backend$read(path, object)
            if (validate && !identical(restored, object)) {
              stop("round trip was not identical", call. = FALSE)
            }
            if (warmups > 0L) {
              for (warmup in seq_len(warmups)) {
                if (operation == "read") {
                  backend$read(path, object)
                } else {
                  backend$write(object, path)
                }
              }
            }
            TRUE
          },
          warning = function(condition) {
            warnings <<- c(warnings, conditionMessage(condition))
            invokeRestart("muffleWarning")
          }
        ),
        error = function(condition) {
          setup_error <<- conditionMessage(condition)
          FALSE
        }
      )

      warning_text <- if (length(warnings)) {
        paste(unique(warnings), collapse = " | ")
      } else {
        NA_character_
      }

      if (!valid) {
        status <- if (identical(setup_error, "round trip was not identical")) {
          "invalid"
        } else {
          "error"
        }
        add_detail(object_type, format, status, setup_error, warning_text)
        if (fail_on_error) {
          stop(
            "Benchmark setup failed for `", object_type, "` with `", format,
            "`: ", setup_error,
            call. = FALSE
          )
        }
        next
      }

      detail_index <- add_detail(
        object_type,
        format,
        "ready",
        warning = warning_text,
        file_size = unname(file.info(path, extra_cols = FALSE)$size)
      )
      detail_indices[[format]] <- detail_index
      benchmark_paths[[format]] <- path
      benchmark_calls[[format]] <- if (operation == "read") {
        backend$read_call(path, object)
      } else {
        backend$write_call(path, object)
      }
      details[[detail_index]]$object_size_bytes <- object_size
      details[[detail_index]]$file_to_object_ratio <-
        details[[detail_index]]$file_size_bytes / object_size
    }

    if (!length(benchmark_calls)) {
      next
    }

    mark_error <- NULL
    measurements <- if (cache_mode == "warm") {
      mark_arguments <- c(
        benchmark_calls,
        list(
          iterations = iterations,
          check = FALSE,
          memory = memory,
          filter_gc = filter_gc
        )
      )
      mark_call <- as.call(c(list(quote(bench::mark)), mark_arguments))
      mark <- tryCatch(
        eval(mark_call, envir = environment()),
        error = function(condition) {
          mark_error <<- conditionMessage(condition)
          NULL
        }
      )
      if (is.null(mark)) {
        NULL
      } else {
        data.frame(
          format = as.character(mark$expression),
          median_seconds = vapply(mark$median, as.numeric, numeric(1)),
          itr_per_sec = as.numeric(mark[["itr/sec"]]),
          mem_alloc_bytes = if ("mem_alloc" %in% names(mark)) {
            vapply(mark$mem_alloc, as.numeric, numeric(1))
          } else {
            rep(NA_real_, nrow(mark))
          },
          n_itr = as.integer(mark$n_itr),
          n_gc = as.integer(mark$n_gc),
          stringsAsFactors = FALSE
        )
      }
    } else {
      cold_rows <- vector("list", length(benchmark_calls))
      names(cold_rows) <- names(benchmark_calls)
      for (format in names(benchmark_calls)) {
        seconds <- numeric(iterations)
        allocations <- numeric(iterations)
        gc_count <- integer(iterations)
        for (iteration in seq_len(iterations)) {
          cold_cache_hook(benchmark_paths[[format]])
          one_arguments <- c(
            setNames(list(benchmark_calls[[format]]), format),
            list(
              iterations = 1L,
              check = FALSE,
              memory = memory,
              filter_gc = FALSE
            )
          )
          one_call <- as.call(c(list(quote(bench::mark)), one_arguments))
          one <- tryCatch(
            eval(one_call, envir = environment()),
            error = function(condition) {
              mark_error <<- conditionMessage(condition)
              NULL
            }
          )
          if (is.null(one)) {
            break
          }
          seconds[[iteration]] <- as.numeric(one$median[[1L]])
          allocations[[iteration]] <- if ("mem_alloc" %in% names(one)) {
            as.numeric(one$mem_alloc[[1L]])
          } else {
            NA_real_
          }
          gc_count[[iteration]] <- as.integer(one$n_gc[[1L]])
        }
        if (!is.null(mark_error)) {
          break
        }
        median_seconds <- stats::median(seconds)
        median_allocation <- if (all(is.na(allocations))) {
          NA_real_
        } else {
          stats::median(allocations, na.rm = TRUE)
        }
        cold_rows[[format]] <- data.frame(
          format = format,
          median_seconds = median_seconds,
          itr_per_sec = 1 / median_seconds,
          mem_alloc_bytes = median_allocation,
          n_itr = iterations,
          n_gc = sum(gc_count),
          stringsAsFactors = FALSE
        )
      }
      if (!is.null(mark_error)) NULL else do.call(rbind, cold_rows)
    }

    if (is.null(measurements)) {
      for (format in names(benchmark_calls)) {
        detail_index <- unname(detail_indices[[format]])
        details[[detail_index]]$status <- "error"
        details[[detail_index]]$reason <- mark_error
      }
      if (fail_on_error) {
        stop("`bench::mark()` failed for `", object_type, "`: ", mark_error, call. = FALSE)
      }
      next
    }

    for (mark_index in seq_len(nrow(measurements))) {
      format <- measurements$format[[mark_index]]
      detail_index <- unname(detail_indices[[format]])
      median_seconds <- measurements$median_seconds[[mark_index]]
      median_ms <- median_seconds * 1000
      file_size <- details[[detail_index]]$file_size_bytes

      matrix[[format]][[object_index]] <- median_ms
      details[[detail_index]]$status <- "ok"
      details[[detail_index]]$median_seconds <- median_seconds
      details[[detail_index]]$median_ms <- median_ms
      details[[detail_index]]$itr_per_sec <- measurements$itr_per_sec[[mark_index]]
      details[[detail_index]]$mem_alloc_bytes <- measurements$mem_alloc_bytes[[mark_index]]
      details[[detail_index]]$n_itr <- measurements$n_itr[[mark_index]]
      details[[detail_index]]$n_gc <- measurements$n_gc[[mark_index]]
      details[[detail_index]]$file_mib_per_sec <- file_size / 1024^2 / median_seconds
      details[[detail_index]]$object_mib_per_sec <- object_size / 1024^2 / median_seconds
    }
  }

  details <- do.call(rbind, details)
  rownames(details) <- NULL

  attr(matrix, "unit") <- "milliseconds"
  attr(matrix, "operation") <- operation
  attr(matrix, "details") <- details
  attr(matrix, "case_metadata") <- case_metadata
  attr(matrix, "package_versions") <- package_versions
  attr(matrix, "environment") <- benchmark_machine_metadata(dir)
  attr(matrix, "backend_settings") <- data.frame(
    format = formats,
    threads = vapply(formats, function(format) {
      if (format %in% c("qs2", "qdata", "fst")) threads else 1L
    }, integer(1)),
    preset = c(
      rdz = "auto: native logical; otherwise whole-root XDR",
      qs2 = if ("qs2" %in% names(package_available) && package_available[["qs2"]]) {
        paste0(
          "compress_level=", qs2::qopt("compress_level"),
          "; shuffle=", qs2::qopt("shuffle")
        )
      } else {
        NA_character_
      },
      qdata = if ("qdata" %in% names(package_available) && package_available[["qdata"]]) {
        paste0(
          "compress_level=", qs2::qopt("compress_level"),
          "; shuffle=", qs2::qopt("shuffle")
        )
      } else {
        NA_character_
      },
      fst = "compress=50; uniform_encoding=TRUE",
      base = "saveRDS default gzip",
      base_uncompressed = "saveRDS compress=FALSE"
    )[formats],
    compression = c(
      rdz = "none",
      qs2 = if ("qs2" %in% names(package_available) && package_available[["qs2"]]) {
        paste0("qs2 level ", qs2::qopt("compress_level"))
      } else {
        NA_character_
      },
      qdata = if ("qdata" %in% names(package_available) && package_available[["qdata"]]) {
        paste0("qdata level ", qs2::qopt("compress_level"))
      } else {
        NA_character_
      },
      fst = "fst compress = 50",
      base = "gzip default",
      base_uncompressed = "none"
    )[formats],
    checksum = c(
      rdz = "always validate IEEE CRC32",
      qs2 = "validate_checksum = TRUE",
      qdata = "validate_checksum = TRUE",
      fst = "format default",
      base = "format default",
      base_uncompressed = "format default"
    )[formats],
    adapter = c(
      rdz = "native logical or whole-root R XDR fallback",
      qs2 = "whole R object",
      qdata = "supported R subset",
      fst = "data frame; atomic vectors use one-column as.data.frame()",
      base = "whole R object",
      base_uncompressed = "whole R object"
    )[formats],
    stringsAsFactors = FALSE,
    row.names = NULL
  )
  attr(matrix, "parameters") <- list(
    operation = operation,
    iterations = iterations,
    warmups = warmups,
    threads = threads,
    thread_comparison = if (threads == 1L) {
      "equal single-thread budget"
    } else {
      "competitor scaling diagnostic; RDZ remains single-threaded"
    },
    build_mode = build_mode,
    memory = memory,
    filter_gc = filter_gc,
    validate = validate,
    cache_mode = cache_mode,
    cold_cache_label = if (cache_mode == "cold_hook") cold_cache_label else NA_character_,
    memory_measure = if (memory) {
      "bench::mark mem_alloc allocation proxy; not peak RSS"
    } else {
      "disabled"
    },
    warmup_policy = "validated setup followed by untimed public-API operations",
    write_target_policy = if (operation == "write") {
      "steady overwrite of an existing validated file"
    } else {
      NA_character_
    },
    throughput_definitions = c(
      file_mib_per_sec = "stored file bytes divided by median elapsed time",
      object_mib_per_sec = "utils::object.size input bytes divided by median elapsed time"
    ),
    size = if (using_default_objects) size else NA_integer_,
    object_suite = if (using_default_objects) object_suite else "custom",
    seed = if (using_default_objects) seed else NA_integer_,
    checksum_policy = c(
      rdz = "always validate CRC32",
      qs2 = "validate_checksum = TRUE",
      qdata = "validate_checksum = TRUE",
      fst = "format default",
      base = "format default",
      base_uncompressed = "format default"
    )[formats]
  )
  matrix
}

benchmark_metric_matrix <- function(details, value, formats) {
  cases <- unique(details$object_type)
  matrix <- data.frame(object_type = cases, stringsAsFactors = FALSE)
  for (format in formats) {
    keys <- paste(details$object_type, details$format, sep = "\r")
    wanted <- paste(cases, format, sep = "\r")
    index <- match(wanted, keys)
    values <- details[[value]][index]
    values[details$status[index] != "ok"] <- NA_real_
    matrix[[format]] <- values
  }
  matrix
}

#' Benchmark paired serialization reads and writes
#'
#' Returns aligned matrices for latency, throughput, file size, and the
#' `bench::mark()` allocation proxy. Read and write setup and validation happen
#' outside timed expressions. A cold-cache hook, when supplied, applies only to
#' reads; writes always use the warm/steady overwrite policy.
benchmark_serialization_matrix <- function(...) {
  arguments <- list(...)
  arguments$operation <- NULL

  read_arguments <- arguments
  read_arguments$operation <- "read"
  read_matrix <- do.call(benchmark_operation_matrix, read_arguments)

  write_arguments <- arguments
  write_arguments$operation <- "write"
  write_arguments$cache_mode <- "warm"
  write_arguments$cold_cache_hook <- NULL
  write_arguments$cold_cache_label <- NULL
  write_matrix <- do.call(benchmark_operation_matrix, write_arguments)

  read_details <- attr(read_matrix, "details")
  write_details <- attr(write_matrix, "details")
  formats <- setdiff(names(read_matrix), "object_type")
  baseline <- read_details[
    read_details$format == "base_uncompressed" & read_details$status == "ok",
    c("object_type", "file_size_bytes")
  ]
  baseline_index <- match(read_details$object_type, baseline$object_type)
  read_details$file_ratio_to_base_uncompressed <-
    read_details$file_size_bytes / baseline$file_size_bytes[baseline_index]
  baseline_index <- match(write_details$object_type, baseline$object_type)
  write_details$file_ratio_to_base_uncompressed <-
    write_details$file_size_bytes / baseline$file_size_bytes[baseline_index]

  result <- list(
    read_ms = read_matrix,
    write_ms = write_matrix,
    file_mib = benchmark_metric_matrix(read_details, "file_size_bytes", formats),
    file_to_object_ratio = benchmark_metric_matrix(
      read_details,
      "file_to_object_ratio",
      formats
    ),
    file_ratio_to_base_uncompressed = benchmark_metric_matrix(
      read_details,
      "file_ratio_to_base_uncompressed",
      formats
    ),
    read_file_mib_per_sec = benchmark_metric_matrix(
      read_details,
      "file_mib_per_sec",
      formats
    ),
    read_object_mib_per_sec = benchmark_metric_matrix(
      read_details,
      "object_mib_per_sec",
      formats
    ),
    write_file_mib_per_sec = benchmark_metric_matrix(
      write_details,
      "file_mib_per_sec",
      formats
    ),
    write_object_mib_per_sec = benchmark_metric_matrix(
      write_details,
      "object_mib_per_sec",
      formats
    ),
    read_alloc_mib = benchmark_metric_matrix(read_details, "mem_alloc_bytes", formats),
    write_alloc_mib = benchmark_metric_matrix(write_details, "mem_alloc_bytes", formats),
    details = rbind(read_details, write_details),
    case_metadata = attr(read_matrix, "case_metadata"),
    package_versions = attr(read_matrix, "package_versions"),
    backend_settings = attr(read_matrix, "backend_settings"),
    environment = attr(read_matrix, "environment"),
    parameters = list(
      read = attr(read_matrix, "parameters"),
      write = attr(write_matrix, "parameters")
    )
  )
  result$file_mib[-1L] <- lapply(result$file_mib[-1L], function(x) x / 1024^2)
  result$read_alloc_mib[-1L] <- lapply(
    result$read_alloc_mib[-1L],
    function(x) x / 1024^2
  )
  result$write_alloc_mib[-1L] <- lapply(
    result$write_alloc_mib[-1L],
    function(x) x / 1024^2
  )
  setting_index <- match(result$details$format, result$backend_settings$format)
  for (setting in setdiff(names(result$backend_settings), "format")) {
    result$details[[paste0("backend_", setting)]] <-
      result$backend_settings[[setting]][setting_index]
  }
  result$details$requested_threads <- result$parameters$read$threads
  result$details$thread_comparison <- result$parameters$read$thread_comparison
  result$details$warmups <- result$parameters$read$warmups
  result$details$memory_enabled <- result$parameters$read$memory
  result$details$memory_measure <- result$parameters$read$memory_measure
  result$details$requested_iterations <- result$parameters$read$iterations
  result$details$cache_mode <- ifelse(
    result$details$operation == "read",
    result$parameters$read$cache_mode,
    result$parameters$write$cache_mode
  )
  result$details$cold_cache_label <- ifelse(
    result$details$operation == "read",
    result$parameters$read$cold_cache_label,
    NA_character_
  )
  result$details$object_suite <- result$parameters$read$object_suite
  result$details$seed <- result$parameters$read$seed
  result$details$target_size <- result$parameters$read$size
  result$details$build_mode <- result$parameters$read$build_mode
  class(result) <- c("rdz_benchmark_result", "list")
  result
}

print.rdz_benchmark_result <- function(x, ..., digits = 5L) {
  cat("Median read time (ms)\n")
  print(x$read_ms, row.names = FALSE, digits = digits)
  cat("\nMedian write time (ms)\n")
  print(x$write_ms, row.names = FALSE, digits = digits)
  cat("\nFile size (MiB)\n")
  print(x$file_mib, row.names = FALSE, digits = digits)
  invisible(x)
}

#' Compatibility read-only view
benchmark_read_matrix <- function(...) {
  benchmark_operation_matrix(operation = "read", ...)
}

#' Write-only benchmark view
benchmark_write_matrix <- function(...) {
  benchmark_operation_matrix(operation = "write", ...)
}

#' Run paired benchmarks at several workload sizes
benchmark_serialization_suite <- function(
  sizes = c(small = 1000L, medium = 100000L, throughput = 1000000L),
  object_suite = c("representative", "expanded"),
  ...
) {
  object_suite <- match.arg(object_suite)
  if (!is.numeric(sizes) || !length(sizes) || anyNA(sizes) ||
      any(!is.finite(sizes)) || any(sizes < 1) ||
      any(sizes != floor(sizes)) || any(sizes > .Machine$integer.max)) {
    stop("`sizes` must contain positive whole numbers.", call. = FALSE)
  }
  if (is.null(names(sizes)) || any(!nzchar(names(sizes))) || anyDuplicated(names(sizes))) {
    names(sizes) <- paste0("size_", as.integer(sizes))
  }

  arguments <- list(...)
  arguments$size <- NULL
  arguments$objects <- NULL
  runs <- vector("list", length(sizes))
  names(runs) <- names(sizes)

  for (index in seq_along(sizes)) {
    size <- as.integer(sizes[[index]])
    run_arguments <- arguments
    run_arguments$size <- size
    run_arguments$object_suite <- object_suite
    runs[[index]] <- do.call(benchmark_serialization_matrix, run_arguments)
    runs[[index]]$details$size_label <- names(sizes)[[index]]
    runs[[index]]$details$target_size <- size
  }

  result <- list(
    runs = runs,
    details = do.call(rbind, lapply(runs, `[[`, "details")),
    sizes = stats::setNames(as.integer(sizes), names(sizes)),
    size_labels = names(sizes),
    object_suite = object_suite,
    environment = runs[[1L]]$environment,
    package_versions = runs[[1L]]$package_versions,
    backend_settings = runs[[1L]]$backend_settings
  )
  rownames(result$details) <- NULL
  class(result) <- c("rdz_benchmark_suite", "list")
  result
}

print.rdz_benchmark_suite <- function(x, ..., digits = 5L) {
  for (label in names(x$runs)) {
    cat("\n=== ", label, " (target size ", x$sizes[[label]], ") ===\n", sep = "")
    print(x$runs[[label]], digits = digits)
  }
  invisible(x)
}
