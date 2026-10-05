# The generic codec, streamed (plan-c Stage C): R_Serialize() into blocks and
# R_Unserialize() out of them, in every build.

roundtrip_generic <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "r")
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  read_rdz(path)
}

leftover_temporaries <- function(dir) {
  list.files(dir, pattern = "-rdz-.*\\.(tmp|backup)$", all.files = TRUE)
}

test_that("every kind of R object round-trips through the generic codec", {
  f <- function(x, y = 2) x + y
  objects <- list(
    null = NULL,
    logical = c(TRUE, NA, FALSE),
    integer = c(1L, NA, .Machine$integer.max),
    compact = 1:10,
    double = c(0, -0, NaN, NA, Inf, -Inf, 2^-1074),
    complex = complex(real = 1:2, imaginary = c(NA, 3)),
    character = c("a", NA, "", "é"),
    raw = as.raw(0:255),
    list = list(1, list("a", NULL), list()),
    factor = factor(c("b", "a", NA)),
    data_frame = data.frame(x = 1:3, y = c("a", "b", NA)),
    matrix = matrix(1:6, 2, dimnames = list(NULL, c("a", "b", "c"))),
    attributes = structure(1:3, note = "kept", class = "custom"),
    formula = stats::as.formula("y ~ x", env = globalenv()),
    call = quote(f(x, 1)),
    symbol = as.name("alpha"),
    pairlist = pairlist(a = 1, b = "x"),
    expression = expression(1 + 2),
    date = as.Date("2026-10-05"),
    posixct = as.POSIXct("2026-10-05 12:00:00", tz = "UTC")
  )
  for (name in names(objects)) {
    expect_identical(roundtrip_generic(objects[[name]]), objects[[name]], label = name)
  }
  g <- roundtrip_generic(f)
  expect_identical(formals(g), formals(f))
  expect_identical(body(g), body(f))
  e <- new.env()
  assign("v", 1:3, envir = e)
  expect_identical(get("v", envir = roundtrip_generic(e)), 1:3)
})

test_that("reference objects keep base R serialization's semantics", {
  methods::setClass("RdzTestPoint", representation(x = "numeric"))
  on.exit(methods::removeClass("RdzTestPoint"), add = TRUE)
  point <- methods::new("RdzTestPoint", x = c(1, 2))
  expect_identical(roundtrip_generic(point), point)

  # A list mixing vectors, a closure and the environment it captures: the
  # restored closure's environment is the restored list's environment.
  shared <- new.env()
  assign("k", 10L, envir = shared)
  counter <- local(function() k + 1L, shared)
  restored <- roundtrip_generic(list(v = 1:3, f = counter, e = shared))
  expect_identical(restored$v, 1:3)
  expect_identical(restored$f(), 11L)
  expect_true(identical(environment(restored$f), restored$e))

  # Base R writes an external pointer as a null pointer; rdz promises exactly
  # that, no more. (Base R has no public constructor for a weak reference.)
  pointer <- roundtrip_generic(methods::new("externalptr"))
  expect_identical(typeof(pointer), "externalptr")
})

test_that("payloads straddling the block size round-trip", {
  mib <- 1048576L
  # The serialization around a raw vector's bytes: 31 bytes in a UTF-8
  # locale, more where the native encoding has a longer name.
  overhead <- length(serialize(raw(), NULL, xdr = TRUE, version = 3L))
  for (n in c(mib - overhead - 1L, mib - overhead, mib - overhead + 1L, 3L * mib + 7L)) {
    x <- rep_len(as.raw(0:250), n)
    path <- tempfile(fileext = ".rdz")
    write_rdz(x, path, mode = "r")
    expect_identical(read_rdz(path), x, label = n)
    expect_identical(rdz_info(path)$payload_bytes, as.numeric(n + overhead), label = n)
    expect_identical(
      rdz_info(path)$block_count,
      as.integer(ceiling((n + overhead) / mib)),
      label = n
    )
    unlink(path)
  }
})

test_that("the streamed writer reproduces the Rust writer's files", {
  # The serialization header records the R version, so the bytes match the
  # reference corpus only under the R that wrote it.
  manifest <- utils::read.delim(
    file.path(rust_fixture_dir(), "manifest.tsv"),
    colClasses = "character", quote = ""
  )
  path <- file.path(rust_fixture_dir(), "gen_rare.rdz")
  payload <- rdz:::rdz_check(.Call(rdz:::rdz_test_read_generic, path))
  writer_version <- as.integer(payload[7:10])
  skip_if_not(
    identical(writer_version, as.integer(serialize(NULL, NULL, xdr = TRUE)[7:10])),
    "the corpus was written by another R version"
  )
  copy <- tempfile(fileext = ".rdz")
  on.exit(unlink(copy), add = TRUE)
  # The Rust writer did not compress.
  old <- options(rdz.preset = "speed")
  on.exit(options(old), add = TRUE)
  for (spec in rust_fixture_specs()) {
    if (manifest$codec[manifest$name == spec$name] != "r_serial_v3") next
    fixture <- file.path(rust_fixture_dir(), paste0(spec$name, ".rdz"))
    write_rdz(spec$value(), copy, mode = spec$mode)
    expect_identical(
      readBin(copy, "raw", file.size(copy)),
      readBin(fixture, "raw", file.size(fixture)),
      label = spec$name
    )
  }
})

test_that("an error during serialization removes the temporary file at once", {
  dir <- tempfile("rdz-unwind-")
  dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE), add = TRUE)
  path <- file.path(dir, "x.rdz")
  write_rdz("old", path)
  x <- rep_len(as.raw(1:200), 3e6)
  expect_error(
    rdz:::rdz_check(.Call(rdz:::rdz_test_write_generic_unwind, x, path, 2L, rdz:::rdz_settings())),
    "failing after 2 blocks"
  )
  # No gc(): the unwind cleanup, not the finalizer, removed it.
  expect_length(leftover_temporaries(dir), 0L)
  expect_identical(read_rdz(path), "old")
})

test_that("an interrupt during a write leaves no file behind", {
  skip_on_os("windows")
  skip_on_cran()
  dir <- tempfile("rdz-interrupt-")
  dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE), add = TRUE)
  path <- file.path(dir, "x.rdz")
  write_rdz("old", path)
  x <- rep_len(as.raw(1:200), 8e6)
  interrupted <- tryCatch(
    {
      tools::pskill(Sys.getpid(), tools::SIGINT)
      write_rdz(x, path, mode = "r")
      FALSE
    },
    interrupt = function(condition) TRUE
  )
  expect_true(interrupted)
  expect_length(leftover_temporaries(dir), 0L)
  expect_identical(read_rdz(path), "old")
})

test_that("a block that fails its checksum stops the read with a classed error", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- rep_len(as.raw(0:250), 3e6)
  old <- options(rdz.preset = "speed") # raw blocks, so offsets are known
  on.exit(options(old), add = TRUE)
  write_rdz(x, path, mode = "r")
  bytes <- readBin(path, "raw", file.size(path))
  # A byte of the second block's payload: past the file header, the first
  # block and its header, and the second block's header.
  at <- 32L + 48L + 1048576L + 48L + 100L
  bytes[[at]] <- as.raw(bitwXor(as.integer(bytes[[at]]), 0xffL))
  writeBin(bytes, path)
  expect_error(read_rdz(path), "checksum mismatch in block 1", class = "rdz_format_error")
})

test_that("a write's R allocation does not grow with the object", {
  skip_if_not_installed("bench")
  skip_on_cran()
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- runif(4e6) # 32 MB
  allocated <- bench::mark(write_rdz(x, path, mode = "r"), iterations = 1L)$mem_alloc
  expect_lt(as.numeric(allocated), 1024^2)
})
