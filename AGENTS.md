# Repository Guidelines

## Project Structure & Module Organization

`rdz` is an R package being re-implemented in C on zubin and zufast (`.agents/plan-c.md`, adopted 2026-10-05). User-facing code lives in `R/`. The C implementation is `src/core/` (R-free: records, container reader and writer, file IO; it never includes an R header and compiles standalone into `tools/c-tests/` and `fuzz/`) and `src/rdz_r.c` (the `.Call` entry points), registered by `src/rdz_init.c`. Until the C port replaces it stage by stage, the Rust implementation in `src/rust/` is the oracle: `configure` builds it when R is 4.5 or newer and cargo and rustc 1.88 are available (`RDZ_RUST=0` disables it, `RDZ_RUST=1` requires it), and tests that need it call `skip_without_rust()`. Tests are in `tests/testthat/`; the Rust reference corpus is in `tests/testthat/fixtures/rust/`; benchmarks and scripts are in `tools/`.

Savvy generated `R/000-wrappers.R`, `src/init.c` and `src/rust/api.h`; savvy-cli is no longer run, and they change only as the Rust implementation is retired.

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
- `tools/run-c-tests` runs the R-free C core under its harness (warnings are errors, ASan and UBSan on), against the Rust reference corpus.
- A version bump in `DESCRIPTION` updates `RDZ_WRITER_*` in `src/core/rdz_format.h` (the header's writer field; `test-c-container.R` checks the two agree).
- `tools/run-mutation-check` proves each `/* GUARD: name */` reader guard load-bearing against its hostile file in `tools/c-tests/probe.c`.
- `tools/run-fuzz [seconds]` fuzzes the container reader after its canary has crashed; `tools/run-fuzz --replay` runs the seeds and corpus once where the compiler has no libFuzzer.
- `RDZ_RUST=0 R CMD INSTALL --preclean .` builds the C implementation alone.
- `Rscript -e 'testthat::test_local(reporter = "summary")'` runs R integration tests.
- `cargo test --manifest-path src/rust/Cargo.toml` runs pure Rust tests that do not require R.
- `savvy-cli test src/rust` runs `#[cfg(feature = "savvy-test")]` tests inside R.
- `cargo fmt --check --manifest-path src/rust/Cargo.toml` checks Rust formatting.
- `R CMD INSTALL .` installs locally. For release checks, run `R CMD build .`, then `R CMD check --no-manual rdz_0.0.0.9000.tar.gz`.

`Cargo.toml` and the committed lockfile pin Savvy 0.10.2, which requires R 4.5 and Rust 1.88. Keep `Cargo.lock` committed and treat Savvy upgrades as compatibility changes: review its changelog, generated files, build templates, and minimum versions. Windows also needs the `x86_64-pc-windows-gnu` Rust target.

## CRAN Rust Packaging Requirements

Follow CRAN's [Using Rust in CRAN packages](https://cran.r-project.org/web/packages/using_rust.html) guidance when changing dependencies or build infrastructure. Prefer bundling all Rust dependencies with `cargo vendor` so installation and checks do not require network access; package vendored sources as an `xz`-compressed tar archive. If a download is unavoidable, pin an exact version, use a secure long-term host under the maintainer's control, embed and verify its checksum, and do not rely on GitHub as the sole source.

List `Cargo` and `rustc` separately in `DESCRIPTION` under `SystemRequirements`, with conservative minimum versions justified by this package and its transitive crates. Include authorship and copyright information for all bundled Rust code, including dependencies, in `DESCRIPTION`. Test release builds with Cargo versions at least two years old, and preferably four or more years old, because CRAN systems may use older distribution toolchains.

The `configure` and `configure.win` scripts must find both `cargo` and `rustc`, checking the system `PATH` and `~/.cargo/bin`, validate required versions, and print the selected `rustc` version before compilation. If no suitable toolchain exists, give installation instructions but never install Rust on the user's behalf. Invoke Cargo with an explicit parallel-job limit of one or two jobs to stay within CRAN policy.

## C Rules

C99, four-space indentation, `rdz_` for internals and `.Call` entry points, `rdz_test_` for test-only entry points. The core never includes an R header and allocates only through zubin's `zb_buf`. Records are zubin layout specifications (`src/core/rdz_records.h`) checked by `rdz_records_check()`; read and write them with zubin's `zb_rd_`/`zb_wr_` at their offsets, never through a struct. Every count, length and offset from a file is bounds-checked before it sizes an allocation or a read. C never raises an rdz error: a failure returns an `rdz_failure` string that `rdz_check()` raises as a condition inheriting `rdz_error`. Heap state that must survive a longjmp hangs off an external pointer created before the first allocation, with a finalizer that releases it. Only the R thread touches SEXPs or calls R.

## Coding Style & Savvy Boundary Rules (the Rust oracle)

Use two-space indentation, `<-`, and `snake_case` in R. Use `rustfmt` and `snake_case` in Rust. Exported Rust functions must return `savvy::Result<()>`, `savvy::Result<savvy::Sexp>`, or a supported `#[savvy]` type.

Input SEXP wrappers are read-only; allocate the corresponding `Owned...Sexp` for output and propagate fallible operations with `?`. Savvy performs no implicit R coercion, so put user-friendly casting and validation in R wrappers where practical. Preserve missing values explicitly with `savvy::NotAvailableValue`; ordinary logical iteration maps `NA` to `true`, which is unacceptable for serialization.

Savvy SEXP wrappers are neither `Send` nor `Sync`. Never move them to worker threads or call the R API there. Extract or copy data on the R thread, parallelize only owned Rust buffers, then allocate R results back on the R thread. Use `savvy_err!` or `?` for recoverable failures. Avoid `panic!`, `unwrap()`, and unchecked indexing at the R boundary: release builds use `panic = "abort"`, so a panic terminates R.

If a dependency adds native libraries, build with `RUSTFLAGS=--print=native-static-libs` and add platform-specific linker flags to the Makevars/configure workflow. Consult the [Savvy guide](https://yutannihilation.github.io/savvy/guide/) and [0.10.2 API](https://docs.rs/savvy/0.10.2/savvy/).

## Testing Guidelines

Name files `test-*.R`, helpers `helper-*.R`, and use descriptive `test_that()` labels. R-side tests are authoritative. Keep format/codec tests independent of Savvy. Savvy-dependent Rust tests use `#[cfg(feature = "savvy-test")]`, return `savvy::Result<()>`, and require `savvy-test = []` in `Cargo.toml`.

Every API or format change needs round-trip tests. Use `expect_identical()` for
ordinary values; use semantic graph/behavior assertions for reference objects
that base R reconstructs rather than preserving by original identity, and for
objects with explicitly ignored transient metadata. Cover
missing values, ordinary and custom attributes, encodings, empty objects,
reference sharing, boundary
lengths, malformed input, automatic whole-root R-serialization fallback,
strict-native rejection, and temporary-file cleanup. Commit a compatibility
fixture before changing an on-disk format released in 0.1.0 or later. Pre-0.1.0
files and fixtures have no backward-compatibility requirement and may be replaced
when the format changes. Every on-disk change must satisfy
`.agents/portability.md`, update cross-OS fixtures where bytes change, and remain
readable on the Windows, macOS, and Linux CI matrix.

## Commit & Pull Request Guidelines

History establishes no convention beyond `Initial commit`. Use short imperative subjects, such as `Add corruption checks for rdz files`. PRs should describe behavior and compatibility impact, link issues, and report R and Rust test/check results. Include benchmarks for performance changes and call out platform-specific behavior.

Do not commit `src/rust/target/`, generated `src/Makevars` or `src/Makevars.win`, credentials, local state, or benchmark output. Keep the tracked `.in` templates.
