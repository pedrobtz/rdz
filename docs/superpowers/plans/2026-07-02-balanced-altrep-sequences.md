# Balanced ALTREP Sequences Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Encode integer and double constant-delta ALTREP vectors compactly under the balanced preset without materializing them or changing the speed layout.

**Architecture:** Add bounded `*_GET_REGION()` sequence scanners in the native serializer. Double ALTREP sequences reuse the existing numeric sequence representation; integer sequences gain a balanced-format encoding containing an exact 32-bit base and delta, with overflow-checked decoding. Non-sequences retain streamed direct storage.

**Tech Stack:** R 4.1 C API, ALTREP region APIs, rdz `FASTRDS2`, testthat 3, rcmdcheck.

---

### Task 1: Specify balanced ALTREP behavior

**Files:**
- Modify: `tests/testthat/test-roundtrip.R`

- [x] Add tests requiring large ascending and descending integer/double ALTREP sequences to report constant-delta strategies and produce compact balanced files.
- [x] Add short ALTREP vectors that remain streamed direct.
- [x] Verify speed files retain their existing direct sizes and bytes.
- [x] Run `Rscript -e 'devtools::test(filter = "roundtrip")'` and confirm the compact-strategy assertions fail.

### Task 2: Add bounded sequence scanners and integer encoding

**Files:**
- Modify: `src/rdz.c`

- [x] Add bounded integer and double scanners using `INTEGER_GET_REGION()` and `REAL_GET_REGION()`.
- [x] Require at least three finite/non-missing values and bit-for-bit reconstruction for doubles.
- [x] Add `INTEGER_ENCODING_SEQUENCE` with signed 32-bit base and delta fields.
- [x] Decode integer sequences using 64-bit arithmetic and reject overflow, `NA_INTEGER`, and malformed short sequences.
- [x] Reuse `REAL_ENCODING_SEQUENCE` for exact double ALTREP sequences.
- [x] Keep speed mode and short balanced ALTREP vectors on streamed direct layouts.

### Task 3: Validate corruption handling and compatibility

**Files:**
- Modify: `tests/testthat/test-roundtrip.R`

- [x] Test minimum-length selection, negative deltas, integer boundaries, and malformed integer sequence metadata.
- [x] Verify materialized double sequences and ALTREP double sequences use the same balanced bytes.
- [x] Run the full test suite and both R 4.6 and forced R 4.1 attribute paths.

### Task 4: Document and check

**Files:**
- Modify: `README.md`
- Modify: `NEWS.md`
- Modify: `working-on.md`

- [x] Document balanced ALTREP sequence encoding and direct fallback for irregular values.
- [x] Run `air format R tests inst/benchmarks` without retaining unrelated formatting changes.
- [x] Run strict `R CMD check --as-cran --no-manual` and require zero errors and warnings.
- [x] Run `git diff --check` and inspect the final diff.
