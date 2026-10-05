# Byte-level surgery on rdz files for tests of hostile input: every
# checksum a change touches is recomputed, so only the intended field is
# wrong. Offsets are 0-based, as in container-format.md.

le_u64 <- function(bytes, at) sum(as.numeric(bytes[at + 1:8]) * 256^(0:7))

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
  entries <- dir + 40 + 48 * nobj + 32 * natt
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
