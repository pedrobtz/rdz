# Inspection below the root (metadata-access.md): rdz_attributes(object =)
# and rdz_schema()$objects read the directory and the blocks of the
# attributes and names asked for, never the data of the parts that hold them.

inspect_fixture <- function() {
  list(
    sales = data.frame(
      day = as.Date("2026-01-01") + 0:199,
      amount = round(seq(1, 100, length.out = 200), 2),
      store = factor(rep(c("north", "south"), 100)),
      at = as.POSIXct("2026-01-01", tz = "Europe/Lisbon") + 1:200
    ),
    models = list(list(coef = c(a = 1.5, b = -2)), structure(matrix(1:6, 2), note = "fitted")),
    label = "Q1"
  )
}

test_that("attributes below the root are those of the part", {
  x <- inspect_fixture()
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(x, path)
  expect_identical(rdz_attributes(path, object = c("sales", "at")), attributes(x$sales$at))
  expect_identical(rdz_attributes(path, object = c("sales", "store")), attributes(x$sales$store))
  expect_identical(rdz_attributes(path, object = list("models", 2)), attributes(x$models[[2]]))
  expect_identical(rdz_attributes(path, object = c(2, 1, 1)), attributes(x$models[[1]]$coef))
  expect_identical(rdz_attributes(path, object = "sales", names = c("names", "class")),
                   attributes(x$sales)[c("names", "class")])
  expect_identical(rdz_attributes(path, object = "sales")$row.names, seq_len(200L))
  expect_identical(rdz_attributes(path, object = "label"), list())
  expect_identical(rdz_attributes(path), attributes(x))
  expect_identical(rdz_attributes(path, object = 0), attributes(x))
})

test_that("an object path that leads nowhere is an error", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(inspect_fixture(), path)
  expect_error(rdz_attributes(path, object = "nope"), "No `nope` in the root")
  expect_error(rdz_attributes(path, object = c("sales", "nope")), "No `nope` in \\$sales")
  expect_error(rdz_attributes(path, object = c("label", 1)), "not a list or a data frame")
  expect_error(rdz_attributes(path, object = list("models", 3)), "past the 2 parts")
  expect_error(rdz_attributes(path, object = c(-1)), "path of names and positive positions")
  expect_error(rdz_attributes(path, object = c("sales", "at"), names = "levels"),
               "Unknown attribute: levels")
})

test_that("attributes and the schema are read without the parts' data", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.preset = "speed")
  on.exit(options(old), add = TRUE)
  x <- list(big = structure(seq_len(100000L), note = "kept"), small = 1L)
  write_rdz(x, path)
  # corrupt the big vector's first payload byte: object 3 (root, names name,
  # names value, big)
  bytes <- readBin(path, "raw", file.size(path))
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  nobj <- sum(as.numeric(bytes[dir + 17:20]) * 256^(0:3))
  natt <- sum(as.numeric(bytes[dir + 21:24]) * 256^(0:3))
  first_block <- sum(as.numeric(bytes[dir + dir_header_len(bytes, dir) + 48 * 3 + 41:44]) * 256^(0:3))
  entry <- dir + dir_header_len(bytes, dir) + 48 * nobj + 32 * natt + 64 * first_block
  payload <- le_u64(bytes, entry + 16)
  bytes[payload + 1L] <- xor(bytes[payload + 1L], as.raw(0xff))
  writeBin(bytes, path)
  expect_error(read_rdz(path), class = "rdz_format_error")
  expect_identical(rdz_attributes(path, object = "big"), list(note = "kept"))
  schema <- rdz_schema(path)
  expect_identical(schema$objects$path, c("", "$big", "$small"))
  expect_identical(schema$objects$length, c(2, 100000, 1))
})

test_that("the schema describes the tree depth first", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(inspect_fixture(), path)
  o <- rdz_schema(path)$objects
  expect_identical(
    o$path,
    c("", "$sales", "$sales$day", "$sales$amount", "$sales$store", "$sales$at", "$models",
      "$models[[1]]", "$models[[1]]$coef", "$models[[2]]", "$label")
  )
  expect_identical(o$class[o$path == "$sales$at"], "POSIXct/POSIXt")
  expect_identical(o$class[o$path == "$sales$store"], "factor")
  expect_identical(o$class[o$path == "$models[[2]]"], "matrix/array")
  expect_identical(o$shape[o$path == "$models[[2]]"], "2 x 3")
  expect_identical(o$shape[o$path == "$sales"], "200 x 4")
  expect_identical(o$attributes[o$path == "$sales$at"], "class, tzone")
  expect_true(all(o$stored_bytes[1] >= o$stored_bytes))
  shallow <- rdz_schema(path, recursive = FALSE)$objects
  expect_identical(shallow$path, c("", "$sales", "$models", "$label"))
  expect_output(print(rdz_schema(path)), "\\$at: POSIXct/POSIXt 200")
})

test_that("a generic file needs allow_full for attributes below the root", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- list(a = structure(1:2, note = "n"), e = new.env())
  write_rdz(x, path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  expect_error(rdz_attributes(path, object = "a"), "allow_full")
  expect_identical(rdz_attributes(path, object = "a", allow_full = TRUE), list(note = "n"))
  expect_null(rdz_schema(path)$objects)
})
