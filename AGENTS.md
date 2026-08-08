# Repository Guidelines

## Project Structure & Module Organization

`rdz` is an R package whose serialization format is implemented in Rust. User-facing code lives in `R/`; Rust code and Cargo files live in `src/rust/`. Keep format, codec, and IO logic in ordinary Rust modules, independent of R, with thin `#[savvy]` boundary functions. Tests are in `tests/testthat/`; benchmarks are in `tools/`.

Do not edit `R/000-wrappers.R`, `src/init.c`, or `src/rust/api.h`; Savvy generates them from marked Rust interfaces.

## Build, Test, and Development Commands

- `Rscript -e 'savvy::savvy_update(); roxygen2::roxygenise()'` regenerates bindings and R documentation after changing a `#[savvy]` signature or its `///` roxygen comments.
- `Rscript -e 'testthat::test_local(reporter = "summary")'` runs R integration tests.
- `cargo test --manifest-path src/rust/Cargo.toml` runs pure Rust tests that do not require R.
- `savvy-cli test src/rust` runs `#[cfg(feature = "savvy-test")]` tests inside R.
- `cargo fmt --check --manifest-path src/rust/Cargo.toml` checks Rust formatting.
- `R CMD INSTALL .` installs locally. For release checks, run `R CMD build .`, then `R CMD check --no-manual rdz_0.0.0.9000.tar.gz`.

Although `Cargo.toml` uses `savvy = "*"`, the committed lockfile resolves Savvy 0.10.2, which requires R 4.5 and Rust 1.88. Keep `Cargo.lock` committed and treat Savvy upgrades as compatibility changes: review its changelog, generated files, build templates, and minimum versions. Windows also needs the `x86_64-pc-windows-gnu` Rust target.

## CRAN Rust Packaging Requirements

Follow CRAN's [Using Rust in CRAN packages](https://cran.r-project.org/web/packages/using_rust.html) guidance when changing dependencies or build infrastructure. Prefer bundling all Rust dependencies with `cargo vendor` so installation and checks do not require network access; package vendored sources as an `xz`-compressed tar archive. If a download is unavoidable, pin an exact version, use a secure long-term host under the maintainer's control, embed and verify its checksum, and do not rely on GitHub as the sole source.

List `Cargo` and `rustc` separately in `DESCRIPTION` under `SystemRequirements`, with conservative minimum versions justified by this package and its transitive crates. Include authorship and copyright information for all bundled Rust code, including dependencies, in `DESCRIPTION`. Test release builds with Cargo versions at least two years old, and preferably four or more years old, because CRAN systems may use older distribution toolchains.

The `configure` and `configure.win` scripts must find both `cargo` and `rustc`, checking the system `PATH` and `~/.cargo/bin`, validate required versions, and print the selected `rustc` version before compilation. If no suitable toolchain exists, give installation instructions but never install Rust on the user's behalf. Invoke Cargo with an explicit parallel-job limit of one or two jobs to stay within CRAN policy.

## Coding Style & Savvy Boundary Rules

Use two-space indentation, `<-`, and `snake_case` in R. Use `rustfmt` and `snake_case` in Rust. Exported Rust functions must return `savvy::Result<()>`, `savvy::Result<savvy::Sexp>`, or a supported `#[savvy]` type.

Input SEXP wrappers are read-only; allocate the corresponding `Owned...Sexp` for output and propagate fallible operations with `?`. Savvy performs no implicit R coercion, so put user-friendly casting and validation in R wrappers where practical. Preserve missing values explicitly with `savvy::NotAvailableValue`; ordinary logical iteration maps `NA` to `true`, which is unacceptable for serialization.

Savvy SEXP wrappers are neither `Send` nor `Sync`. Never move them to worker threads or call the R API there. Extract or copy data on the R thread, parallelize only owned Rust buffers, then allocate R results back on the R thread. Use `savvy_err!` or `?` for recoverable failures. Avoid `panic!`, `unwrap()`, and unchecked indexing at the R boundary: release builds use `panic = "abort"`, so a panic terminates R.

If a dependency adds native libraries, build with `RUSTFLAGS=--print=native-static-libs` and add platform-specific linker flags to the Makevars/configure workflow. Consult the [Savvy guide](https://yutannihilation.github.io/savvy/guide/) and [0.10.2 API](https://docs.rs/savvy/0.10.2/savvy/).

## Testing Guidelines

Name files `test-*.R`, helpers `helper-*.R`, and use descriptive `test_that()` labels. R-side tests are authoritative. Keep format/codec tests independent of Savvy. Savvy-dependent Rust tests use `#[cfg(feature = "savvy-test")]`, return `savvy::Result<()>`, and require `savvy-test = []` in `Cargo.toml`.

Every API or format change needs round-trip tests using `expect_identical()`. Cover missing values, attributes, encodings, empty objects, reference sharing, boundary lengths, malformed input, and temporary-file cleanup. Commit a legacy fixture before changing a stable on-disk format.

## Commit & Pull Request Guidelines

History establishes no convention beyond `Initial commit`. Use short imperative subjects, such as `Add corruption checks for rdz files`. PRs should describe behavior and compatibility impact, link issues, and report R and Rust test/check results. Include benchmarks for performance changes and call out platform-specific behavior.

Do not commit `src/rust/target/`, generated `src/Makevars` or `src/Makevars.win`, credentials, local state, or benchmark output. Keep the tracked `.in` templates.
