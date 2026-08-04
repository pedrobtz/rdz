# Development status

Updated: 2026-07-02

## Current objective

Build a low-latency R object serializer with a stable speed-first format and a
balanced format that applies size-reducing transforms only when they are exact
or measurably smaller.

## Current state

- `write_rdz()` and `read_rdz()` support atomic vectors, lists, matrices, data
  frames, tibbles, and top-level data tables through the native codec.
- Unsupported objects use R serialization. Nested data tables remain on this
  fallback path.
- Integer and double ALTREP vectors use bounded region reads. Exact balanced
  sequences use constant-delta encodings; speed mode and irregular values stay
  direct.
- R 4.1 through 4.5 enumerate attributes through public base R interfaces.
  R 4.6 and later use the lower-overhead C attribute API. Both paths emit the
  same representation for covered objects.
- `rdz_threads()` respects online processors and, on Linux, CPU affinity and
  cgroup v1/v2 quotas.
- `benchmark.yaml` and `native-checks.yml` are intentionally manual workflows.

See the main [README](../README.md) for the user-facing contract and
[NEWS](../NEWS.md) for completed changes.

## Verification baseline

- 348 testthat expectations pass on the normal and forced R 4.1--4.5 attribute
  paths.
- The R 4.1, release, devel, Windows, and Linux CI matrix has completed.
- Local `R CMD check --as-cran --no-manual` reports zero errors and warnings;
  its only note is the expected `New submission` note.
- The speed format remains byte-compatible with the pre-rename `FASTRDS1`
  representation, and current readers accept existing `FASTRDS2` files.
- Linux threading code cross-compiles with `-Wall -Wextra -Werror`.

Benchmark methodology, commands, and retained results are documented in the
[benchmark guide](../inst/benchmarks/README.md).

## TODO

- [ ] Add deterministic cgroup v1 and v2 tests covering nested quotas,
  fractional CPU limits, malformed files, and CPU affinity interaction.
- [ ] Add a Linux container assertion that `--cpus=1.5` produces
  `rdz_threads() == 2L`.
- [ ] Add malformed-file fuzzing under ASAN and UBSAN.

## Open decisions

The product stance on cross-architecture portability, connection/raw-vector
APIs, per-node fallback, and checksums is not yet settled. These are tracked as
decisions—not commitments—in the [roadmap](roadmap.md).
