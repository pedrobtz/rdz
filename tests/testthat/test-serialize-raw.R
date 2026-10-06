# rdz_serialize() and rdz_unserialize(): the file's bytes in a raw vector,
# and every reader taking one where it takes a path.

test_that("a raw vector holds the bytes a file would", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  values <- list(
    frame = data.frame(a = 1:3, b = c("x", NA, "z"), d = as.Date("2026-10-06") + 0:2),
    nested = list(a = 1.5, b = list(c = letters)),
    generic = list(f = quote(g(x)), z = 1i),
    empty = list()
  )
  for (name in names(values)) {
    for (threads in c(1L, 4L)) {
      old <- options(rdz.threads = threads)
      bytes <- rdz_serialize(values[[name]])
      write_rdz(values[[name]], path)
      options(old)
      expect_type(bytes, "raw")
      expect_identical(bytes_but_writer_raw(bytes), bytes_but_writer(path), label = name)
      expect_identical(rdz_unserialize(bytes), values[[name]], label = name)
    }
  }
})

test_that("the readers take a raw vector where they take a path", {
  bytes <- rdz_serialize(mtcars, metadata = c(source = "datasets"))
  expect_identical(read_rdz(bytes), mtcars)
  expect_identical(rdz_unserialize(bytes, select = c("wt", "mpg")), mtcars[c("wt", "mpg")])
  info <- rdz_info(bytes)
  expect_identical(info$metadata, c(source = "datasets"))
  expect_identical(info$content_hash, rdz_hash(mtcars))
  expect_identical(rdz_schema(bytes)$objects$path[2], "$mpg")
  expect_identical(rdz_attributes(bytes, names = "names"), list(names = names(mtcars)))
  expect_identical(rdz_verify(bytes, content = TRUE), bytes)
  expect_identical(rdz_unserialize(rdz_serialize(1:5, mode = "r")), 1:5)
  expect_error(rdz_serialize(new.env(), mode = "native"), class = "rdz_unsupported_error")
})

test_that("damaged bytes are format errors", {
  bytes <- rdz_serialize(seq_len(1000L))
  expect_error(rdz_unserialize(bytes[-length(bytes)]), class = "rdz_format_error")
  bad <- bytes
  bad[100L] <- xor(bad[100L], as.raw(0xff))
  expect_error(rdz_unserialize(bad), class = "rdz_format_error")
  expect_error(rdz_unserialize(raw()), class = "rdz_format_error")
  expect_error(rdz_unserialize("not raw"), "raw vector")
})
