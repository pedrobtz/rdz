# Repository Guidelines

## Project Structure & Module Organization

`rdz` is an R package implemented in C on zubin and zufast (`.agents/plan-c.md`, adopted 2026-10-05; the Rust implementation it replaced was retired at Stage I). User-facing code lives in `R/`. The C implementation is `src/core/` (R-free: records, container reader and writer, codecs, pipeline, file IO; it never includes an R header and compiles standalone into `tools/c-tests/` and `fuzz/`), `src/adapter/` (the R boundary: the generic codec's streams, the native planner and reader) and `src/rdz_r.c` (the `.Call` entry points), registered by `src/rdz_init.c`. Tests are in `tests/testthat/`: the frozen 0.1.0 corpus in `fixtures/v0.1.0/` and the retired Rust implementation's corpus in `fixtures/rust/`, both read-compatibility data; benchmarks and scripts are in `tools/`.

Project design and implementation guidance lives in:

- [Plan C: re-implementing rdz in C](.agents/plan-c.md) (adopted 2026-10-05; the current sequence)
- [Current implementation assessment](.agents/current-state.md)
- [Native serializer architecture](.agents/architecture.md)
- [Pre-0.1 container wire format](.agents/container-format.md)
- [Performance and parallelism design](.agents/performance.md)
- [Native serialization roadmap](.agents/roadmap.md)
- [Validation and benchmarking methodology](.agents/validation.md)
- [Complete R SEXP coverage matrix](.agents/sexp-coverage.md)
- [Cross-OS file portability contract](.agents/portability.md)
- [Metadata and selective attribute access](.agents/metadata-access.md)
- [Encoding and format research directions](.agents/encoding-research.md)
- [`qs2`, `qdata`, and `fst` research](.agents/research.md)

Treat `current-state.md` as the dated implementation checkpoint,
`architecture.md` as the target component boundary, `roadmap.md` as the
implementation sequence, `validation.md` as the completion gate,
`container-format.md` as the implemented pre-release framing contract,
`sexp-coverage.md` as the type-handling registry, `portability.md` as the wire
format portability contract, `metadata-access.md` as the selective inspection
contract, and `encoding-research.md` as the candidate algorithm register. Update
the documents when a deliberate design decision changes rather than allowing the
implementation and guidance to diverge.

The public serializer must cover every object supported by base R serialization.
Use dedicated native codecs for common types and whole-root R serialization for
rare or unsupported types such as closures and environments. Never embed
independently R-serialized fallback fragments inside a native object graph because
that can break sharing and cycles across the codec boundary.

Attributes are part of the R value and must be preserved, including custom and
class-defining attributes. A native codec may write an object only when it can
preserve every attribute and its value; otherwise automatic mode must serialize
the complete root with R serialization and strict-native mode must reject it.
Never silently strip an unknown or unsupported attribute.

Native `.rdz` files must expose bounded container information, object/schema
descriptors, and supported attribute values without materializing unrelated data
vectors. Preserve a seekable, checksummed object/attribute/block directory in the
format from the first native version. Generic R-serialization payloads may expose
only a clearly labeled synopsis; never claim exact arbitrary attribute access
without fully unserializing the root.

External pointers are not native values. An external pointer encountered as a
root, list element, or unknown/semantic attribute makes the root ineligible for
native encoding and selects whole-root R serialization in automatic mode. The
only exception is an explicitly registered, non-semantic, reconstructible
attribute, such as `data.table`'s `.internal.selfref`; the native adapter may omit
that attribute and continue through the data-frame codec while preserving the
`c("data.table", "data.frame")` class and supported table metadata. Using the
data-frame codec is a physical-format choice, not coercion to a base data frame.
Never generalize this exception into silently dropping unknown external-pointer
attributes.

`.rdz` files must be readable across supported Windows, macOS, and Linux systems.
Use canonical little-endian fields and explicit widths for native payloads and R's
XDR stream format for whole-root fallback. Never persist host struct layout,
native-word-order scalars, pointer widths, addresses, or CPU-specific requirements.

## Build, Test, and Development Commands

- `Rscript -e 'roxygen2::roxygenise()'` regenerates R documentation.
- `tools/run-c-tests` runs the R-free C core under its harness (warnings are errors, ASan and UBSan on), against the reference corpora.
- A version bump in `DESCRIPTION` updates `RDZ_WRITER_*` in `src/core/rdz_format.h` (the header's writer field; `test-c-container.R` checks the two agree).
- `tools/run-mutation-check` proves each `/* GUARD: name */` reader guard load-bearing against its hostile file in `tools/c-tests/probe.c`.
- `tools/run-fuzz [seconds]` fuzzes the container reader after its canary has crashed; `tools/run-fuzz --replay` runs the seeds and corpus once where the compiler has no libFuzzer.
- `Rscript tools/exchange.R write DIR` and `check DIR...` exchange the frozen corpus's specs between platforms (`exchange.yaml`); `tools/make-frozen-fixtures.R` wrote the corpus once and is never rerun over it.
- `Rscript -e 'testthat::test_local(reporter = "summary")'` runs R integration tests.
- `R CMD INSTALL .` installs locally. For release checks, run `R CMD build .`, then `R CMD check --no-manual rdz_0.0.0.9000.tar.gz`.

## C Rules

C99, four-space indentation, `rdz_` for internals and `.Call` entry points, `rdz_test_` for test-only entry points. The core never includes an R header and allocates only through zubin's `zb_buf`. Records are zubin layout specifications (`src/core/rdz_records.h`) checked by `rdz_records_check()`; read and write them with zubin's `zb_rd_`/`zb_wr_` at their offsets, never through a struct. Every count, length and offset from a file is bounds-checked before it sizes an allocation or a read. C never raises an rdz error: a failure returns an `rdz_failure` string that `rdz_check()` raises as a condition inheriting `rdz_error`. Heap state that must survive a longjmp hangs off an external pointer created before the first allocation, with a finalizer that releases it. Only the R thread touches SEXPs or calls R.

## R Style

Use two-space indentation, `<-`, and `snake_case` in R.

## Testing Guidelines

Name files `test-*.R`, helpers `helper-*.R`, and use descriptive `test_that()` labels. R-side tests are authoritative; the C harness and the fuzz targets cover the R-free core.

Build a double whose bits matter (`-0`, NaN payloads) from its bytes in any function a test calls more than once: R's byte compiler folds the literal `-0` to `+0`, so the value changes once the JIT compiles the function, and `identical()` cannot tell (the content hash can).

Every API or format change needs round-trip tests. Use `expect_identical()` for
ordinary values; use semantic graph/behavior assertions for reference objects
that base R reconstructs rather than preserving by original identity, and for
objects with explicitly ignored transient metadata. Cover
missing values, ordinary and custom attributes, encodings, empty objects,
reference sharing, boundary
lengths, malformed input, automatic whole-root R-serialization fallback,
strict-native rejection, and temporary-file cleanup. The format froze at 0.1.0
(container-format.md, "Compatibility and extension"): every later rdz reads the
frozen corpus (`fixtures/v0.1.0/`) to its values, which is never regenerated; an
addition gets new fixtures beside it, and a changed meaning gets a new version. Every on-disk change must satisfy
`.agents/portability.md`, update cross-OS fixtures where bytes change, and remain
readable on the Windows, macOS, and Linux CI matrix.

## Commit & Pull Request Guidelines

History establishes no convention beyond `Initial commit`. Use short imperative subjects, such as `Add corruption checks for rdz files`. PRs should describe behavior and compatibility impact, link issues, and report R and C test/check results. Include benchmarks for performance changes and call out platform-specific behavior.

Do not commit object files, credentials, local state, or benchmark output. `src/Makevars` and `src/Makevars.win` are tracked and static.
