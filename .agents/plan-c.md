# RDZ Plan C: re-implementing rdz in C

**Status:** adopted, 2026-10-05: the maintainer decided to re-implement rdz in C on this plan
(§10). The stages are under way from Stage A; nothing below Stage A is implemented yet. It is written against zubin 0.1.0 (implemented, release branch prepared,
waiting for zufast on CRAN), zufast 0.1.0 (tagged, CRAN pending) and the rdz tree at
`a80264f` plus the in-flight character dictionary work (encodings 8 and 9).

**Role among the documents.** This file decides the implementation language and sequences
the port. It does not change what rdz is: [container-format.md](container-format.md)
stays the wire contract, [portability.md](portability.md) the portability contract,
[metadata-access.md](metadata-access.md) the inspection contract,
[sexp-coverage.md](sexp-coverage.md) the type registry, [validation.md](validation.md) the
completion gate, [performance.md](performance.md) the pipeline design and
[encoding-research.md](encoding-research.md) the algorithm register. Every one of those
is language-agnostic and is amended only where §3 says. [architecture.md](architecture.md)
and [roadmap.md](roadmap.md) are the two it supersedes in part (§9).

---

## 1. The decision

**Re-implement rdz in C99, on the `zu*` family's headers, and port the format rather than
the code.** The goal is the fastest general serialization package for R. The language is not
where that speed comes from, but C removes the two things that stand between rdz and it:
the R API boundary and the CRAN toolchain.

### 1.1 Where the time goes, and why the language does not move it

| Cost centre | What bounds it | Rust vs C |
|---|---|---|
| Walking and building R objects | R's C API: `TYPEOF`, `ATTRIB` policy, `allocVector`, `SET_STRING_ELT` | identical; Rust reaches it through FFI |
| Moving bytes | `memcpy` and the compressor; zstd and lz4 are C libraries under any Rust crate | identical |
| Character columns on read | R's global string cache (`mkCharLenCE`), roughly tens of millions of strings per second | identical, until ALTREP deferred strings, which need the C API directly |
| Logical and integer transforms | SIMD classification and packing | equal: target attributes with run-time dispatch in C, `is_x86_feature_detected!` in Rust |
| Parallel block compression | a thread pool and an ordered writer | rayon in Rust; a small pool in C, as `qs2` and `fst` each carry |

### 1.2 What C gives this package

- **Direct R API.** ALTREP classes for lazy reads, `R_Serialize()` through
  `R_outpstream` callbacks into blocks, `DATAPTR_OR_NULL()` on ALTREP inputs, bulk
  `mkCharLenCE()`: each is a plain call in C and hand-written `unsafe` FFI in Rust. The
  Rust version already needed `src/r_bridge.c` for attributes and encodings.
- **The family's building blocks, already built and gated.** zubin supplies the capped
  growable buffer, the checked cursor, typed reads and writes at any byte order, and record
  layouts with vectorised unpack and pack kernels, all header-only through `LinkingTo`.
  zufast supplies XXH3 (one-shot and streaming), f16, UTF-8 validation and endian helpers.
  Six C packages ship with the same CI: sanitizers, valgrind, LTO, gctorture, rchk,
  libFuzzer with canaries, i386 and s390x legs, allocation-failure sweeps.
- **An ordinary CRAN package.** No Cargo, no `vendor.tar.xz`, no offline build dance, no
  Rtools Rust toolchain, faster installs, a smaller tarball, and a lower R floor than the
  current `R (>= 4.5.0)` if the attribute API allows it (§5).

### 1.3 What Rust gives up, honestly

The borrow checker and `rayon`. The first is replaced by the rule the family already
lives by, that every heap object is owned by R before the first call that can longjmp, and
by an R-free core that is fuzzed and run under sanitizers on every push. The second costs a
bounded thread pool of a few hundred lines; CRAN caps checks at two threads in either
language, and the pipeline design in performance.md is unchanged.

### 1.4 Sunk cost

| | |
|---|---|
| Rust written | 5,270 lines, plus the in-flight character work |
| Native codecs complete | 1 of 7 types (logical); character in progress |
| Vendored crates | 13, including `cc`: a C compiler is already a build requirement |
| Still ahead | integer, double, character, factor, list, data frame, compression, the parallel pipeline, the streamed generic bridge, tuning, the freeze |

Most of the work is ahead. The format, the tests' intent, the fixtures and the benchmark
harness carry over.

## 2. What is kept

- **The format.** `container-format.md` as the wire contract, with the amendments in §3.
  Every header and entry already has an explicit width, offset and byte order, which is to
  say every one is a zubin layout.
- **The R API and its semantics.** `write_rdz(x, path, mode = c("auto", "native", "r"))`,
  `read_rdz(path)`, `rdz_info(path)`, `rdz_schema(path)`, `rdz_attributes(path, object,
  names, allow_full)`; the attribute policy, the whole-root fallback rule, the
  no-fragment rule, the external-pointer rule of AGENTS.md. Not one user-visible promise
  changes.
- **The Rust implementation, as the oracle.** Before the port starts, the Rust writer
  generates a fixture corpus: every supported type at lengths zero, one, block size minus
  one, exactly, plus one, and multiblock; every logical record kind; every dictionary
  policy; a generic-codec file for a representative rare object. The C reader must read
  every fixture to an `identical()` value, and the C writer, under the same block policy,
  must produce bytes the Rust reader reads back identically. The Rust tree stays in the
  repository until the last fixture passes, then moves to a `rust-reference` tag and is
  deleted from `main`.
- **The benchmark harness.** `tools/benchmark.R` and `tools/bench-helpers.R` are R code
  and run unchanged; the single-thread default and the checksum-enabled competitor settings
  of validation.md stay the comparison contract.
- **The in-flight character dictionary work** (encodings 8 and 9, uncommitted on
  2026-10-05). Recommendation: finish and land it in Rust first, so the character record is
  frozen by the oracle rather than ported from a diff. It is the one record the Rust side
  has not yet fixed, and the C port of Stage G needs a fixed target.

## 3. Three format decisions to take before the port freezes anything

The format is pre-release. These are free now and expensive after 0.1.0.

1. **Checksum: XXH3-64 in place of IEEE CRC32.** XXH3 is in zufast (`zuf_hash64()`,
   `zuf_hasher_*`), bit-identical to xxHash, faster than any portable CRC32, and what
   `qs2` uses. The four-byte checksum fields of the file header, block header and
   directory header become eight bytes; the container version becomes 3. The alternative,
   keeping CRC32, means asking zufast to add it (its deferred list has CRC32C; IEEE CRC32
   would be a second request) and accepting a slower per-block scan. CRC's guaranteed
   detection of short burst errors is not a property a storage format needs over a 64-bit
   hash. **Recommended: XXH3-64, eight-byte fields, container version 3.**
2. **The generic payload is streamed from day one.** `R_Serialize()` writes through an
   `R_outpstream` whose callback appends to the current block buffer and hands full blocks
   to the pipeline; `R_Unserialize()` reads through an `R_inpstream` that pulls decompressed
   blocks in order. The whole-raw allocation and the separate whole-payload integrity pass,
   which roadmap Phase 8 already requires removed before 0.1.0, never exist in the C
   version. `R_InitOutPStream`, `R_InitInPStream`, `R_Serialize` and `R_Unserialize` are
   not on R 4.6.1's `tools:::nonAPI` list (checked 2026-10-04). This is the item zubin's
   next.md gates on rdz's decision; deciding yes unblocks it there too.
3. **Compression: zstd, vendored.** The block header already carries a compression ID.
   The speed presets of performance.md need zstd's negative and low levels at least; LZ4
   stays evidence-gated as encoding-research.md says. zukomp today registers only the
   DEFLATE family, and a zstd satellite there is not scheduled, so rdz vendors zstd's
   single-file amalgamation under `src/vendor/zstd/` with the family's
   `tools/vendor/{manifest.tsv,fetch,record,verify}` tooling, `inst/COPYRIGHTS`, `cph`
   entries and hidden symbols, as `fst` and `zstdlite` do. If zukomp later ships zstd, rdz
   can switch to its registered table in a minor release without a format change, since the
   block bytes are zstd frames either way. **Recommended: vendor zstd; LZ4 only on
   evidence.**

## 4. Architecture in C

The layers of architecture.md, with the Rust names replaced:

```text
R/                 write_rdz(), read_rdz(), rdz_info(), rdz_schema(), rdz_attributes()   unchanged
src/rdz_r.c        .Call entry points: argument re-validation, conditions, the R-owned pipeline handle
src/adapter/       R thread only: class-first dispatch, traversal, attribute policy, CHARSXP access,
                   the R_Serialize bridge, allocation of decoded R objects
src/core/          R-free: records (zubin layouts), container writer and reader, directory,
                   type codecs, block pipeline, zstd, XXH3, atomic file replacement
src/vendor/zstd/   the amalgamation, byte-identical to the pinned release
```

Rules, each enforced by a gate:

- **The core never includes an R header.** It compiles standalone into the fuzz targets and
  a C test harness (`-DRDZ_STANDALONE`, zucbor's pattern); an `Rinternals.h` error in the
  fuzz build means something leaked. The core's only allocation primitive is zubin's
  `zb_buf`, so "where can this allocate" is one search.
- **Records are layouts.** The 32-byte file header, 40-byte block header, 36-byte directory
  header, 48-byte object entry, 32-byte attribute entry, 56-byte block entry and 32-byte
  trailer are `zb_layout` specification strings, parsed once at load into static
  `zb_field` arrays. Headers are read with `zb_cur_*` (unchanged cursor on any failure,
  so a truncation is reported at its byte) and written into `zb_put_raw()` slots with
  `zb_wr_*`. Directory tables are read with the vectorised `zb_unpack_*` kernels, one pass
  per field over all entries, which is the field-major win zubin exists for.
- **Blocks are `zb_buf`s with a hard cap.** The 64 MiB reader limit and the 1 MiB writer
  block size become `max` on the buffer, enforced where the buffer grows; the
  `ZB_BUF_HIT_LIMIT` flag distinguishes a limit from an allocator failure in the error
  class.
- **Only the R thread touches SEXPs or calls R.** Workers receive owned `zb_buf`s and
  immutable metadata; the ordered writer owns only buffers and the file handle. This is
  performance.md's pipeline unchanged. The pool is pthreads (winpthreads under Rtools),
  not OpenMP: Apple's clang has no OpenMP, and one rdz-owned pool whose workers call
  single-threaded zstd is the nested-parallelism rule performance.md already states. Thread
  count comes from an option, defaults to one until Stage I's tuning, and never exceeds
  what CRAN permits during checks.
- **Heap state that must survive a longjmp is owned by R.** The pipeline handle, every
  block buffer and the open temporary file hang off one external pointer created before the
  first R call, through `zb_r_buf_new()` and a finalizer of the same shape, so an
  `Rf_error()` or an interrupt tears the pipeline down and removes the temporary file.
  `R_CheckUserInterrupt()` runs on the R thread between blocks and nowhere else.
- **Checksums stream.** One `zuf_hasher` per block and one over the directory; no second
  pass over bytes already written.
- **Strings cross the boundary once, as bytes plus encoding.** The adapter extracts
  `CHAR()`, `LENGTH()` and `getCharCE()` into owned buffers before any worker sees them; the
  dictionary hashes CHARSXP addresses in-process and never persists one, exactly as the
  Rust writer does. Reads make CHARSXPs on the R thread. The later ALTREP deferred-string
  read, the one route past the string cache, is zubin 0.3.0's views and is listed under
  roadmap.md's later opportunities, not promised here.
- **SIMD follows zufast's policy.** The scalar classifier is the bit-exact reference; the
  AVX2 and NEON kernels are ported behind `__attribute__((target(...)))` with
  `__builtin_cpu_supports()` dispatch inside the file, because a portable `Makevars`
  cannot give one object `-mavx2`. The s390x leg proves the scalar path and the wire bytes
  on a big-endian host.

Naming, by the family rule: R exports keep their names; C internals `rdz_`; `.Call` entry
points `rdz_`; test-only entry points `rdz_test_`; condition classes `rdz_*` inheriting
`rdz_error`. No public C API and no installed headers: rdz is an end product, like zucbor.

## 5. Dependencies and their state on 2026-10-05

| Package | State | What rdz takes | How |
|---|---|---|---|
| zufast 0.1.0 | tagged; CRAN submission pending | XXH3, f16, UTF-8 validation, endian loads and stores | `LinkingTo` |
| zubin 0.1.0 | implemented (14 R functions, headers `rw.h`, `cursor.h`, `buf.h`, `layout.h`, `zubin-r.h`); release branch waits for zufast on CRAN | buffer, cursor, layouts and kernels, R-owned buffers | `LinkingTo` |
| zstd | vendored by rdz (§3) | per-block compression | `src/vendor/zstd/` |
| zukomp, zucrypt | not needed | — | — |

`DESCRIPTION` becomes: `LinkingTo: zubin, zufast`; `Imports:` nothing beyond `utils`; no
`SystemRequirements`; `Remotes: pedrobtz/zubin@main, pedrobtz/zufast@main` during
development only, removed at submission, as zuxlsx does.

**The R floor.** The Rust version requires R 4.5.0, and its last commit moved attribute
iteration off `ATTRIB()`, which R-devel marks non-API, onto a newer entry point. The C
adapter must iterate attributes the same way, and whichever API that is sets the floor:
4.1 if `getAttrib()` over a known allowlist suffices for the native path, higher if general
iteration needs the newer call. Record the floor with its reason rather than inheriting
4.5 by default.

**Release order.** zufast, then zubin, then rdz. Every stage below can be developed with
the `Remotes` field; only the submission waits.

## 6. Stages

The type order of roadmap.md (logical, integer, double, character, factor, list, data
frame) is unchanged, and so is its completion gate per type. The stages below restructure
the sequence for a port: the container and the generic codec come first, because they
restore full R-object coverage before any native codec exists, and because the generic
codec is the fallback every later stage relies on.

Sizes: **S** a sitting, **M** a few, **L** the stage is the week. A stage is done when its
exit criteria pass in CI on Linux, macOS and Windows, one pull request per stage.

### Stage A — Decide, land the oracle, freeze the targets · S

**Do:** take the three decisions of §3 and record them in container-format.md (version 3)
and encoding-research.md; finish and merge the in-flight character dictionary work in
Rust; generate the fixture corpus of §2 into `tests/testthat/fixtures/rust/` with a
manifest (spec, policy, SHA-256, generating commit); tag the Rust tree
`rust-reference-pre-c`.

**Exit:** the corpus exists and every fixture reads back `identical()` in the Rust build;
the three decisions are written where the format is specified.

### Stage B — The C skeleton and the R-free container · M

**Do:** new `src/` layout of §4 with an explicit `OBJECTS` list and portable make;
`LinkingTo: zubin, zufast`; the record layouts; the container writer (forward pass,
directory, trailer, same-directory temporary file and atomic replacement preserving
permissions) and reader (bounded trailer discovery, directory validation, every reader
limit as a `zb_buf` cap or a checked count); XXH3 checksums; the standalone harness and
`fuzz_container` with a canary; CI from the family: `R-CMD-check`, `native-checks`,
`hardening`, `arch`. The R side gains only `rdz_info()` over the new reader.

**Exit:** `rdz_info()` reports every Rust fixture's container metadata identically to the
Rust `rdz_info()`; the fuzzer ran ten minutes on the reader with no finding; the s390x leg
reads the little-endian fixtures correctly.

### Stage C — The generic codec, streamed · M

**Do:** the `R_outpstream`/`R_inpstream` bridge into and out of blocks, single-threaded;
`mode = "r"` complete; the generic synopsis; `write_rdz()`/`read_rdz()` for every object,
all through the generic codec; the interrupt test and the error-path test (temporary file
removed, external pointer finalized).

**Exit:** the whole universal round-trip matrix of validation.md passes through the generic
codec; peak memory during a write is bounded by block size, measured; forced-generic
benchmarks against `qs2` are recorded.

### Stage D — The pipeline and compression · M

**Do:** the thread pool, bounded queues, sequence numbers, ordered writer and reader,
cooperative cancellation and first-error propagation; vendored zstd with the vendor
tooling, `inst/COPYRIGHTS` and hidden symbols; per-block codec selection with raw fallback
for incompressible blocks; the thread-count option; the small-input bypass.

**Exit:** equal-budget scaling measured on large vectors with no small-vector regression;
ASan clean at one and at eight threads with 4 KiB blocks (the stress shape, since no TSan
leg exists); `tools/vendor/verify` clean; the generic codec compresses.

### Stage E — The logical codec, ported · M

**Do:** the five record kinds, the tri-state classifier scalar first, then AVX2 and NEON
behind dispatch; selective `names` reads and the authoritative schema for logical files.

**Exit:** byte-identical files to the Rust fixtures for all six benchmark distributions;
the write and read results of roadmap Phase 1 reproduced or bettered.

### Stage F — Integer and double · L

**Do:** roadmap Phases 2 and 3 as written: exact `i32` baseline; shuffle, frame-of-reference
plus bit-packing, delta and run-length behind encoding IDs with a cheap selector; exact
`f64` bits with the eight-byte shuffle; ALP gated by encoding-research.md's audit.

**Exit:** the Phase 2 and 3 exit criteria of roadmap.md.

### Stage G — Character and factor · L

**Do:** roadmap Phases 4 and 5: exact bytes-plus-encoding extraction through the API,
encodings 2, 8 and 9 ported against the Stage A fixtures, cross-locale tests, factors.

**Exit:** the Phase 4 and 5 exit criteria; every character fixture reads identically.

### Stage H — Lists and data frames · M

**Do:** roadmap Phases 6 and 7, including the bounded recursion, the whole-root fallback
on an unsupported child, the `data.table` transient-pointer registry, column scheduling,
and the complete `rdz_schema()`/`rdz_attributes()` surface.

**Exit:** the Phase 6 and 7 exit criteria; the full data-frame matrix against `qdata`,
`qs2` and `fst` recorded.

### Stage I — Tuning, fuzzing, the freeze, 0.1.0 · M

**Status:** done 2026-10-05 but the submission, which waits for zufast and zubin on CRAN
(current-state.md, "Checkpoint 2026-10-05: plan-c Stage I").

**Do:** roadmap Phase 8 as written, minus the generic-bridge item already done in Stage C;
cross-OS fixture exchange between CI jobs; the frozen 0.1.0 fixtures; mutation checks on
every reader guard (`/* GUARD */` markers, zucbor's tool); `cran-comments.md`; the Rust tree
removed from `main` once the last fixture passes; submission after zubin is on CRAN.

**Exit:** on CRAN; the 0.1.0 compatibility rule of roadmap.md in force.

### Stage J — Decimal doubles (ALP), before 0.1.0 · S

**Status:** done 2026-10-05 (current-state.md, "Checkpoint 2026-10-05: plan-c Stage J").

**Do:** encoding-research.md's ALP audit as an experiment against qs2, fst and Pcodec;
encoding 23 if it moves the frontier; its guard, fuzzing, frozen fixtures.

**Exit:** decimal doubles smaller and faster than every compared format, the other
distributions unchanged, the corpus extended and not rewritten.

### Stage K — Selective reads · S

**Status:** done 2026-10-06 (current-state.md, "Checkpoint 2026-10-06: plan-c Stage K").

**Do:** `read_rdz(select =)` over the existing directory: a data frame's columns, a list's
elements; nothing else read. **Exit:** one column read faster than fst's `columns =`.

### Stage L — Inspection below the root; tables as written · S

**Status:** done 2026-10-06 (current-state.md, "Checkpoint 2026-10-06: plan-c Stage L").

**Do:** metadata-access.md's `rdz_schema(recursive =)` and `rdz_attributes(object =)` over the
directory; data.tables rebuilt wherever they are read. **Exit:** a part's attributes and the
tree read with that part's data corrupt.

## 7. Gates

- **Differential against the oracle.** Every Rust fixture is read to an `identical()`
  value; under the same policy the C writer's bytes are read back by the Rust reference
  until Stage I retires it. A fixture that cannot be reproduced is a format question, not a
  test to skip.
- **Family gates on every push.** Sanitizers, valgrind, LTO, gctorture (quick on PRs, full
  on `main`), blocking rchk, libFuzzer targets for the container reader, each codec and the
  generic bridge, i386 and s390x, the allocation-failure sweep once the adapter exists. A
  gate counts once it has been seen to fail.
- **The benchmark matrix** of validation.md, unchanged, single-threaded by default, run and
  recorded at every stage that changes a hot path; the equal-budget scaling matrix from
  Stage D.
- **Portability for real.** The s390x leg does not merely compile: it reads the
  little-endian fixtures and writes files the x86-64 leg reads.

## 8. Risks

| Risk | Handling |
|---|---|
| Memory-safety bugs in C | R-owned heap rule; R-free core fuzzed with sanitizers; valgrind and rchk blocking; the reader guards mutation-checked |
| Threads on Windows and without TSan | pthreads through Rtools' winpthreads, as other CRAN packages do; stress runs under ASan with tiny blocks and many threads; one-thread path first-class and deterministic |
| Vendored zstd | about 1.3 MB of amalgamated C, the `fst`/`zstdlite` precedent; manifest, checksums, patches-as-files, hidden symbols |
| CRAN timing of zufast and zubin | develop on `Remotes`; the submission is the only step that waits |
| SIMD kernels in C | scalar reference first, bit-exact; target attributes with run-time dispatch; the arch legs |
| Losing Rust's type system for codec state machines | the format is small and fully specified; the fixture corpus and the standalone harness are the net |
| Discarding 5,270 lines | one of seven types; the format, the tests' intent and the harness carry over; the Rust tree stays as the oracle until it is not needed |

## 9. What this changes in the other documents, and when

| Document | Change | When |
|---|---|---|
| `container-format.md` | version 3: eight-byte XXH3-64 checksum fields; the streamed generic payload; zstd as compression ID 1 | Stage A |
| `encoding-research.md` | the checksum and compression decisions recorded with their evidence | Stage A |
| `architecture.md` | the module layout and the Savvy boundary sections replaced by §4; the C shim section retired | Stage B |
| `roadmap.md` | Phases 2–8 mapped to Stages F–I; a pointer to this file at the top | Stage B |
| `validation.md` | layer 1 becomes the R-free C harness and fuzz targets; layer 2 retired | Stage B |
| `AGENTS.md`, `CLAUDE.md` | "implemented in Rust", the Savvy-generated-file rule and the Cargo commands replaced | Stage B |
| `DESCRIPTION` | `SystemRequirements` removed; `LinkingTo`; the R floor with its reason | Stage B |
| `current-state.md` | a dated checkpoint at Stage A and at each stage close | each stage |
| `portability.md`, `metadata-access.md`, `sexp-coverage.md`, `performance.md`, `research.md` | unchanged | — |

## 10. Decisions

Taken on 2026-10-05. The first is the maintainer's; the rest follow this plan's
recommendations, which the maintainer's instruction to proceed on the plan adopted, and any of
them can be reopened before Stage B freezes the format.

- [x] Re-implement in C on zubin and zufast, as §1 recommends. **Decided by the maintainer.**
- [x] Checksum: XXH3-64 with eight-byte fields and container version 3 (recommended).
- [x] Compression: vendored zstd (recommended); LZ4 only on evidence.
- [x] Land the in-flight character dictionary work in Rust before Stage A (recommended):
      pedrobtz/rdz#3.
- [x] The R floor, decided by the attribute-iteration API rather than inherited: taken at
      Stage B, where the C adapter's attribute iteration is written, and recorded there with
      its reason.
