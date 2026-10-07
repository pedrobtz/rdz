# rdz_attributes() walks `object` and reads the attributes in one call
# (the follow-up to issue #27, F.1): one open of the file.

attribute_opens <- function(code) {
  before <- .Call(rdz:::rdz_test_opens)
  force(code)
  .Call(rdz:::rdz_test_opens) - before
}

attribute_value <- function() {
  df <- data.frame(id = 1:3, f = factor(c("a", "b", "a")))
  rownames(df) <- c("x", "y", "z")
  attr(df, "meta") <- list(k = 1)
  v <- seq_len(5000) + 0.5
  x <- list(sales = df, models = list(m1 = 1, m2 = structure(1:3, foo = "bar")), u = list(1, 2))
  attr(x, "big") <- v
  attr(x, "big2") <- v # stored as a reference to "big"
  x
}

test_that("rdz_attributes() opens the file once, whatever the path and the file", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(attribute_value(), path, mode = "native")
  expect_identical(attribute_opens(rdz_attributes(path)), 1)
  expect_identical(attribute_opens(rdz_attributes(path, object = c("sales", "f"))), 1)
  expect_identical(attribute_opens(rdz_attributes(path, object = list("models", 2))), 1)
  expect_identical(attribute_opens(rdz_attributes(path, object = "sales",
                                                  names = c("meta", "row.names", "class"))), 1)
  expect_identical(attribute_opens(rdz_attributes(path, names = "big2")), 1)
  expect_identical(attribute_opens(try(rdz_attributes(path, object = "nope"), silent = TRUE)), 1)

  generic <- tempfile(fileext = ".rdz")
  on.exit(unlink(generic), add = TRUE)
  write_rdz(attribute_value(), generic, mode = "r")
  expect_identical(attribute_opens(rdz_attributes(generic, "sales", allow_full = TRUE)), 1)
  expect_identical(attribute_opens(try(rdz_attributes(generic, "sales"), silent = TRUE)), 1)
})

test_that("a repeated name gives its value each time; unknown names are listed once", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- attribute_value()
  write_rdz(x, path, mode = "native")
  got <- rdz_attributes(path, names = c("big", "names", "big"))
  expect_identical(got, list(big = attr(x, "big"), names = names(x), big = attr(x, "big")))
  got <- rdz_attributes(path, object = "sales", names = c("class", "meta", "class"))
  expect_identical(got, list(class = "data.frame", meta = list(k = 1), class = "data.frame"))
  expect_error(rdz_attributes(path, names = c("zz", "names", "zz", "yy")),
               "^Unknown attribute: zz, yy$", class = "rdz_argument_error")
})

test_that("a name step into a list without names finds no such part", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(attribute_value(), path, mode = "native")
  expect_error(rdz_attributes(path, object = list("u", "a")), "^No `a` in \\$u\\.$",
               class = "rdz_argument_error")
  expect_identical(rdz_attributes(path, object = list("u", 1)), list())
})

test_that("a stored data frame class replaces the implied one in place", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(a = 1:2)
  class(df) <- c("tbl_df", "data.frame")
  attr(df, "z") <- 1
  write_rdz(df, path, mode = "native")
  expect_identical(rdz_info(path)$codec, "native_v1")
  expect_identical(rdz_attributes(path),
                   list(names = "a", row.names = 1:2, class = c("tbl_df", "data.frame"), z = 1))
})

test_that("a malformed object is refused for a native file, after the open", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(attribute_value(), path, mode = "native")
  expect_error(rdz_attributes(path, object = list(NA)), "must be NULL, 0, or a path",
               class = "rdz_argument_error")
  # a generic file answers it as it always has: allow_full first
  write_rdz(attribute_value(), path, mode = "r")
  expect_error(rdz_attributes(path, object = list(NA)), "allow_full", class = "rdz_argument_error")
})

test_that("rdz_attributes() lists the attributes rdz_schema() names, for every part", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  ordered_df <- data.frame(o = factor(c("lo", "hi"), levels = c("lo", "hi"), ordered = TRUE))
  class(ordered_df) <- c("tbl_df", "data.frame")
  values <- list(
    attribute_value(),
    list(a = ordered_df, b = structure(list(c = 1:2), class = "rec"), m = matrix(1:4, 2)),
    data.frame(x = 1:3, y = c("a", "b", "c"))
  )
  steps_of <- function(p) {
    parts <- regmatches(p, gregexpr("\\$[A-Za-z0-9_.]+|\\[\\[[0-9]+\\]\\]", p))[[1L]]
    lapply(parts, function(s) {
      if (startsWith(s, "$")) substring(s, 2L) else as.numeric(gsub("[^0-9]", "", s))
    })
  }
  for (x in values) {
    write_rdz(x, path, mode = "native")
    schema <- rdz_schema(path)$objects
    for (i in seq_len(nrow(schema))) {
      object <- if (nzchar(schema$path[i])) steps_of(schema$path[i])
      expect_identical(paste(names(rdz_attributes(path, object = object)), collapse = ", "),
                       schema$attributes[i], label = schema$path[i])
    }
  }
})
