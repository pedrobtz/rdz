test_that("write_rdz writes a file and returns its path invisibly", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  result <- withVisible(write_rdz(list(value = 1L), path, mode = "r"))

  expect_false(result$visible)
  expect_identical(result$value, path)
  expect_true(file.exists(path))
  expect_identical(read_rdz(path), list(value = 1L))

  bytes <- readBin(path, what = "raw", n = file.info(path)$size)
  expect_identical(bytes[1:4], as.raw(c(0x52, 0x44, 0x5a, 0x1a)))
  expect_identical(
    readBin(bytes[5:6], integer(), n = 1L, size = 2L, endian = "little"),
    3L
  )
  expect_identical(
    readBin(bytes[7:8], integer(), n = 1L, size = 2L, endian = "little"),
    32L
  )
  expect_identical(
    readBin(bytes[13:14], integer(), n = 1L, size = 2L, endian = "little"),
    1L
  )
  expect_identical(
    readBin(bytes[15:16], integer(), n = 1L, size = 2L, endian = "little"),
    3L
  )
  expect_identical(bytes[33:36], charToRaw("RBLK"))
  expect_identical(bytes[(length(bytes) - 39L):(length(bytes) - 36L)], charToRaw("RDZT"))
  expect_identical(tail(bytes, 4L), charToRaw("ZEND"))
})

test_that("write_rdz safely replaces an existing file", {
  path <- tempfile(pattern = "rdz path with spaces ", fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz("first", path)
  write_rdz(list(replacement = TRUE), path)

  expect_identical(read_rdz(path), list(replacement = TRUE))
})

test_that("file access failures are reported", {
  missing <- tempfile(fileext = ".rdz")
  invalid <- file.path(tempfile("missing-directory-"), "object.rdz")

  expect_error(suppressWarnings(read_rdz(missing)))
  expect_error(suppressWarnings(write_rdz(1L, invalid)))
  expect_false(file.exists(invalid))
})

test_that("truncated and invalid files are rejected", {
  valid <- tempfile(fileext = ".rdz")
  truncated <- tempfile(fileext = ".rdz")
  invalid <- tempfile(fileext = ".rdz")
  on.exit(unlink(c(valid, truncated, invalid)), add = TRUE)

  write_rdz(list(value = rep(pi, 100L)), valid, mode = "r")
  bytes <- readBin(valid, what = "raw", n = file.info(valid)$size)
  writeBin(bytes[seq_len(length(bytes) %/% 2L)], truncated)
  writeBin(as.raw(0:15), invalid)

  expect_error(read_rdz(truncated), "truncated|trailer")
  expect_error(read_rdz(invalid))
})

test_that("payload corruption is detected before deserialization", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(list(value = rep(pi, 100L)), path, mode = "r")
  bytes <- readBin(path, what = "raw", n = file.info(path)$size)
  # The first block payload starts after the 32-byte file header and the
  # 48-byte block header.
  bytes[[81L]] <- as.raw(
    bitwXor(as.integer(bytes[[81L]]), 0xffL)
  )
  writeBin(bytes, path)

  expect_error(read_rdz(path), "checksum mismatch")
})

test_that("header and block-header corruption are detected", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(list(value = 1L), path, mode = "r")
  bytes <- readBin(path, what = "raw", n = file.info(path)$size)

  corrupt_header <- bytes
  corrupt_header[[25L]] <- as.raw(bitwXor(as.integer(corrupt_header[[25L]]), 0xffL))
  writeBin(corrupt_header, path)
  expect_error(read_rdz(path), "header checksum mismatch")

  corrupt_block <- bytes
  corrupt_block[[33L]] <- as.raw(bitwXor(as.integer(corrupt_block[[33L]]), 0xffL))
  writeBin(corrupt_block, path)
  expect_error(read_rdz(path), "block 0 header")
})

test_that("unknown format versions and flags are rejected", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(1L, path)
  bytes <- readBin(path, what = "raw", n = file.info(path)$size)

  unknown_version <- bytes
  unknown_version[5:6] <- writeBin(4L, raw(), size = 2L, endian = "little")
  writeBin(unknown_version, path)
  expect_error(read_rdz(path), "unsupported rdz format version 4")

  unknown_flags <- bytes
  unknown_flags[9:12] <- writeBin(1L, raw(), size = 4L, endian = "little")
  writeBin(unknown_flags, path)
  expect_error(read_rdz(path), "unsupported header flags")
})

test_that("ordinary RDS files are not treated as rdz containers", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  saveRDS(list(value = pi), path)

  expect_error(read_rdz(path))
})

test_that("the transitional generic payload uses R's XDR stream", {
  old <- options(rdz.compress = 0) # stored raw, so the stream is visible
  on.exit(options(old), add = TRUE)
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)

  write_rdz(list(value = 1L), path, mode = "r")
  bytes <- readBin(path, what = "raw", n = 82L)

  expect_identical(bytes[81:82], charToRaw("X\n"))
})

test_that("codec modes distinguish fallback from strict native encoding", {
  auto <- tempfile(fileext = ".rdz")
  forced_r <- tempfile(fileext = ".rdz")
  native <- tempfile(fileext = ".rdz")
  on.exit(unlink(c(auto, forced_r, native)), add = TRUE)

  write_rdz(function() 1L, auto, mode = "auto")
  write_rdz(1L, forced_r, mode = "r")

  expect_identical(rdz_info(auto)$codec, "r_serial_v3")
  expect_identical(rdz_info(forced_r)$codec, "r_serial_v3")
  expect_error(
    write_rdz(function() 1L, native, mode = "native"),
    "native serialization is not implemented for closure"
  )
  expect_false(file.exists(native))
  expect_error(write_rdz(1L, native, mode = "invalid"), "arg")
})

test_that("rdz_info reads bounded container metadata and a generic synopsis", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  x <- structure(
    list(value = raw(2L * 1024L * 1024L)),
    class = c("custom_list", "list"),
    note = "metadata value is not copied into the synopsis"
  )

  write_rdz(x, path)
  info <- rdz_info(path)

  expect_s3_class(info, "rdz_info")
  expect_identical(info$container_version, 3L)
  expect_identical(info$codec, "r_serial_v3")
  expect_identical(info$codec_id, 1L)
  expect_identical(info$codec_version, 3L)
  expect_gte(info$block_count, 2L)
  expect_identical(info$block_size, 1024L * 1024L)
  expect_false(info$authoritative)
  expect_false(info$exact_attributes)
  expect_true(info$full_read_required_for_attributes)
  expect_identical(info$synopsis$root_type, "list")
  expect_identical(info$synopsis$class, c("custom_list", "list"))
  expect_setequal(info$synopsis$attribute_names, c("names", "class", "note"))
  expect_true(info$synopsis$exact_attributes_require_full_read)
  expect_setequal(
    info$integrity_checks,
    c("header_xxh3", "directory_xxh3", "directory_and_block_bounds")
  )
  output <- capture.output(visible <- withVisible(print(info)))
  expect_match(output[[1L]], "<rdz_info>", fixed = TRUE)
  expect_false(visible$visible)
  expect_identical(visible$value, info)
})

test_that("rdz_info rejects a corrupt directory without reading the payload", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(list(value = 1L), path, mode = "r")
  bytes <- readBin(path, what = "raw", n = file.info(path)$size)
  directory_byte <- length(bytes) - 40L
  bytes[[directory_byte]] <- as.raw(
    bitwXor(as.integer(bytes[[directory_byte]]), 0xffL)
  )
  writeBin(bytes, path)

  expect_error(rdz_info(path), "directory checksum mismatch")
})

test_that("generic synopsis collection does not dispatch an R length method", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  method_name <- "length.rdz_phase0b_length_probe"
  assign(
    method_name,
    function(x) stop("length method must not run"),
    envir = .GlobalEnv
  )
  on.exit(rm(list = method_name, envir = .GlobalEnv), add = TRUE)
  x <- structure(1:3, class = "rdz_phase0b_length_probe")

  expect_no_error(write_rdz(x, path, mode = "r"))
  expect_identical(rdz_info(path)$synopsis$length, 3)
  expect_true(identical(read_rdz(path), x))
})

test_that("bounded synopsis cannot prevent generic whole-root coverage", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  long_class <- paste(rep.int("🙂", 1000L), collapse = "")
  x <- structure(1L, class = long_class)
  attr(x, paste0("attribute_", paste(rep.int("x", 1000L), collapse = ""))) <- 2L

  expect_no_error(write_rdz(x, path, mode = "r"))
  expect_true(identical(read_rdz(path), x))
  expect_true(rdz_info(path)$synopsis$truncated)
  # natively, a long attribute name is stored whole
  write_rdz(x, path)
  expect_true(identical(read_rdz(path), x))
})

test_that("Rust temporary files are cleaned after atomic replacement", {
  directory <- tempfile("rdz-atomic-")
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  path <- file.path(directory, "object.rdz")

  write_rdz("first", path)
  write_rdz("second", path)

  expect_identical(read_rdz(path), "second")
  expect_identical(list.files(directory, all.files = TRUE, no.. = TRUE), "object.rdz")
})

test_that("paths are validated", {
  expect_error(read_rdz(character()), "single")
  expect_error(read_rdz(NA_character_), "single")
  expect_error(write_rdz(1L, tempdir()), "must not refer to a directory")
})

test_that("every error rdz raises is an rdz_error of a documented class", {
  path <- tempfile(fileext = ".rdz")
  on.exit(unlink(path), add = TRUE)
  write_rdz(data.frame(a = 1:3), path)
  expect_error(read_rdz(path, rows = 0), class = "rdz_argument_error")
  expect_error(read_rdz(path, select = "nope"), class = "rdz_argument_error")
  expect_error(write_rdz(1, path, metadata = c("x")), class = "rdz_argument_error")
  expect_error(read_rdz(tempfile()), class = "rdz_io_error")
  expect_error(write_rdz(1, file.path(tempfile(), "x.rdz")), class = "rdz_io_error")
  expect_error(rdz_save(file = path), class = "rdz_error")
  writeBin(as.raw(1:10), path)
  expect_error(read_rdz(path), class = "rdz_format_error")
})

test_that("writing through a symbolic link replaces the file it names", {
  skip_on_os("windows")
  dir <- tempfile("rdz-link-")
  dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE), add = TRUE)
  target <- file.path(dir, "data.rdz")
  link <- file.path(dir, "latest.rdz")
  write_rdz(1:3, target)
  skip_if_not(file.symlink(target, link), "no symbolic links here")
  write_rdz(4:6, link)
  expect_true(nzchar(Sys.readlink(link)))
  expect_identical(read_rdz(target), 4:6)
  expect_identical(read_rdz(link), 4:6)
  expect_setequal(list.files(dir), c("data.rdz", "latest.rdz"))
  # a relative link, through another link
  rel <- file.path(dir, "rel.rdz")
  skip_if_not(file.symlink("latest.rdz", rel), "no relative links here")
  write_rdz(7:9, rel)
  expect_identical(read_rdz(target), 7:9)
  expect_setequal(list.files(dir), c("data.rdz", "latest.rdz", "rel.rdz"))
})
