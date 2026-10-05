# RDZ Architecture

## Status and scope

This document describes the target architecture for RDZ. It is a design guide,
not a description of every feature in the current code. The current container,
which wraps an R serialization version-3 payload, is experimental and carries no
backward-compatibility promise. Replace it freely while the format remains
unreleased. The format shipped by `rdz` 0.1.0 will be the first compatibility
baseline and will include an explicitly identified native fast payload and an
R-serialization fallback.

See [current-state.md](current-state.md) for the dated implementation inventory
and [roadmap.md](roadmap.md) for the sequence from the transitional container to
this target.

Phase 0B's implemented pre-release framing contract is specified separately in
[container-format.md](container-format.md). It reserves codec, object, attribute,
block, encoding, and compression IDs without prematurely freezing the later
type-specific record encodings.

The public RDZ API aims to cover every object that base R can serialize. Coverage
does not require every R type to receive a dedicated native encoding. Common
vectors and their compositions use the native fast path; rare or semantically
complex objects use one whole-root R serialization stream inside the RDZ
container.

The first native format supports these R values:

- `NULL`
- logical, integer, double, and character vectors
- factors and ordered factors
- lists
- data frames composed from supported columns

The intended handling of every R `SEXPTYPE`, including internal and pseudo-types,
is normative in [sexp-coverage.md](sexp-coverage.md).

Dedicated native support for environments, closures, promises, external pointers,
weak references, pairlists, language objects, S4/S7 objects, unsupported ALTREP
classes, matrices, arrays, and arbitrary attributes is not an initial goal. These
values select the generic R-serialization codec in automatic mode. Strict native
mode must fail clearly rather than silently converting or stripping them.

## Design goals

1. Cover every object supported by base R serialization through either the
   native fast path or the whole-root generic fallback.
2. Beat general-purpose R serialization on natively supported values and compete
   with `qs2`/`qdata` and full-data-frame `fst` reads and writes.
3. Preserve R values exactly, including typed missing values, floating-point bit
   patterns, character encodings, names, factor levels, and data-frame row names,
   except for explicitly registered transient metadata that has no serialized
   value semantics.
4. Keep peak temporary memory bounded by block size and worker count rather than
   total object size.
5. Keep format, codec, compression, checksum, and IO code independent of R.
6. Keep all R API access on the R thread and confine unsafe FFI to a small module.
7. Make format evolution explicit and retain deterministic compatibility tests.
8. Make every released `.rdz` codec readable across supported Windows, macOS,
   Linux, CPU endianness, pointer widths, and locales under the contract in
   [portability.md](portability.md).

Metadata/schema inspection and selective reading of supported attributes are
first-version requirements under [metadata-access.md](metadata-access.md). Random
row access, arbitrary data-frame column projection, and zero-copy ALTREP decoding
remain possible future extensions.

## Layered design

```text
R wrapper
   |
   | one .Call (src/rdz_r.c)
   v
thin entry point: argument re-validation, conditions, R-owned handles
   |
   v
R adapter (R thread only)
   |- class-aware dispatch
   |- SEXP traversal and validation
   |- character and attribute access
   |- allocation of decoded R objects
   |
   v
payload selection
   |- native typed codec for the supported fast subset
   `- one base-R serialization stream for the whole root
   |
   v
block/compression pipeline
   |- vector and composite records
   |- block transforms and compression
   |- structural validation
   |
   v
RDZ container and IO
   |- magic, versions, codec ID
   |- block/footer index
   |- checksums
   |- temporary file and safest platform replacement
```

The implementation is C99 on zubin and zufast (plan-c.md section 4, adopted
2026-10-05); the Rust implementation in `src/rust/` is the oracle until the port
retires it. The module layout:

```text
R/                 write_rdz(), read_rdz(), rdz_info(), rdz_schema(), rdz_attributes()
src/rdz_r.c        .Call entry points: argument re-validation, failures as rdz_failure,
                   core state owned by external pointers
src/rdz_init.c     routine registration (C, and the Rust oracle when built)
src/adapter/       R thread only: dispatch, traversal, attribute policy, CHARSXP
                   access, the R_Serialize bridge, allocation of decoded objects
                   (from Stage C)
src/core/          R-free: rdz_format.h (wire constants, errors), rdz_records
                   (zubin layouts), rdz_container (reader, writer), rdz_io (files,
                   atomic replacement); type codecs and the block pipeline later
src/vendor/zstd/   the amalgamation (from Stage D)
```

The dependency direction is the rule: `src/core/` never includes an R header and
compiles standalone into `tools/c-tests/` and `fuzz/`; its only allocation
primitive is zubin's `zb_buf`.

## R, C, and Rust responsibilities

### R

R wrappers provide user-friendly argument validation, paths, presets, codec mode,
and error messages. They call a single native entry point per operation. The
public API may eventually offer both direct file operations and low-level
raw-vector operations, but file IO is the normal high-performance path.

The intended codec modes are:

- `auto`: use the native codec when the complete root is supported, otherwise
  serialize the complete root with base R;
- `native`: require the native codec and report an unsupported-value error;
- `r`: force the generic base-R serialization codec.

`auto` is the coverage-oriented public default. `native` is essential for codec
tests, native benchmarks, and users who do not want implicit fallback.
Supplying an R serialization `refhook`, if supported by the public API, forces the
generic whole-root codec; strict native mode with a `refhook` is an argument error.

### The .Call boundary

`src/rdz_r.c` holds thin entry points. They translate the path (UTF-8 on
Windows, native elsewhere), create an external pointer before the core allocates
anything, call the core, and build the R result. C never raises an rdz error: a
failure returns an `rdz_failure` string with its kind, which `rdz_check()` in
`R/c-core.R` raises as `rdz_format_error`, `rdz_limit_error`,
`rdz_version_error`, `rdz_codec_error`, `rdz_io_error` or `rdz_memory_error`, each
inheriting `rdz_error`. Do not put format logic in these functions.

### R adapter (R thread only)

The adapter owns semantic dispatch and recursive traversal. Only the R thread
touches SEXPs or calls R; workers receive owned `zb_buf`s and immutable metadata.
Strings cross the boundary once, as bytes plus encoding. Every helper uses the
public R API and never inspects the internal `SEXPREC` layout. The generic codec
streams through `R_Serialize()`/`R_Unserialize()` callbacks into and out of
blocks (container-format.md).

During decoding, the R thread allocates the destination vector and decodes into
it. A future ALTREP path may change selected copies without changing the codec.

### The Rust oracle

Until each C stage replaces it, `src/rust/` (Savvy 0.10.2) still writes and
reads, and its corpus in `tests/testthat/fixtures/rust/` fixes the bytes the C
implementation must read and reproduce. Savvy's generated `src/init.c`,
`src/rust/api.h` and `R/000-wrappers.R` are frozen; `src/rdz_init.c` registers
their routines when configure builds the Rust library and error stubs when it
does not.

## Type dispatch

Dispatch classed values before their underlying storage type:

```text
inherits(x, "data.frame")  -> data frame
inherits(x, "factor")      -> factor or ordered factor
otherwise TYPEOF(x):
  NILSXP                    -> NULL
  LGLSXP                    -> logical
  INTSXP                    -> integer
  REALSXP                   -> double
  STRSXP                    -> character
  VECSXP                    -> list
  anything else             -> unsupported-native signal
```

This ordering is required because factors are integer vectors and data frames are
lists. Classed values other than the supported classes produce the same
unsupported-native signal until their semantics are explicitly implemented.
`SEXPTYPE` is only the storage dispatch: class, attributes, S4/object state, and
ALTREP eligibility are checked first as specified by the
[coverage matrix](sexp-coverage.md#class-attribute-and-representation-overlays).

## Codec selection and whole-root fallback

RDZ files identify their payload codec independently from the outer container.
At minimum, the container distinguishes:

```text
NATIVE_V1       dedicated RDZ typed/vector codec
R_SERIAL_V3     one base-R serialization version-3 XDR stream
```

The experimental allocation-based container is not a compatibility target. The
generic writer must initialize `R_Serialize()` with `R_pstream_xdr_format` and
stream its callbacks directly through the same bounded block/compression writer
used by native blocks, avoiding a complete raw-vector allocation. Generic reads
reverse the pipeline and feed decompressed bytes to `R_Unserialize()` on the R
thread.

Fallback is for the complete root object, not individual unsupported children.
For example, a list containing a numeric vector, a closure, and an environment is
written as one R serialization stream. Independently serializing only the closure
or environment would break sharing and cycles crossing the native/fallback
boundary. [Base R documents](https://stat.ethz.ch/R-manual/R-devel/library/base/html/serialize.html)
that reference sharing is preserved within one serialization call but not across
separate calls.

Automatic encoding may either run a capability pass before writing or
optimistically attempt native encoding into the temporary file. If native
encoding returns the distinguished unsupported-native outcome, discard that
temporary output and serialize the original root through the generic codec. Only
that outcome triggers fallback: IO errors, allocation failures, internal format
errors, and invalid arguments remain errors.

The boundary uses the optimistic shape: one native call owns both eligibility
and writing. Phase 1 implements the supported logical branch inside that call;
unsupported roots return before creating a file. Do not reintroduce a
selection-only call followed by a second native traversal.

Generic coverage means the coverage and limitations of base R serialization. It
does not make external resources portable: external pointers, weak references,
connections, namespace references, and similar runtime state retain R's normal
serialization caveats and optional `refhook` behavior. Store the R serialization
format version and source native encoding metadata required by R version 3.

An external pointer is not automatically permission to fall back or to drop it.
The adapter classifies the location and owning object using the policy under
[External-pointer metadata](#external-pointer-metadata). Unknown or semantic
external pointers select whole-root fallback; only registered transient metadata
may be omitted from an otherwise native root.

Environments may reuse recursive traversal and native leaf encoders in a future
graph codec, but they must not reuse the list record itself. Their bindings,
identity, enclosure, special binding state, and cycles require the distinct
decoder lifecycle described in
[sexp-coverage.md](sexp-coverage.md#environments-are-graph-containers-not-lists).

## Native payload model

The native payload is self-describing and recursively typed. The initial tag set
reserves entries for:

```text
NULL, LOGICAL, INTEGER, DOUBLE, CHARACTER, FACTOR, LIST, DATA_FRAME
```

Every vector record contains its logical element count, supported metadata, and
one or more independently decodable blocks. Composite records contain their
element count and recursively encoded children. Child records are self-delimiting
so encoding a list does not require buffering the complete list merely to know
its byte length.

Use fixed little-endian widths for the first implementation unless benchmarks
show a meaningful reason for added variable-length complexity. Reserve tag and
flag space for future encodings and back-references.

## Type semantics

### Logical

Preserve `FALSE`, `TRUE`, and `NA_LOGICAL` as distinct states. Never use ordinary
Savvy logical iteration, which does not preserve `NA` correctly; access the raw
integer representation. The implemented classifier validates raw R `i32` values
and produces separate TRUE and NA bitplanes with runtime-dispatched scalar,
AVX2, and NEON kernels. It accumulates exact counts and transitions in the same
pass, then selects constant, modal-default dense planes, sparse `u16` patches,
run ends, or a bounded short-period record for each 65,536-value block.

The old mapping `00=FALSE`, `01=TRUE`, `10=NA`, low bits first, remains the
two-bit decoder/reference representation; `11` and nonzero padding are invalid.
Dense AVX2 expansion and the scalar decoder produce the same R values.
Host-specific kernels affect execution only and must emit identical canonical
bytes. Optional names use independently addressable exact-encoding character
records; nonportable native-encoded names select whole-root fallback. See
[encoding research](encoding-research.md#logical-vectors).

### Integer

The baseline representation is little-endian `i32`, preserving `NA_INTEGER`.
Block metadata identifies optional transforms such as bit packing, delta, or run
length encoding.

### Double

Encode values through their IEEE-754 `u64` bit representation. Do not normalize
NaNs: R's missing double, ordinary NaNs, infinities, and negative zero must round
trip bit-for-bit.

### Character

Each distinct string representation includes an NA marker, byte length, raw
bytes, and R encoding tag. Exact character support requires public R character
APIs rather than Savvy's UTF-8 `&str` convenience layer. Record the source native
encoding in file metadata because native-encoded bytes are locale-dependent.

Cross-locale reconstruction follows [portability.md](portability.md): preserve
explicit UTF-8, Latin-1, ASCII, and byte strings; for incompatible native
encodings, transcode from the recorded source encoding to UTF-8 rather than
interpreting bytes in the reader's locale.

A block may use offsets plus concatenated bytes or a dictionary plus indices.
The decoded result must preserve per-element encodings, not merely Unicode text.

### Factor

Store integer codes, levels as a character vector, and the ordered/class state.
Validate that every code is missing or in `1..=length(levels)`.

### List

Store optional names and recursive child records. Enforce a configurable nesting
limit. Reserve a reference/back-reference mechanism before declaring the format
stable; either implement supported sharing/cycles or reject them explicitly.

### Data frame

Store row count, column count, column names, the exact supported `row.names`
representation, and recursively typed columns. Validate equal column lengths and
preserve zero-row and zero-column data frames.

An explicitly supported `data.table` overlay may use this same physical codec.
This does not coerce it to a base `data.frame`: preserve the
`c("data.table", "data.frame")` class vector, columns, names, row names, and
keys/index metadata when those attributes have defined native semantics. Omit its
registered transient `.internal.selfref` attribute; do not serialize the pointer
address or treat it as graph data. The decoder returns a `data.table`-classed
object without a persisted process address. `data.table` may reconstruct the
self-reference; an eager reconstruction hook can be added on the R thread if
benchmarks and compatibility tests justify it. Tests must check class, table
content, supported metadata, and usable restored behavior rather than
`expect_identical()` on the complete attribute set.

## Attribute policy

Attributes are part of an R object's value. The adapter must enumerate and
classify every attribute before native encoding; finding one that cannot be
represented is the distinguished unsupported-native outcome, never permission to
drop it. Automatic mode then writes the unchanged complete root with
`R_SERIAL_V3`, while strict native mode fails clearly.

Native version 1 directly supports only attributes whose semantics are defined:

- vectors and lists: optional `names`
- factors: `levels`, `class`, and optional `names`
- data frames: `names`, `row.names`, and `class`

All names, values, order, and representation details of those attributes must
round-trip. Class-defining attributes, custom user attributes, `dim`, `dimnames`,
time-series metadata, units, time zones, and package-specific attributes are not
less important merely because the first native codec does not understand them.
They select whole-root fallback unless an explicit native class/attribute contract
covers them.

General tagged attribute maps can be added in a later native codec version after
graph/reference semantics are defined. Attribute values are arbitrary R objects
and can contain sharing, reference objects, unsupported types, or cycles, so a
general map must use the same object/reference table as the containing object; it
must not create separately serialized attribute islands.

The sole omission mechanism is the narrow transient-metadata registry below.
Each entry is a documented semantic exception, not a weakening of the general
attribute-preservation rule.

## External-pointer metadata

RDZ distinguishes external resources from runtime-only cache or self-reference
metadata:

1. An `EXTPTRSXP` used as the root value, a list element, an environment binding,
   or another object's semantic state is unsupported by the native codec.
   Automatic mode writes the complete root with `R_SERIAL_V3`; strict native mode
   returns the documented unsupported-native error.
2. An external-pointer attribute may be ignored only through a registry entry
   keyed by the owning class and exact attribute name. The entry must explain why
   the attribute is non-semantic, how the owning package reconstructs or tolerates
   its absence, and which behavior tests prove that omission is safe.
3. The initial registry contains `data.table::.internal.selfref`. The native
   adapter omits it and continues through the data-frame codec without removing
   the `data.table` class. This exception does not match other attributes or
   external pointers nested below it.
4. Omission is a deliberate semantic normalization and must be visible in format
   documentation and test expectations. The reader never restores a process
   address from the file and does not fabricate an opaque pointer.
5. If the owner class/version is not recognized, the registry predicate fails,
   or any other unsupported attribute is present, automatic mode falls back for
   the whole root instead of partially stripping the object.

Keep this registry in the R adapter, not the ordinary Rust codec. It depends on R
class and attribute semantics; the portable codec should receive only the
validated metadata that is actually written.

## Payload codec and file container

Keep the sequential payload encoder generic over `std::io::Write` and the decoder
generic over `std::io::Read`. Raw-vector helpers can use `Vec<u8>`/`Cursor`, while
normal file operations use buffered files.

The outer file container is responsible for magic, container version, payload
codec ID/version, endianness declaration where needed, checksums, and any footer
index. The index is a required checksummed object/attribute/block directory for
native codecs, not merely an optional optimization. File-only finalization may
require `Write + Seek`, but the payload codec must not require seeking.

All native container and payload fields use documented fixed widths and canonical
little-endian bytes. The generic R stream uses version-3 XDR, not R's native
binary format. Host layout and platform-specific fast-file modes are forbidden;
the complete invariants and release gate are in
[portability.md](portability.md).

For single-pass writing, container version 2 should prefer final metadata
in a footer:

```text
[magic | container version | codec ID | codec version | flags]
[codec payload and independently addressable attribute blocks]
[object/attribute/block directory]
[directory offset | directory length | payload length | checksums | closing magic]
```

If the existing front-loaded length and checksum are retained, the file writer
must write placeholders and seek back. That is acceptable for files but must not
leak into the codec API.

The reader locates the fixed trailer from end-of-file, validates the directory,
and can inspect native object descriptors or fetch selected attribute blocks
without decoding unrelated leaf data. The generic R-serialization codec exposes
only a bounded synopsis unless the complete root is unserialized. The complete
capability and API rules are in [metadata-access.md](metadata-access.md).

## File safety

Write to a uniquely named temporary file in the destination directory. Complete
the payload and footer, close it, preserve existing destination permissions, and
replace the destination only after successful finalization. Rename-over-target
is visibility-atomic but not crash-durable without an explicit sync mode. The
portable backup-and-rollback fallback is not atomic across its two renames;
document that recovery limitation and clean up the temporary file on every
recoverable failure.

The decoder must treat input as untrusted. It must use checked arithmetic and
enforce limits for allocation size, vector length, block size, recursion depth,
attribute count, and total decoded bytes. Checksums detect corruption but do not
replace structural validation and are not authentication.

## Compatibility policy

Before the `rdz` 0.1.0 release, files are disposable development artifacts: byte
layouts may change at any time, old files may be rejected, and no compatibility
reader or legacy fixture is required. Development fixtures may be regenerated or
removed in the same change as a format revision. Do not spend performance or
design budget preserving any pre-0.1.0 container.

The native payload and the streamed R-serialization payload receive distinct
codec identities. The latter records the embedded R serialization version and is
decoded by R, so it follows base R's documented format compatibility and resource
caveats. Beginning with the format released by `rdz` 0.1.0:

- the 0.1.0 golden files become permanent compatibility fixtures;
- existing tags and meanings are immutable;
- new optional behavior uses reserved flags or new self-delimiting records;
- incompatible changes require a new codec version.

Before freezing a physical encoding or block metadata, consult the candidate
register in [encoding-research.md](encoding-research.md). Borrow leaf encodings
from established formats only after their R semantics, decode cost, portability,
and end-to-end benchmark behavior satisfy its adoption gates.

## Related documents

- [Performance design](performance.md)
- [Implementation roadmap](roadmap.md)
- [Validation and benchmarking](validation.md)
- [R SEXP coverage matrix](sexp-coverage.md)
- [Cross-OS portability](portability.md)
- [Metadata and selective attribute access](metadata-access.md)
- [Encoding and format research](encoding-research.md)
- [`qs2`, `qdata`, and `fst` research](research.md)
