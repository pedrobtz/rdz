# General attributes (plan-c Stage I): any attribute whose value is native
# is written natively, attributes of its own included.

roundtrip_attrs <- function(x) {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path, mode = "native")
  list(value = read_rdz(path), info = rdz_info(path))
}

test_that("common attributed objects are native and identical", {
  cases <- list(
    date = as.Date("2026-10-05") + 0:4,
    posixct = as.POSIXct("2026-10-05 12:00:00", tz = "Europe/Lisbon") + 1:3,
    difftime = as.difftime(c(1, 2.5), units = "hours"),
    matrix = matrix(1:6, 2, dimnames = list(c("a", "b"), NULL)),
    array = array(c(TRUE, NA), c(1L, 2L, 1L)),
    classed_list = structure(list(a = 1, b = "x"), class = "myclass"),
    posixlt = as.POSIXlt("2026-10-05 12:00:00", tz = "UTC"),
    factor_extra = structure(factor(c("a", "b")), comment = "note"),
    nested = structure(1:3, meta = structure(list(k = 1L), extra = "deep")),
    frame = data.frame(d = as.Date("2026-01-01") + 0:2,
                       t = as.POSIXct(0:2, origin = "1970-01-01", tz = "UTC"),
                       x = 1:3),
    frame_extra = structure(data.frame(x = 1:2), label = "a frame"),
    tibble_like = structure(list(x = 1:2), class = c("tbl_df", "tbl", "data.frame"),
                            row.names = c(NA, -2L), names = "x"),
    long_name = structure(1L, `a very long attribute name indeed` = strrep("v", 100))
  )
  for (name in names(cases)) {
    out <- roundtrip_attrs(cases[[name]])
    expect_identical(out$info$codec, "native_v1", label = name)
    expect_identical(out$value, cases[[name]], label = name)
  }
})

test_that("a keyed data.table keeps its key natively", {
  skip_if_not_installed("data.table")
  dt <- data.table::data.table(k = c(2L, 1L, 3L), v = c("b", "a", "c"))
  data.table::setkeyv(dt, "k")
  out <- roundtrip_attrs(dt)
  expect_identical(out$info$codec, "native_v1")
  expect_identical(data.table::key(out$value), "k")
  expect_true(all.equal(out$value, dt))
})

test_that("rdz_info() lists and rdz_attributes() reads general attributes", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- structure(c(a = 1, b = 2), units = "m", meta = list(1L, "two"))
  write_rdz(x, path)
  expect_identical(rdz_info(path)$attribute_names, c("names", "units", "meta"))
  expect_identical(rdz_attributes(path), attributes(x))
  expect_identical(rdz_attributes(path, names = "meta"), list(meta = list(1L, "two")))
  expect_identical(rdz_attributes(path, names = "units"), list(units = "m"))
  expect_error(rdz_attributes(path, names = "nope"), "Unknown attribute")
})

test_that("attributes that are not native send the whole root to the generic codec", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  cases <- list(
    environment = structure(1:2, env = globalenv()),
    s4 = asS4(c(1, 2)),
    selfref = structure(list(1), .internal.selfref = 1L),
    # a name with a byte past ASCII, made from bytes so no locale rewrites it
    non_ascii_name = `attr<-`(1L, rawToChar(as.raw(c(0x63, 0x61, 0x66, 0xe9))), 1L),
    row_names = structure(1:2, row.names = c("a", "b")),
    # a column longer than the rows
    matrix_column = data.frame(x = 1:3, m = I(matrix(1:6, 3)))
  )
  for (name in names(cases)) {
    write_rdz(cases[[name]], path)
    expect_identical(rdz_info(path)$codec, "r_serial_v3", label = name)
    expect_identical(read_rdz(path), cases[[name]], label = name)
  }
})

test_that("an attribute R refuses is a format error, not R's", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.preset = "speed") # every block raw
  on.exit(options(old), add = TRUE)
  # "dix" becomes "dim", whose value c(4L, 4L) does not fit six values
  write_rdz(structure(1:6, dix = c(4L, 4L)), path, mode = "native")
  bytes <- readBin(path, "raw", file.size(path))
  at <- grepRaw(charToRaw("dix"), bytes, fixed = TRUE)
  expect_length(at, 1L)
  bytes[at + 2L] <- charToRaw("m")
  bytes <- reseal_block(bytes, at - 1L - 5L) # the record's 5-byte header
  writeBin(bytes, path)
  expect_error(read_rdz(path), class = "rdz_format_error")
})
