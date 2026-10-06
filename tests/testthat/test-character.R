# Native character vectors and factors (plan-c Stage G).

roundtrip_native <- function(x, policy = NULL, ...) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  if (!is.null(policy)) {
    old_env <- Sys.getenv("RDZ_STRING_DICT", unset = NA)
    Sys.setenv(RDZ_STRING_DICT = policy)
    on.exit(if (is.na(old_env)) Sys.unsetenv("RDZ_STRING_DICT") else
      Sys.setenv(RDZ_STRING_DICT = old_env), add = TRUE)
  }
  old <- options(...)
  on.exit(options(old), add = TRUE)
  write_rdz(x, path, mode = "native")
  expect_identical(rdz_info(path)$codec, "native_v1")
  read_rdz(path)
}

encoded <- function() {
  latin1 <- "caf\xe9"
  Encoding(latin1) <- "latin1"
  bytes <- rawToChar(as.raw(c(0xff, 0x41, 0xfe)))
  Encoding(bytes) <- "bytes"
  utf8 <- "été ☃"
  Encoding(utf8) <- "UTF-8"
  c("plain", NA, "", utf8, latin1, bytes)
}

test_that("character vectors keep bytes, encoding tags and NA under every policy", {
  x <- rep_len(encoded(), 5000L)
  for (policy in c("plain", "block", "global", "auto")) {
    got <- roundtrip_native(x, policy)
    expect_identical(got, x, label = policy)
    expect_identical(Encoding(got), Encoding(x), label = policy)
  }
})

test_that("long and many strings span blocks", {
  long <- strrep("x", 1e6) # nearly a block on its own
  x <- c("a", long, NA, sprintf("id-%07d", 1:300000))
  expect_identical(roundtrip_native(x, rdz.threads = test_threads(4L)), x)
  # A string larger than a block is not a native record: the root goes
  # generic in automatic mode, which streams it.
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  huge <- c("a", strrep("y", 2e6))
  expect_error(write_rdz(huge, path, mode = "native"), "larger than one block",
               class = "rdz_unsupported_error")
  write_rdz(huge, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path), huge)
  expect_identical(roundtrip_native(character()), character())
  y <- stats::setNames(c("u", "v"), c("first", "second"))
  expect_identical(roundtrip_native(y), y)
})

test_that("native-encoded non-ASCII strings are stored natively, as UTF-8", {
  skip_if_not(isTRUE(l10n_info()$`UTF-8`), "not a UTF-8 locale")
  x <- c("a", "caf\u00e9", NA)
  Encoding(x) <- "unknown" # as R leaves typed and imported strings in a UTF-8 locale
  expect_identical(Encoding(x[[2L]]), "unknown")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "native")
  expect_identical(rdz_info(path)$codec, "native_v1")
  got <- read_rdz(path)
  expect_identical(got, x)
  expect_identical(Encoding(got[[2L]]), "UTF-8")
  expect_identical(rdz_info(path)$content_hash, rdz_hash(x))
  # the same string marked UTF-8 is the same value
  expect_identical(rdz_hash(x), rdz_hash(enc2utf8(x)))
  df <- data.frame(city = x, stringsAsFactors = TRUE)
  write_rdz(df, path)
  expect_identical(rdz_info(path)$codec, "native_v1")
  expect_identical(read_rdz(path), df)
})

test_that("a native string the session cannot convert losslessly goes generic", {
  skip_on_os("windows")
  old <- Sys.getlocale("LC_CTYPE")
  skip_if(!nzchar(suppressWarnings(Sys.setlocale("LC_CTYPE", "C"))), "no C locale")
  on.exit(Sys.setlocale("LC_CTYPE", old), add = TRUE)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9))) # unmarked, not ASCII
  write_rdz(x, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path), x)
  expect_error(write_rdz(x, path, mode = "native"), class = "rdz_unsupported_error")
})

test_that("a file's strings read the same in another locale", {
  skip_on_cran()
  skip_on_os("windows")
  # the child R loads the installed package, which test_local() does not make
  skip_if(length(find.package("rdz", lib.loc = .libPaths(), quiet = TRUE)) == 0L,
          "rdz is not installed")
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- rep_len(encoded(), 10L)
  write_rdz(x, path, mode = "native")
  script <- tempfile(fileext = ".R")
  on.exit(unlink(script), add = TRUE)
  writeLines(sprintf(
    'x <- rdz::read_rdz("%s"); cat(Encoding(x), sep = ","); cat("|"); cat(vapply(x, function(s) paste(as.integer(charToRaw(s)), collapse = "."), ""), sep = ",")',
    path), script)
  out <- system2(file.path(R.home("bin"), "Rscript"), c("--vanilla", script),
                 stdout = TRUE,
                 env = c("LC_ALL=C", "LANG=C",
                         paste0("R_LIBS=", paste(.libPaths(), collapse = .Platform$path.sep))))
  expected <- paste0(
    paste(Encoding(x), collapse = ","), "|",
    paste(vapply(x, function(s) paste(as.integer(charToRaw(s)), collapse = "."), ""),
          collapse = ",")
  )
  expect_identical(paste(out, collapse = ""), expected)
})

test_that("factors round-trip with their levels and order", {
  cases <- list(
    plain = factor(c("b", "a", NA, "b")),
    unused = factor(c("a", "a"), levels = c("a", "b", "c")),
    ordered = factor(c("lo", "hi", "mid"), levels = c("lo", "mid", "hi"), ordered = TRUE),
    no_levels = factor(character()),
    empty_levels = factor(c(NA, NA), levels = character()),
    na_level = factor(c("a", NA), exclude = NULL),
    big = factor(sample(sprintf("level-%03d", 1:300), 600000L, replace = TRUE))
  )
  for (name in names(cases)) {
    expect_identical(roundtrip_native(cases[[name]]), cases[[name]], label = name)
  }
})

test_that("factors keep other attributes; bad codes are left generic", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  f <- factor(c("a", "b"))
  attr(f, "note") <- "x"
  write_rdz(f, path)
  expect_identical(rdz_info(path)$codec, "native_v1")
  expect_identical(read_rdz(path), f)
  named <- structure(factor(c("a", "b")), names = c("x", "y"))
  write_rdz(named, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path), named)
  bad <- structure(c(1L, 5L), levels = c("a", "b"), class = "factor")
  write_rdz(bad, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_identical(read_rdz(path), bad)
})

test_that("a factor's levels and class are readable without its codes", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  f <- factor(c("lo", "hi"), levels = c("lo", "hi"), ordered = TRUE)
  write_rdz(f, path)
  info <- rdz_info(path)
  expect_identical(info$root_type, "factor")
  expect_identical(info$attribute_names, c("levels", "class"))
  expect_identical(
    rdz_attributes(path, names = c("levels", "class")),
    list(levels = c("lo", "hi"), class = c("ordered", "factor"))
  )
})
