# Data Table, ALTREP, and Portability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add native top-level `data.table` support, stream common unmaterialized ALTREP numeric vectors without changing the file format, and finish the release portability gate.

**Architecture:** The native encoder will omit only a top-level data table's runtime-only `.internal.selfref` attribute and the R reader will rebuild it with `data.table::setDT()` when available. Unmaterialized integer and real ALTREP vectors will be accepted by capability detection and written in bounded chunks through `*_GET_REGION`; balanced mode will conservatively use the existing direct layouts. Release hardening will remove source-package detritus and inaccessible URLs, then validate macOS and Linux builds.

**Tech Stack:** R 4.6 C API, testthat 3, data.table, ALTREP region APIs, rcmdcheck, Docker/Colima when available.

---

### Task 1: Native top-level data.table support

**Files:**
- Modify: `src/rdz.c`
- Modify: `R/rdz.R`
- Modify: `DESCRIPTION`
- Modify: `tests/testthat/test-roundtrip.R`
- Modify: `inst/benchmarks/simple.R`
- Modify: `README.md`
- Modify: `NEWS.md`

- [x] **Step 1: Add failing data-table tests**

Add tests that create keyed and unkeyed tables, require `explain_rdz(x, codec = "native")$codec[[1L]] == "native"`, round-trip through `codec = "native"`, compare columns/classes/keys, and verify `:=` works without warning. Add a nested-data-table test that continues to reject forced native encoding.

- [x] **Step 2: Verify the tests fail before implementation**

Run:

```sh
Rscript -e "devtools::test(filter = 'roundtrip')"
```

Expected: top-level data tables report `object is not supported by the native codec`.

- [x] **Step 3: Omit only the runtime self-reference during native encoding**

In `src/rdz.c`, detect a top-level object inheriting from `data.table`. During capability traversal and attribute encoding, skip an attribute only when its tag is `.internal.selfref` and its value is `EXTPTRSXP`. Adjust the serialized attribute count accordingly. Do not skip the attribute for nested data tables, so those objects retain the existing safe fallback.

- [x] **Step 4: Rebuild the top-level data table after reading**

Change `read_rdz()` to assign the native result and repair it without copying:

```r
object <- .Call(C_rdz_read, path.expand(as.character(file)))
if (
  inherits(object, "data.table") &&
    requireNamespace("data.table", quietly = TRUE)
) {
  data.table::setDT(object)
}
object
```

Declare `data.table` in `Suggests`.

- [x] **Step 5: Verify native behavior and benchmark intent**

Add this assertion before timing the taxi data:

```r
stopifnot(rdz::explain_rdz(taxi, codec = "native")$codec[[1L]] == "native")
```

Run the focused tests and a reduced taxi benchmark. Expected: exact class/key preservation and no fallback row for rdz.

- [x] **Step 6: Document the special treatment**

Document that top-level data tables use the native codec, runtime self-reference state is reconstructed when data.table is installed, and nested data tables still fall back.

### Task 2: Stream common ALTREP sequences

**Files:**
- Modify: `src/rdz.c`
- Modify: `tests/testthat/test-roundtrip.R`
- Modify: `README.md`
- Modify: `NEWS.md`

- [x] **Step 1: Add failing ALTREP tests**

Test large `seq_len()` integer vectors, `as.double(seq_len())` real vectors, and a tibble containing an ALTREP identifier. Require native codec selection under speed and balanced presets, exact values/classes after reading, and a reported strategy containing `ALTREP` and `streamed direct`.

- [x] **Step 2: Verify current fallback**

Run:

```sh
Rscript -e "devtools::test(filter = 'roundtrip')"
```

Expected: the new compact-sequence cases select R serialization or reject `codec = "native"`.

- [x] **Step 3: Permit safe unmaterialized ALTREP types**

Change native capability detection so an ALTREP object with no data pointer is accepted only for `INTSXP` and `REALSXP`. Continue rejecting character, list, logical, complex, and raw ALTREP objects until each has an explicit bounded writer.

- [x] **Step 4: Write ALTREP values in bounded regions**

Add a fixed-size chunk writer using `INTEGER_GET_REGION()` and `REAL_GET_REGION()`. For unmaterialized ALTREP integer and real vectors, skip pointer-based balanced analyses, emit the existing direct encoding tag where required, and stream bytes through `writer_write()`. Fail cleanly if a region call returns fewer values than requested.

- [x] **Step 5: Verify format and memory behavior**

Run round-trip tests under both presets and compare files against equivalent materialized vectors. Expected: identical decoded values and the same existing direct on-disk layout, without materializing the source during capability detection.

- [x] **Step 6: Update documentation**

Replace the blanket ALTREP fallback statement with the precise contract: unmaterialized integer and real ALTREP vectors stream directly; other compact representations retain R serialization fallback.

### Task 3: Release and portability hardening

**Files:**
- Modify: `.Rbuildignore`
- Modify: `DESCRIPTION`
- Modify: `working-on.md`
- Test: package source tarball and Linux container

- [x] **Step 1: Remove known source-package notes**

Add `^CLAUDE[.]md$` to `.Rbuildignore`. Remove `URL` and `BugReports` while the repository returns 404 to unauthenticated CRAN checks; restore them when the repository is public.

- [x] **Step 2: Run formatting and focused tests**

Run:

```sh
air format R tests inst/benchmarks
Rscript -e "devtools::test()"
```

Expected: all tests pass with no warnings.

- [x] **Step 3: Run the strict source-package check**

Run:

```sh
Rscript -e 'rcmdcheck::rcmdcheck(args = c("--no-manual", "--as-cran"), build_args = "--no-manual", error_on = "warning")'
```

Expected: zero errors and warnings; only the standard new-submission note may remain.

- [x] **Step 4: Validate Linux compilation and cgroup behavior**

If Docker or Colima is available, run the package tests in a Linux container constrained with `--cpus=1.5` and assert `rdz_threads() == 2L`. Otherwise cross-compile `src/threading.c` for Linux with strict warnings and record the missing runtime validation in `working-on.md`.

- [x] **Step 5: Refresh the tracking document**

Update `working-on.md` with the data-table contract, ALTREP contract, current test count, strict-check result, Linux/container result, and remaining Windows/old-release CI work.
