# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

`rdz` is an R package (with a C backend) that serializes R objects with the goal of
minimizing latency, deliberately trading file size for speed. Public API is four
exported functions in [R/rdz.R](R/rdz.R): `write_rdz()`, `read_rdz()`,
`explain_rdz()`, and `rdz_threads()`. All four are thin wrappers over `.Call`
entry points registered in [src/init.c](src/init.c).

## Commands

The package requires compilation (`NeedsCompilation: yes`). Use the R toolchain:

```r
devtools::load_all(".")   # compile C and load for interactive work
devtools::document()      # regenerate NAMESPACE + man/ from roxygen (RoxygenNote 8.0.0)
devtools::test()          # run testthat suite (edition 3)
devtools::check()         # full R CMD check
```

Run a single test file or filter by name:

```r
testthat::test_file("tests/testthat/test-roundtrip.R")
devtools::test(filter = "roundtrip")   # matches test-roundtrip.R
```

Command-line equivalents:

```sh
R CMD build . && R CMD check rdz_*.tar.gz
```

Benchmarks (not run by the test suite; require Suggests `qs2`, `fst`, `bench`):

```sh
Rscript inst/benchmarks/benchmark.R          # vs qs2 / qdata
Rscript inst/benchmarks/data-frames-fst.R    # vs fst; RDZ_FST_THREADS=1 for single-threaded control
```

Environment knobs: `RDZ_BENCH_N`, `RDZ_BENCH_ITERATIONS`, `RDZ_FST_THREADS`.

## Architecture

Two codecs, selected per object. `codec = "auto"` (default) uses the native path
when it can preserve the object and falls back to R serialization otherwise;
`"native"` errors instead of falling back; `"r"` forces R serialization.

- **Native codec** — a recursive, single-pass tree serializer for atomic vectors,
  strings, lists, matrices, and data frames. The reader memory-maps files on
  POSIX (`mmap`) and uses a Windows fallback. This path does **not** preserve
  shared list-node identity; recursive objects, ALTREP vectors, and unsupported
  types deliberately route to R serialization.
- **R codec** — a direct, uncompressed R serialization stream for everything the
  native path can't represent.

### Two on-disk formats keyed by preset

- `preset = "speed"` (default) → `FASTRDS1`: node header + raw `memcpy` of vector
  payloads; strings use a dictionary-or-flat layout.
- `preset = "balanced"` → `FASTRDS2`: adds size-reducing transforms, each gated on
  an *exact* or measured saving check (not a heuristic guess):
  - 2-bit logical packing (incl. `NA`)
  - frame-of-reference integer bit-packing (≤8-bit ranges, `NA` code only if needed)
  - numeric: low-cardinality bit-pattern dictionary, exact constant-delta
    sequences (e.g. `Date`/`POSIXct`), and XOR-delta + byte-transpose + LZ4 for
    high-cardinality doubles — the XOR/LZ4 path requires ≥12.5% saving on both a
    first-block control point and the full payload
  - flat string payloads in independent 64 KiB LZ4 blocks + RLE-varint lengths,
    each chosen only when the complete encoded layout is smaller than raw

**The on-disk magic tags remain `FASTRDS1`/`FASTRDS2` even though the package was
renamed to rdz.** Do not change them — they preserve backward compatibility with
pre-rename files.

### C source layout ([src/](src/))

- [rdz.c](src/rdz.c) — the bulk of the logic: file I/O, mmap, the tree
  writer/reader, format dispatch, and `C_rdz_save/read/explain`. Node types and
  per-type encoding enums are defined at the top.
- `logical_codec.c`, `integer_codec.c`, `numeric_codec.c` (+ headers) — the
  per-type balanced encoders/decoders. See [numeric_codec.h](src/numeric_codec.h)
  for the dictionary/sequence/XOR contracts.
- [threading.c](src/threading.c) — pthread work distribution behind
  `rdz_threads()`; caps at `RDZ_MAX_THREADS` (64) and, on Linux, respects CPU
  affinity and cgroup v1/v2 quotas. Built with `-pthread` (see [src/Makevars](src/Makevars)).
- `vendor/lz4/` — vendored LZ4 1.10.0, isolated with its own provenance in
  [inst/COPYRIGHTS](inst/COPYRIGHTS). Treat as third-party; don't edit.

`explain_rdz()` runs the *real* serializer into a temporary stream and reports the
per-node codec/strategy/byte counts, so it costs roughly the same as writing.

## Constraints worth knowing

- Native files use the writer's byte order and C numeric widths; the reader
  rejects files from incompatible architectures. There is no checksum — this is
  not an archival format.
- Decode paths validate encoding tags, indexes, padding, overflow, and truncated
  input; preserve that validation when editing codecs.
- CI ([.github/workflows/native-checks.yml](.github/workflows/native-checks.yml))
  runs ASAN/UBSAN sanitizers, valgrind, LTO, gctorture, and rchk via reusable
  workflows in `pedrobtz/r-actions`. Memory-safety and PROTECT correctness matter;
  write C accordingly.
