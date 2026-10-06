# RDZ Architecture

## Status

This document describes rdz as implemented at 0.1.0: a C implementation on
zubin and zufast (adopted 2026-10-05, [plan-c.md](plan-c.md)), which replaced
the Rust implementation and its Savvy bindings. Everything below is true of
the code; a statement that stops being true is a bug in this file or in the
code. The earlier, Rust-era version of this document is in the git history
(before the 2026-10-06 rewrite) and is not a description of rdz.

The contracts it refers to:

- [container-format.md](container-format.md): the wire format, frozen as of
  rdz 0.1.0 (its "Compatibility and extension" rules);
- `?rdz` ("Compatibility"), `?write_rdz` ("Native and generic", "Replacing
  files"), `?rdz-errors`: what users are promised;
- [metadata-access.md](metadata-access.md): inspection without the data;
- `AGENTS.md`: the coding rules and the gates every change passes.

## Layers

```text
R/                 the API: arguments, options, row and column subsetting,
                   schema trees, archives, skip_unchanged, condition classes
src/rdz_r.c        .Call entry points: open, info, read, verify, write
src/adapter/       the R boundary: which R objects are native and how they
                   are planned, hashed and rebuilt; the generic codec's
                   R_Serialize/R_Unserialize streams
src/core/          R-free C99: records, container reader and writer, codecs,
                   pipeline, file IO, content hash
src/vendor/zstd/   Zstandard 1.5.7, compiled with no exported symbols
```

Two rules hold the layers apart, and are gated:

1. **The core never includes an R header.** It compiles standalone into the C
   harness (`tools/c-tests/`), the mutation probe and the fuzz targets
   (`fuzz/`), none of which link R.
2. **Only the R thread touches a SEXP or calls R.** Worker threads compress,
   decompress, checksum and decode blocks into memory the R thread owns; they
   never allocate R objects. ThreadSanitizer runs the core harness in CI.

### The core (`src/core/`)

| Module | Role |
|---|---|
| `rdz_format.h`, `rdz_records.{h,c}` | Wire constants and the seven fixed records, as zubin layouts read and written at their offsets, never through structs |
| `rdz_error.c` | The error type: a status (format, limit, version, codec, io, memory, unsupported) and a message |
| `rdz_io.{h,c}` | Files with 64-bit offsets; the output file (temporary sibling renamed over the destination, or memory) |
| `rdz_container.{h,c}` | The reader (validation of header, directory, blocks, object graph, references, metadata) and the writer |
| `rdz_codec.{h,c}` | Per-block zstd, stored only when smaller |
| `rdz_pipeline.{h,c}` | A bounded ring of block slots served by worker threads, consumed in order |
| `rdz_logical.c`, `rdz_numeric.c`, `rdz_alp.c`, `rdz_string.c` | The block encodings: logical 1, 3–7; integer 10–14; double 20–23; character 2, 8, 9 |
| `rdz_vector.{h,c}` | One vector through the pipeline, written or read |
| `rdz_native.{h,c}` | The logical-root file the Rust reference wrote |
| `rdz_graph.{h,c}` | Native object graphs, written and read without recursion; selections, windows and block plans |
| `rdz_content.{h,c}` | The content hash accumulator (XXH3-128 over a canonical stream) |

Every count, length and offset that comes from a file is bounded before it
sizes an allocation or a read; each of the marked reader guards is shown
load-bearing by `tools/run-mutation-check`. Allocation that grows with a
file goes through zubin's `zb_buf`, and its size arithmetic through
`zb_size_add`/`zb_size_mul`; fixed bookkeeping (pipeline slots, an output
file's name) uses `malloc`/`calloc` directly.

### The adapter (`src/adapter/`)

`rdz_native_r.c` is where R semantics meet the format. It owns:

- **Eligibility and planning.** A plan walks the value once, iteratively,
  and either lists its nodes (objects with types, lengths, attribute entries
  and data pointers) or names the first reason it cannot be native. The
  reasons are the list in `?write_rdz` ("Native and generic").
- **The writer's policies**, none of them part of the format:
  - *small parts*: in automatic mode, at least 1,024 parts averaging less
    than 1 KiB of data go generic;
  - *sharing*: a vector of 4 KiB or more met twice (the same SEXP) is
    written once and referenced (type 8);
  - *strings*: an unmarked non-ASCII string is stored with the UTF-8 tag
    when `Rf_translateCharUTF8()` gives valid UTF-8 that converts back to
    the same bytes, and sends its vector generic otherwise (the C locale);
  - *row names*: automatic ones (`c(NA, -n)`) are implied; a positive
    compact pair (`c(NA, n)`, as from `head()`) is written as `1:n`, which R
    makes compact again;
  - *ALTREP*: a compact sequence or deferred string is materialised and
    written as ordinary data (so `1:3e7` costs its 120 MB while writing).
- **Hashing** (`rdz_hash()`): the plan, fed to the same accumulator the
  writer feeds, with one string-digest cache per hash.
- **Reading back**: allocation of every wanted object in directory order,
  blocks decoded into them, attributes set in reverse (so a data frame's
  class comes last), references resolved to their targets. A data.table gets
  a NULL external pointer as `.internal.selfref`, which data.table itself
  treats as "read from disk" and repairs; rdz never loads data.table.
- **Selections and windows**: masks of the objects a `select` or an
  attribute read needs, and per-object row windows for `rows`. A window is
  refused (and R reads the value whole) when a windowed vector is shared
  with a part read whole, or carries `dim`, `dimnames` or `tsp`.

`rdz_generic.c` streams R serialization (version 3) through the same blocks:
`R_Serialize` writes into 1 MiB blocks as it goes, under `R_UnwindProtect`,
so an error or interrupt removes the temporary file. The generic content
hash is R serialization version 2 without its header, as the digest package
computes it.

### The R layer (`R/`)

R validates arguments (raising `rdz_argument_error`), reads options into
the settings the C layer takes, and does what is simplest over R values:

- `read_rdz(rows =)`: the C reader returns the window covering the rows;
  R takes the rows as `[` does (`rdz_rows_take()`), and reads whole any
  value the reader refuses to window, and any generic file;
- `read_rdz(select =)` on a generic file: the value is read whole and
  subset;
- `rdz_schema()` builds its tree from the directory dump;
- `rdz_attributes(object =)`, `rdz_save()`/`rdz_load()`, `skip_unchanged`.

A `select` and a `rows` read each open the file more than once (the
directory for the selection, then the read); one C entry point taking
`select`, `rows` and `object` would remove that, and is a candidate for a
later version.

## Writing

```text
write_rdz(x, path)
  R: arguments, metadata section, settings
  C: plan x  -- not native (auto) --> generic: R_Serialize -> blocks
       | native
  for each node in plan order (R thread): encode its blocks, feeding the
  content hash; each block -> pipeline slot -> worker: zstd, XXH3-64
  -> blocks written in order -> directory, synopsis, metadata, trailer
  -> rename over path (or the bytes, for rdz_serialize())
```

The bytes depend only on the value, the preset and the block size, never on
the number of threads (tested).

## Reading

```text
read_rdz(path, select, rows)
  open: header, trailer, directory: checksums and every bound, before any
        block is read
  native: allocate the wanted objects; for each wanted block: read, verify
          XXH3-64, decompress (workers), decode into its object (or into a
          window's scratch, then copy the rows); set attributes; resolve
          references
  generic: R_Unserialize from a stream that verifies and decompresses each
           block as R reads it
```

`rdz_info()` reads only the header, directory and trailer; a generic file's
synopsis is unserialized only after a scan shows it holds nothing but plain
vectors. `rdz_verify()` checks every block's checksum without decoding, and
with `content = TRUE` the value against the stored hash.

## Errors

C never raises an rdz error below the outermost `.Call`. A failure returns an
`rdz_failure` string whose `kind` attribute R maps to a condition class
(`rdz_check()`); R's own checks raise `rdz_argument_error` or
`rdz_io_error`. Every class inherits `rdz_error` (`?rdz-errors`). An R error
raised inside R_Serialize or R_Unserialize (an object whose serialization
fails) reaches the caller as it is.

Heap state that must outlive a longjmp hangs off an external pointer created
before the first allocation, with a finalizer that releases it.

## Decisions recorded here

These were open, or contradicted, in the Rust-era documents; the code is as
stated.

- **No refhook.** Neither the generic writer nor the reader takes one;
  environments and external pointers go through R serialization as R
  handles them.
- **Strings in the session encoding** are stored as UTF-8 when the
  conversion is lossless (above), and the reader never transcodes: a string
  comes back marked as stored (UTF-8, Latin-1, bytes or ASCII).
- **data.table selfref**: a NULL external pointer, not omission (above).
- **Named factors** are written generically.
- **Encoding 1** (the Rust reference's dense two-bit logical record) is
  read, never written; its decoder stays for the files that reference wrote.
- **Presets** differ only in the zstd level (`speed`: none; `balanced`: 1;
  `compact`: 6). The decimal double encoding (23) is chosen only for a block
  that will be compressed, so `speed` never writes it.
- **Pre-0.1.0 gates.** AArch64 byte-equivalence is met: the frozen corpus's
  speed files are rewritten byte for byte on macOS arm64 in CI, and the C
  harness compares the NEON and scalar logical kernels. The logical
  "all-write" performance criterion against fst (long-run and alternating
  input) is waived for 0.1.0: it concerns speed, not the format.

## Limits, and what the tests do and do not cover

- **Selection is one level deep.** `select` names parts of the root (a
  list's elements or a data frame's columns), and `rows` applies to the
  root; neither reaches below it. `rdz_attributes(object =)` does.
- **Graphs are trees with references.** A native graph has no cycles: a
  reference points to an earlier object that is not its ancestor, and R
  values that can form cycles (environments) are generic, where R
  serialization keeps them. Tested: a self-referencing environment reads
  back as one.
- **Sizes.** Lengths and offsets are 64-bit throughout, so files over
  2 GiB and long vectors are within the format. A container's parts are
  `u32`-counted, and the writer refuses more than 1,000,000 parts. Neither
  a file over 2 GiB nor a long vector is exercised by the tests (they need
  that much disk and memory); `tools/benchmark.R` and manual runs cover the
  large cases.
- **Types.** Tested beyond the native types: time series (native, `tsp` as
  an attribute), unforced promises, environment cycles, weak references
  (as R serializes them: without key or value), S7 and S4 objects (generic).
- **Hostile files.** Every marked reader guard is load-bearing (mutation
  check); every flipped byte and every prefix of two Rust-era files and
  nine frozen ones is rejected (the C harness); libFuzzer runs on every
  push and nightly. Its inputs are at most 64 KiB, so it never builds a
  1 MiB block or a dictionary over several blocks; the frozen corpus and
  the R tests cover those.
- **Not tested:** I/O failures (a full disk, a failed write or rename) are
  handled by checking every call, but no test injects them; there is no
  performance-regression gate in CI (`tools/benchmark.R` is run by hand);
  reading in another locale is tested with a C-locale child process, not
  on CRAN or Windows.

## Compatibility

The format is frozen as of rdz 0.1.0 ([container-format.md](container-format.md),
"Compatibility and extension"): new encodings, kinds and fields are added
under new identifiers, and a version number changes only with a changed
meaning. The corpus in `tests/testthat/fixtures/v0.1.0/` was written by rdz
0.1.0 and only grows (`tools/check-frozen-corpus.R`, `corpus.yaml`).
