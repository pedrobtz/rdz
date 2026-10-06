# Byte-level surgery on rdz files for tests of hostile input: every
# checksum a change touches is recomputed, so only the intended field is
# wrong. Offsets are 0-based, as in container-format.md.

le_u64 <- function(bytes, at) sum(as.numeric(bytes[at + 1:8]) * 256^(0:7))

# The directory header's length (40, or 64 with a content hash): where the
# object entries begin, after the directory's offset.
dir_header_len <- function(bytes, dir) sum(as.numeric(bytes[dir + 7:8]) * 256^(0:1))

xxh3_le <- function(r) {
  testthat::skip_if_not_installed("zufast")
  h <- zufast::fast_hash(r)
  rev(as.raw(strtoi(substring(h, seq(1, 15, 2), seq(2, 16, 2)), 16L)))
}

# Recomputes the checksums of the block whose stored bytes start at
# `payload` (its header's and its directory entry's), and the directory's.
reseal_block <- function(bytes, payload) {
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  dir_len <- le_u64(bytes, n - 24L)
  nobj <- sum(as.numeric(bytes[dir + 17:20]) * 256^(0:3))
  natt <- sum(as.numeric(bytes[dir + 21:24]) * 256^(0:3))
  nblk <- sum(as.numeric(bytes[dir + 25:28]) * 256^(0:3))
  entries <- dir + dir_header_len(bytes, dir) + 48 * nobj + 32 * natt
  for (k in seq_len(nblk) - 1L) {
    e <- entries + 64 * k
    if (le_u64(bytes, e + 16) != payload) next
    stored <- sum(as.numeric(bytes[e + 25:28]) * 256^(0:3))
    h <- xxh3_le(bytes[payload + seq_len(stored)])
    bytes[e + 57:64] <- h
    bytes[payload - 48 + 41:48] <- h
  }
  bytes[n - 16 + 1:8] <- xxh3_le(bytes[dir + seq_len(dir_len)])
  bytes
}

# A file's bytes as a writer without content hashes writes them: the
# directory header's hash extension (bytes 40 to 63 of a 64-byte header)
# removed, the lengths and checksums that cover it resealed, and the
# header's writer field and checksum zeroed (bytes_but_writer()).
bytes_without_hash <- function(path) {
  bytes <- readBin(path, "raw", file.size(path))
  n <- length(bytes)
  dir <- le_u64(bytes, n - 32L)
  header_len <- sum(as.numeric(bytes[dir + 7:8]) * 256^(0:1))
  if (header_len == 64) {
    bytes <- bytes[-(dir + 41:64)]
    n <- length(bytes)
    bytes[dir + 7:8] <- as.raw(c(40L, 0L))
    bytes[dir + 33:40] <- xxh3_le(bytes[dir + 1:32])
    dir_len <- le_u64(bytes, n - 24L) - 24
    bytes[n - 24 + 1:8] <- as.raw((dir_len %/% 256^(0:7)) %% 256)
    bytes[n - 16 + 1:8] <- xxh3_le(bytes[dir + seq_len(dir_len)])
  }
  bytes[21:32] <- as.raw(0L)
  bytes
}

# A raw vector's bytes but the writer field (bytes_but_writer() of a file).
bytes_but_writer_raw <- function(bytes) {
  bytes[21:32] <- as.raw(0L)
  bytes
}
