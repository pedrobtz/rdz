# The Rust reference corpus (plan-c.md section 2, Stage A).
#
# Each spec names a file in tests/testthat/fixtures/rust/, the write_rdz() mode
# and RDZ_STRING_DICT policy it was written with, and a constructor for the
# value it holds. tools/make-rust-fixtures.R writes the files and the manifest
# from these specs with the Rust implementation; test-rust-fixtures.R checks
# that the installed implementation reads every one back identically. The
# constructors are deterministic and use no random number generator.

rust_fixture_dir <- function() {
  testthat::test_path("fixtures", "rust")
}

fixture_hash3 <- function(n, salt = 0) {
  if (n == 0L) {
    return(integer())
  }
  as.integer(((seq_len(n) + salt) * 2654435761) %% 4294967296 %% 3)
}

fixture_mixed_logical <- function(n, salt = 0) {
  c(FALSE, TRUE, NA)[fixture_hash3(n, salt) + 1L]
}

fixture_sparse_logical <- function(n) {
  x <- logical(n)
  x[seq(1000L, n, by = 1000L)] <- TRUE
  x[seq(7919L, n, by = 7919L)] <- NA
  x
}

fixture_runs_logical <- function(n) {
  rep_len(rep(c(TRUE, NA, FALSE), each = 4096L), n)
}

fixture_periodic_logical <- function(n) {
  rep_len(c(TRUE, FALSE, NA, FALSE, TRUE), n)
}

# Marks are set explicitly so that the values do not depend on how the file
# is sourced.
fixture_utf8 <- function(x) {
  Encoding(x) <- "UTF-8"
  x
}

fixture_encoded_strings <- function() {
  latin1 <- "caf\xe9"
  Encoding(latin1) <- "latin1"
  bytes <- "\xff\xfe"
  Encoding(bytes) <- "bytes"
  c("plain", NA, "", fixture_utf8("\u00e9t\u00e9 \u2603"), latin1, bytes)
}

fixture_with_names <- function(x, names) {
  names(x) <- names
  x
}

fixture_rare_object <- function() {
  f <- y ~ x + log(z)
  environment(f) <- globalenv()
  list(
    complex = complex(real = c(1, -2.5, NA), imaginary = c(0, 3, 1)),
    symbol = quote(alpha),
    call = quote(f(x, 1L, "a")),
    expression = expression(1 + 2, g(y)),
    formula = f,
    raw = as.raw(c(0, 127, 255)),
    compact = 1:10,
    date = as.Date("2026-10-05"),
    time = as.POSIXct("2026-10-05 12:34:56", tz = "UTC"),
    matrix = matrix(1:6, 2L, dimnames = list(c("a", "b"), NULL)),
    nested = list(list(list(NULL, NA_character_)))
  )
}

fixture_doubles <- function() {
  c(0, -0, 1.5, -2^-1074, .Machine$double.xmax, Inf, -Inf, NA, NaN, pi)
}

# The R serialization of a raw vector of length n is 31 + n bytes (version 3,
# XDR, native encoding "UTF-8"), so these straddle the 1 MiB generic block.
fixture_raw <- function(n) {
  rep_len(as.raw(0:250), n)
}

rust_fixture_specs <- function() {
  block <- 65536L
  mib <- 1048576L
  list(
    # Native logical lengths around the 65,536-value block.
    list(name = "lgl_len0", mode = "native", policy = "plain",
         spec = "logical(0)", value = function() logical()),
    list(name = "lgl_len1", mode = "native", policy = "plain",
         spec = "TRUE", value = function() TRUE),
    list(name = "lgl_block_minus1", mode = "native", policy = "plain",
         spec = "mixed TRUE/FALSE/NA, 65535 values",
         value = function() fixture_mixed_logical(block - 1L)),
    list(name = "lgl_block", mode = "native", policy = "plain",
         spec = "mixed TRUE/FALSE/NA, 65536 values",
         value = function() fixture_mixed_logical(block)),
    list(name = "lgl_block_plus1", mode = "native", policy = "plain",
         spec = "mixed TRUE/FALSE/NA, 65537 values",
         value = function() fixture_mixed_logical(block + 1L)),
    list(name = "lgl_multiblock", mode = "native", policy = "plain",
         spec = "mixed TRUE/FALSE/NA, 200000 values",
         value = function() fixture_mixed_logical(200000L, salt = 7)),
    # Native logical record kinds.
    list(name = "lgl_constant_true", mode = "native", policy = "plain",
         spec = "rep(TRUE, 65536)", value = function() rep(TRUE, block)),
    list(name = "lgl_constant_na", mode = "native", policy = "plain",
         spec = "rep(NA, 65536)", value = function() rep(NA, block)),
    list(name = "lgl_sparse", mode = "native", policy = "plain",
         spec = "FALSE with TRUE every 1000th and NA every 7919th",
         value = function() fixture_sparse_logical(block)),
    list(name = "lgl_runs", mode = "native", policy = "plain",
         spec = "runs of 4096 TRUE, NA, FALSE",
         value = function() fixture_runs_logical(block)),
    list(name = "lgl_periodic", mode = "native", policy = "plain",
         spec = "period 5: TRUE FALSE NA FALSE TRUE",
         value = function() fixture_periodic_logical(block)),
    list(name = "lgl_kinds_multiblock", mode = "native", policy = "plain",
         spec = "one block each: constant, sparse, runs, periodic, mixed",
         value = function() c(
           rep(FALSE, block), fixture_sparse_logical(block),
           fixture_runs_logical(block), fixture_periodic_logical(block),
           fixture_mixed_logical(block)
         )),
    # Native logical with names, under every dictionary policy.
    list(name = "lgl_names_plain", mode = "native", policy = "plain",
         spec = "70000 values, names k0..k36 repeating",
         value = function() fixture_with_names(
           fixture_mixed_logical(70000L), paste0("k", seq_len(70000L) %% 37L))),
    list(name = "lgl_names_block", mode = "native", policy = "block",
         spec = "70000 values, names k0..k36 repeating",
         value = function() fixture_with_names(
           fixture_mixed_logical(70000L), paste0("k", seq_len(70000L) %% 37L))),
    list(name = "lgl_names_global", mode = "native", policy = "global",
         spec = "70000 values, names k0..k36 repeating",
         value = function() fixture_with_names(
           fixture_mixed_logical(70000L), paste0("k", seq_len(70000L) %% 37L))),
    list(name = "lgl_names_auto_repeated", mode = "native", policy = "auto",
         spec = "70000 values, names k0..k36 repeating",
         value = function() fixture_with_names(
           fixture_mixed_logical(70000L), paste0("k", seq_len(70000L) %% 37L))),
    list(name = "lgl_names_auto_unique", mode = "native", policy = "auto",
         spec = "5000 values, unique names",
         value = function() fixture_with_names(
           fixture_mixed_logical(5000L), sprintf("n%06d", seq_len(5000L)))),
    list(name = "lgl_names_encodings_plain", mode = "native", policy = "plain",
         spec = "600 values, names in every encoding and NA, repeating",
         value = function() fixture_with_names(
           fixture_mixed_logical(600L), rep_len(fixture_encoded_strings(), 600L))),
    list(name = "lgl_names_encodings_global", mode = "native", policy = "global",
         spec = "600 values, names in every encoding and NA, repeating",
         value = function() fixture_with_names(
           fixture_mixed_logical(600L), rep_len(fixture_encoded_strings(), 600L))),
    list(name = "lgl_names_multiblock", mode = "native", policy = "plain",
         spec = "50000 values, unique 24-byte names over two string blocks",
         value = function() fixture_with_names(
           fixture_mixed_logical(50000L), sprintf("name-%019d", seq_len(50000L)))),
    list(name = "lgl_names_empty", mode = "native", policy = "plain",
         spec = "zero-length logical with zero-length names",
         value = function() fixture_with_names(logical(), character())),
    # The generic codec: block boundaries, a rare object, automatic fallback.
    list(name = "gen_null", mode = "r", policy = "plain",
         spec = "NULL", value = function() NULL),
    list(name = "gen_raw_block_minus1", mode = "r", policy = "plain",
         spec = "raw vector whose serialization is 1 MiB - 1 bytes",
         value = function() fixture_raw(mib - 32L)),
    list(name = "gen_raw_block", mode = "r", policy = "plain",
         spec = "raw vector whose serialization is 1 MiB",
         value = function() fixture_raw(mib - 31L)),
    list(name = "gen_raw_block_plus1", mode = "r", policy = "plain",
         spec = "raw vector whose serialization is 1 MiB + 1 bytes",
         value = function() fixture_raw(mib - 30L)),
    list(name = "gen_raw_multiblock", mode = "r", policy = "plain",
         spec = "raw vector of 3 MiB + 12345 bytes",
         value = function() fixture_raw(3L * mib + 12345L)),
    list(name = "gen_rare", mode = "r", policy = "plain",
         spec = "list of complex, symbol, call, expression, formula, raw, ALTREP compact sequence, Date, POSIXct, matrix, nested list",
         value = fixture_rare_object),
    list(name = "gen_logical_forced", mode = "r", policy = "plain",
         spec = "mixed logical with names, mode r",
         value = function() fixture_with_names(
           fixture_mixed_logical(100L), paste0("k", seq_len(100L)))),
    list(name = "gen_integer_auto", mode = "auto", policy = "plain",
         spec = "integer with NA and extremes, mode auto",
         value = function() c(0L, 1L, -1L, NA, .Machine$integer.max, -.Machine$integer.max)),
    list(name = "gen_double_auto", mode = "auto", policy = "plain",
         spec = "doubles: signed zeros, subnormal, extremes, infinities, NA, NaN",
         value = fixture_doubles),
    list(name = "gen_character_auto", mode = "auto", policy = "plain",
         spec = "character in every encoding and NA, mode auto",
         value = fixture_encoded_strings),
    list(name = "gen_factor_auto", mode = "auto", policy = "plain",
         spec = "factor with NA and an unused level, mode auto",
         value = function() factor(c("b", "a", NA, "b"), levels = c("a", "b", "c"))),
    list(name = "gen_data_frame_auto", mode = "auto", policy = "plain",
         spec = "data frame of logical, integer, double, character, factor, mode auto",
         value = function() data.frame(
           l = c(TRUE, NA, FALSE), i = c(1L, NA, 3L), d = c(0.5, NaN, -Inf),
           s = c("x", NA, fixture_utf8("\u00e9")), f = factor(c("u", "v", "u")),
           stringsAsFactors = FALSE
         ))
  )
}

# The encoding and compression IDs of every block, read from the directory
# (container version 3: 40-byte trailer, 40-byte directory header, 48-byte
# object entries, 32-byte attribute entries, 64-byte block entries).
rdz_block_encodings <- function(path) {
  bytes <- readBin(path, what = "raw", n = file.info(path)$size)
  le <- function(at, width) {
    sum(as.numeric(bytes[at + seq_len(width)]) * 256^(seq_len(width) - 1L))
  }
  trailer <- length(bytes) - 40L
  directory <- le(trailer + 8L, 8L)
  objects <- le(directory + 16L, 4L)
  attributes <- le(directory + 20L, 4L)
  blocks <- le(directory + 24L, 4L)
  first <- directory + 40L + 48L * objects + 32L * attributes
  entry <- first + 64L * (seq_len(blocks) - 1L)
  list(
    encoding = vapply(entry, function(at) le(at + 48L, 2L), numeric(1L)),
    compression = vapply(entry, function(at) le(at + 50L, 2L), numeric(1L))
  )
}

# A file's bytes without the header's writer field and checksum (bytes 21 to
# 32, 1-based): the Rust reference recorded no writer.
bytes_but_writer <- function(path) {
  bytes <- readBin(path, "raw", file.size(path))
  bytes[21:32] <- as.raw(0L)
  bytes
}
