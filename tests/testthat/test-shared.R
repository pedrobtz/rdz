# Shared objects (container-format.md, "Native object graphs"): a vector of
# at least 4 KiB met twice in one value is written once and read back as one
# shared R object.

test_that("a large vector met twice is written once and read back shared", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- seq(0.5, 50000, by = 0.5) * 1.1 # 100,000 doubles
  l <- list(a = x, b = x, c = list(d = x), e = 1:3)
  write_rdz(l, path)
  shared_size <- file.size(path)
  expect_identical(read_rdz(path), l)
  write_rdz(list(a = x, b = x + 0, c = list(d = x * 1), e = 1:3), path)
  expect_gt(file.size(path), 2.5 * shared_size) # three copies
  write_rdz(l, path)
  o <- rdz_schema(path)$objects
  expect_identical(o$shared_with[o$path == "$b"], "$a")
  expect_identical(o$shared_with[o$path == "$c$d"], "$a")
  expect_true(is.na(o$shared_with[o$path == "$a"]))
  expect_identical(o$stored_bytes[o$path == "$b"], 0)
  # selection and attribute reads follow a shared part to its target
  expect_identical(read_rdz(path, select = "c"), l["c"])
  expect_identical(read_rdz(path, select = c("b", "e")), l[c("b", "e")])
  expect_identical(rdz_attributes(path, object = c("c", "d")), list())
})

test_that("shared columns, names and factors read back identical", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  df <- data.frame(a = seq_len(5000L) * 3L)
  df$b <- df$a
  write_rdz(df, path)
  expect_identical(read_rdz(path), df)
  expect_identical(rdz_schema(path)$objects$shared_with[3], "$a")
  nm <- sprintf("n%04d", 1:1000)
  v <- list(p = stats::setNames(seq_len(1000L), nm), q = stats::setNames(rev(seq_len(1000L)), nm))
  write_rdz(v, path)
  expect_identical(read_rdz(path), v)
  f <- factor(sample(c("u", "v"), 3000L, replace = TRUE))
  write_rdz(list(f, f), path)
  expect_identical(read_rdz(path), list(f, f))
})

test_that("small vectors are not shared", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- 1:100
  write_rdz(list(x, x), path)
  expect_true(all(is.na(rdz_schema(path)$objects$shared_with)))
})

test_that("sharing is part of the value rdz stores and hashes", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- as.numeric(seq_len(10000L))
  shared <- list(x, x)
  copied <- list(x, x + 0)
  expect_identical(shared, copied)
  write_rdz(shared, path)
  expect_identical(rdz_info(path)$content_hash, rdz_hash(shared))
  # read back, the value is still shared: its hash is the file's
  expect_identical(rdz_hash(read_rdz(path)), rdz_info(path)$content_hash)
  expect_false(identical(rdz_hash(shared), rdz_hash(copied)))
})

test_that("a root attribute shared with another reads alone", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  v <- seq_len(5000L) + 0.5
  df <- data.frame(x = 1:3)
  attr(df, "a") <- v
  attr(df, "b") <- v # stored as a reference to "a"
  write_rdz(df, path)
  expect_identical(rdz_attributes(path, names = "b"), list(b = v))
})
