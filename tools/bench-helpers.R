options(tibble.width = Inf)
# write benchmark

bench_write <- function(x, iterations = 10L, dir = tempdir()) {
  files <- c(
    rdz = file.path(dir, "bench.rdz"),
    qd = file.path(dir, "bench.qs2"),
    qs2 = file.path(dir, "bench.qs"),
    fst = file.path(dir, "bench.fst"),
    base = file.path(dir, "bench.rds")
  )
  on.exit(unlink(files), add = TRUE)

  res <- bench::mark(
    rdz = rdz::write_rdz(x, files[["rdz"]]),
    qd = qs2::qd_save(x, files[["qd"]]),
    qs2 = qs2::qs_save(x, files[["qs2"]]),
    fst = fst::write_fst(as.data.frame(x), files[["fst"]]),
    base = base::saveRDS(x, files[["base"]]),
    iterations = iterations,
    check = FALSE,
    filter_gc = FALSE
  )

  res$file_size <- bench::as_bench_bytes(
    file.info(files[as.character(res$expression)])$size
  )
  res[order(res$median), ]
}

# read benchmark

bench_read <- function(x, iterations = 10L, dir = tempdir()) {
  files <- c(
    rdz = file.path(dir, "bench.rdz"),
    qd = file.path(dir, "bench.qs2"),
    qs2 = file.path(dir, "bench.qs"),
    fst = file.path(dir, "bench.fst"),
    base = file.path(dir, "bench.rds")
  )
  on.exit(unlink(files), add = TRUE)

  # seed one file per back-end to read back
  rdz::write_rdz(x, files[["rdz"]])
  qs2::qd_save(x, files[["qd"]])
  qs2::qs_save(x, files[["qs2"]])
  fst::write_fst(as.data.frame(x), files[["fst"]])
  base::saveRDS(x, files[["base"]])

  res <- bench::mark(
    rdz = rdz::read_rdz(files[["rdz"]]),
    qd = qs2::qd_read(files[["qd"]]),
    qs2 = qs2::qs_read(files[["qs2"]]),
    fst = fst::read_fst(files[["fst"]]),
    base = base::readRDS(files[["base"]]),
    iterations = iterations,
    check = FALSE,
    filter_gc = FALSE
  )

  res$file_size <- bench::as_bench_bytes(
    file.info(files[as.character(res$expression)])$size
  )
  res[order(res$median), ]
}
