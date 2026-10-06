# The frozen 0.1.0 corpus (plan-c Stage I): files the C writer wrote when
# the format froze, in tests/testthat/fixtures/v0.1.0/. Every later rdz must
# read each to the value its spec constructs (container-format.md,
# "Compatibility and extension"); a native `speed` file, whose blocks are
# stored raw, must also be what this rdz writes, byte for byte but the writer
# field (a generic file holds the writing R's version). tools/make-frozen-fixtures.R wrote the files and the manifest once;
# no test rewrites them. tools/exchange.R uses these specs between operating
# systems. The constructors are deterministic and use no random numbers.

frozen_hash <- function(n, k, salt = 0) {
  if (n == 0L) return(integer())
  as.integer(((seq_len(n) + salt) * 2654435761) %% 4294967296 %% k)
}

frozen_utf8 <- function(x) {
  Encoding(x) <- "UTF-8"
  x
}

frozen_strings <- function() {
  latin1 <- "caf\xe9"
  Encoding(latin1) <- "latin1"
  bytes <- "\xff\xfe"
  Encoding(bytes) <- "bytes"
  c("plain", NA, "", frozen_utf8("été ☃"), latin1, bytes)
}

# -0 from its bits: R's byte compiler folds the literal `-0` to +0, so a
# function's -0 changes once the JIT compiles it
frozen_neg_zero <- function() {
  readBin(as.raw(c(0, 0, 0, 0, 0, 0, 0, 0x80)), "double", endian = "little")
}

frozen_doubles <- function() {
  # NaN payloads and -0 as bits, never as long literals
  na_payload <- readBin(as.raw(c(1, 0, 0, 0, 0, 0, 0xf8, 0x7f)), "double", endian = "little")
  c(0, frozen_neg_zero(), 1.5, -2^-1074, 2^1023, Inf, -Inf, NA, NaN, na_payload, 2^-24)
}

frozen_frame <- function(n) {
  data.frame(
    id = seq_len(n),
    group = factor(c("a", "b", "c")[frozen_hash(n, 3L) + 1L]),
    score = frozen_hash(n, 1000L, 7) / 8,
    flag = c(TRUE, FALSE, NA)[frozen_hash(n, 3L, 1) + 1L],
    label = c("x", "y", NA, "zz")[frozen_hash(n, 4L, 2) + 1L],
    day = as.Date("2026-01-01") + frozen_hash(n, 400L),
    stringsAsFactors = FALSE
  )
}

frozen_fixture_specs <- function() {
  native <- list(
    list(name = "lgl_kinds", value = function() {
      c(rep(TRUE, 70000L), c(FALSE, TRUE, NA)[frozen_hash(70000L, 3L) + 1L],
        rep_len(c(TRUE, FALSE, NA, FALSE, TRUE), 70000L), rep(NA, 5L))
    }),
    list(name = "lgl_runs", value = function() {
      rep_len(rep(c(TRUE, NA, FALSE), each = 4096L), 20000L)
    }),
    list(name = "lgl_periodic", value = function() rep_len(c(TRUE, FALSE, NA, FALSE, TRUE), 10000L)),
    list(name = "lgl_sparse_named", value = function() {
      x <- logical(5000L)
      x[seq(100L, 5000L, by = 100L)] <- TRUE
      x[777L] <- NA
      names(x) <- paste0("n", seq_along(x))
      x
    }),
    list(name = "int_kinds", value = function() {
      c(1:300000, frozen_hash(1000L, 50L) + 1000000L, rep(7L, 5000L), NA, -2147483647L,
        2147483647L, frozen_hash(3000L, 2147483647L))
    }),
    list(name = "int_delta", value = function() seq(-5L, by = 3L, length.out = 50000L)),
    list(name = "int_runs", value = function() rep(c(1L, NA, 3L), each = 10000L)),
    # two blocks: runs of 2.5 after the special values, then raw values
    list(name = "dbl_kinds", value = function() {
      c(frozen_doubles(), rep(2.5, 131072L), frozen_hash(3000L, 1000L) / 7)
    }),
    # decimals (encoding 23, Stage J) at the compressing levels: prices in
    # cents, millisecond times, one-decimal readings with NA and -0
    list(name = "dbl_decimal", value = function() {
      c(round(100 + cumsum(c(0, (frozen_hash(9999L, 41L) - 20L) / 100)), 2),
        1.7e9 + frozen_hash(5000L, 30000000L) / 1000,
        replace(frozen_hash(3000L, 500L) / 10, c(7L, 99L), c(NA, frozen_neg_zero())))
    }),
    list(name = "chr_plain", value = function() rep_len(frozen_strings(), 600L)),
    list(name = "chr_dictionary", value = function() {
      c("a", "bb", "ccc", NA)[frozen_hash(80000L, 4L) + 1L]
    }),
    list(name = "chr_named", value = function() {
      stats::setNames(c("one", "two", NA), c("a", NA, ""))
    }),
    list(name = "factor", value = function() {
      factor(c("lo", "mid", "hi", NA)[frozen_hash(2000L, 4L) + 1L], levels = c("lo", "mid", "hi"))
    }),
    list(name = "ordered_extra", value = function() {
      structure(factor(c("b", "a"), levels = c("a", "b", "c"), ordered = TRUE), comment = "x")
    }),
    list(name = "empty_vectors", value = function() {
      list(logical(), integer(), double(), character(), factor(character()), list(), NULL)
    }),
    list(name = "nested_list", value = function() {
      list(a = 1L, b = list(c = "x", d = list(NULL, list(TRUE))), e = c(f = 2.5))
    }),
    list(name = "frame", value = function() frozen_frame(1000L)),
    list(name = "frame_row_names", value = function() {
      df <- frozen_frame(5L)
      rownames(df) <- c("r1", "r2", "r3", "r4", "r5")
      df
    }),
    list(name = "frame_classed", value = function() {
      structure(list(x = 1:3, y = c("a", "b", "c")), class = c("tbl_df", "tbl", "data.frame"),
                row.names = c(NA, -3L), label = "kept")
    }),
    list(name = "attributes", value = function() {
      list(
        date = as.Date("2026-10-05") + 0:2,
        time = as.POSIXct(c(0, 3600), origin = "1970-01-01", tz = "Europe/Lisbon"),
        span = as.difftime(c(1, 2.5), units = "hours"),
        matrix = matrix(1:6, 2L, dimnames = list(c("a", "b"), NULL)),
        deep = structure(1:2, meta = structure(list(k = 1L), extra = "v"))
      )
    })
  )
  # a shared vector (Stage O): written once, read back shared
  native[[length(native) + 1L]] <- list(name = "shared", value = function() {
    x <- frozen_hash(5000L, 977L) / 4
    list(a = x, b = x, c = list(x), d = data.frame(u = x, v = x))
  })
  # user metadata (Stage N), native and generic
  native[[length(native) + 1L]] <- list(name = "metadata", value = function() 1:3,
                                        # names from strings, not symbols: the
                                        # parser turns a symbol native (C locale)
                                        metadata = stats::setNames(c("frozen", "\u2713"),
                                                                   c("source", "cl\u00e9")))
  generic <- list(
    list(name = "gen_rare", value = function() {
      list(complex = complex(real = 1, imaginary = -2), raw = as.raw(c(0, 255)),
           call = quote(f(x)), s4 = asS4(c(1, 2)))
    }),
    # past one 1 MiB block
    list(name = "gen_large", value = function() {
      list(as.raw(frozen_hash(1100000L, 256L) %/% 64L), quote(g(y)))
    })
  )
  specs <- list()
  for (s in native) {
    for (preset in c("speed", "balanced")) {
      specs[[length(specs) + 1L]] <- list(name = paste0(s$name, "_", preset), preset = preset,
                                          codec = "native_v1", value = s$value,
                                          metadata = s$metadata)
    }
  }
  for (s in generic) {
    for (preset in c("speed", "balanced")) {
      specs[[length(specs) + 1L]] <- list(name = paste0(s$name, "_", preset), preset = preset,
                                          codec = "r_serial_v3", value = s$value)
    }
  }
  specs
}

# Writes a spec's value at its preset's level (the corpus names its files by
# the presets of their time: "speed" is level 0, "balanced" level 1),
# natively or (generic specs) through the generic codec, with the default
# dictionary policy.
frozen_level <- function(preset) c(speed = 0L, balanced = 1L)[[preset]]

write_frozen_fixture <- function(spec, path) {
  old <- options(rdz.compress = frozen_level(spec$preset), rdz.threads = 1L)
  on.exit(options(old), add = TRUE)
  mode <- if (identical(spec$codec, "native_v1")) "native" else "r"
  rdz::write_rdz(spec$value(), path, mode = mode, metadata = spec$metadata)
}

frozen_dir <- function() testthat::test_path("fixtures", "v0.1.0")
frozen_manifest <- function() {
  utils::read.delim(file.path(frozen_dir(), "manifest.tsv"), colClasses = "character",
                    quote = "")
}
