folder <- file.path(tempdir(), "rdz_bench")
unlink(folder, recursive = TRUE)
dir.create(folder)

cache_dir <- tools::R_user_dir("rdz", "cache")
dir.create(cache_dir, recursive = TRUE, showWarnings = FALSE)
taxi_csv <- file.path(cache_dir, "taxi.csv")

if (!file.exists(taxi_csv)) {
  base <- "https://data.cityofnewyork.us/resource/t29m-gskq.csv"
  urls <- c(
    paste0(base, "?$limit=500000&$offset=0&$order=:id"),
    paste0(base, "?$limit=500000&$offset=500000&$order=:id")
  )
  chunk_files <- file.path(
    cache_dir,
    paste0("taxi_chunk_", seq_along(urls), ".csv")
  )
  curl::multi_download(urls, chunk_files)
  chunks <- lapply(chunk_files, data.table::fread)
  data.table::fwrite(data.table::rbindlist(chunks), taxi_csv)
  unlink(chunk_files)
}

taxi <- data.table::fread(taxi_csv)
stopifnot(rdz::explain_rdz(taxi, codec = "native")$codec[[1L]] == "native")

files <- list(
  rds = file.path(folder, "taxi.rds"),
  fst = file.path(folder, "taxi.fst"),
  qs2 = file.path(folder, "taxi.qs2"),
  rdz = file.path(folder, "taxi.rdz")
)

res_write <- bench::mark(
  rds = saveRDS(taxi, file = files$rds),
  fst = fst::write_fst(taxi, path = files$fst),
  qs2 = qs2::qs_save(taxi, file = files$qs2),
  rdz = rdz::write_rdz(taxi, file = files$rdz),
  iterations = 10,
  check = FALSE
)

res_write

file_sizes <- sapply(files, function(f) {
  format(structure(file.size(f), class = "object_size"), units = "auto")
})
print(file_sizes)

res_read <- bench::mark(
  rds = readRDS(files$rds),
  fst = fst::read_fst(files$fst),
  qs2 = qs2::qs_read(files$qs2),
  rdz = rdz::read_rdz(files$rdz),
  iterations = 10,
  check = FALSE
)

res_read
