# rdz_save() and rdz_load(): named objects in one file, loaded all or some.

test_that("objects are saved and loaded, all or by name", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  a <- 1:3
  b <- mtcars
  cc <- list(x = "y")
  rdz_save(a, "b", list = "cc", file = path, metadata = c(saved = "test"))
  expect_identical(rdz_info(path)$metadata, c(saved = "test"))
  expect_identical(read_rdz(path), list(a = a, b = b, cc = cc))
  env <- new.env()
  expect_identical(rdz_load(path, envir = env), c("a", "b", "cc"))
  expect_identical(mget(c("a", "b", "cc"), envir = env), list(a = a, b = b, cc = cc))
  env2 <- new.env()
  expect_identical(rdz_load(path, names = "b", envir = env2), "b")
  expect_identical(ls(env2), "b")
  expect_identical(env2$b, b)
  # from a raw vector too
  env3 <- new.env()
  rdz_load(rdz_serialize(list(z = 2)), envir = env3)
  expect_identical(env3$z, 2)
})

test_that("loading some reads only theirs", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  old <- options(rdz.compress = 0)
  on.exit(options(old), add = TRUE)
  small <- 1:3
  big <- seq_len(100000L) * 2L
  rdz_save(small, big, file = path)
  # corrupt big's first payload byte
  bytes <- readBin(path, "raw", file.size(path))
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  nobj <- sum(as.numeric(bytes[dir + 17:20]) * 256^(0:3))
  natt <- sum(as.numeric(bytes[dir + 21:24]) * 256^(0:3))
  hl <- dir_header_len(bytes, dir)
  first_block <- sum(as.numeric(bytes[dir + hl + 48 * 4 + 41:44]) * 256^(0:3)) # root, names x2, small, big
  entry <- dir + hl + 48 * nobj + 32 * natt + 64 * first_block
  payload <- le_u64(bytes, entry + 16)
  bytes[payload + 1L] <- xor(bytes[payload + 1L], as.raw(0xff))
  writeBin(bytes, path)
  env <- new.env()
  expect_error(rdz_load(path, envir = env), class = "rdz_format_error")
  rdz_load(path, names = "small", envir = env)
  expect_identical(env$small, small)
})

test_that("generic objects are saved and loaded too", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  e <- new.env()
  assign("v", 42, envir = e)
  f <- function(x) x + 1
  rdz_save(e, f, file = path)
  expect_identical(rdz_info(path)$codec, "r_serial_v3")
  env <- new.env()
  rdz_load(path, names = "e", envir = env)
  expect_identical(ls(env), "e")
  expect_identical(get("v", envir = env$e), 42)
})

test_that("saving and loading refuse what they cannot do", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  a <- 1
  expect_error(rdz_save(file = path), "Nothing to save")
  expect_error(rdz_save(a, a, file = path), "once")
  expect_error(rdz_save(no_such_object, file = path), "not found: no_such_object")
  expect_error(rdz_save(a + 1, file = path), "must name objects")
  write_rdz(1:3, path)
  expect_error(rdz_load(path), "of type integer")
  write_rdz(list(1, 2), path)
  expect_error(rdz_load(path), "naming each")
  rdz_save(a, file = path)
  expect_error(rdz_load(path, names = "zz"), "Unknown in `select`: zz")
  expect_error(rdz_load(path, envir = list()), "environment")
})
