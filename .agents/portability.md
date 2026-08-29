# RDZ Cross-OS Portability

## Portability contract

An `.rdz` file written on a supported Windows, macOS, or Linux system must be
readable on every other supported system, subject to:

- the target R version understanding the recorded payload codec/version;
- required packages/classes being available when base R serialization needs
  them;
- base R's documented limitations for external pointers, weak references,
  connections, namespaces, native code, and other process-local resources;
- sufficient address space and R vector-length support on the reader.

The contract applies to both payload codecs:

- `NATIVE_V1` uses a platform-independent RDZ wire representation;
- `R_SERIAL_V3` uses R serialization version 3 in XDR format, never native-word
  order.

There is no host-native RDZ file preset. A speed option may change compression or
filters, but it must not create a file tied to CPU endianness, C/Rust struct
layout, pointer width, operating system, or locale.

Cross-OS readable does not mean every live resource becomes portable. An external
pointer may round-trip according to base R's serialization/refhook behavior while
the address, file descriptor, DLL handle, device, or process it referred to
cannot exist in another process or OS.

## Canonical scalar representation

All native format fields have an explicitly documented width and byte order:

| Logical value | Wire representation |
|---|---|
| Type/encoding/codec tags | Explicit unsigned integer width, normally `u8` or `u16` |
| Flags | Explicit little-endian `u16`/`u32`; unknown mandatory bits are errors |
| Counts, lengths, and offsets | Little-endian `u64` unless a smaller width is part of a documented sub-encoding |
| R integer values | Exact little-endian `i32`, including `NA_INTEGER` |
| R double values | `f64::to_bits()` as little-endian `u64` |
| Future complex values | Two exact little-endian `u64` bit patterns, real then imaginary |
| Checksums | Algorithm-defined bytes with a fixed RDZ byte order |

Compact bit encodings define bit order, padding, and unused-bit validation. The
implemented logical mapping stores elements from the low bits upward with
`00=FALSE`, `01=TRUE`, `10=NA`, rejects `11`, and requires unused high bits in
the final byte to be zero.

Never persist:

- `usize`, `isize`, `R_xlen_t`, `long`, `size_t`, or pointer values;
- Rust/C enum memory representations;
- Rust `bool` or native C `_Bool` layout;
- an in-memory struct via `transmute`, raw struct copy, or implicit padding;
- platform-native integer or floating-point byte order;
- an address as object identity.

Object IDs are explicit little-endian `u32` values assigned by the encoder, not
SEXP addresses. Future reference IDs must likewise use a documented fixed width.
SEXP addresses may be temporary hash keys during one serialization call but
never appear in a file.

On read, convert file `u64` lengths to `R_xlen_t`/`usize` with checked bounds.
Reject an object that the target cannot represent rather than truncating or
wrapping its length.

## Container and block representation

The header, block headers, footer, and indexes use canonical little-endian fields.
The implemented widths, offsets, magic values, checksums, and reader limits are
specified in [container-format.md](container-format.md).
The metadata directory and fixed closing trailer follow the same rules; metadata
inspection must produce equivalent logical results across supported operating
systems without decoding host-specific layouts.
The file magic and codec/version IDs have the same bytes on every OS. Producer OS,
endianness, pointer width, and R version may be recorded for diagnostics but must
not change native decoding rules.

Each block records its actual transform, compression codec, encoded size, decoded
size, and integrity metadata. A decoder must not infer these from the producing
library or host. Blocks compressed by a standardized codec such as
[Zstandard](https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md)
must be decodable independently of the implementation version that produced them.

Compression output need not be byte-identical across library versions unless RDZ
explicitly promises deterministic writing. Compatibility means that later readers
decode committed fixtures correctly. Checksums cover the exact stored bytes and
use the checksum algorithm/version recorded by the format.

Files are always opened in binary mode. Windows text translation must never be
able to alter newline or control bytes.

## Portable generic R serialization

The generic whole-root codec must initialize `R_Serialize()` with
`R_pstream_xdr_format` and serialization version 3. Do not use
`R_pstream_binary_format`, even if native-word-order serialization benchmarks
faster on the writer. R's public [`serialize()` documentation](https://stat.ethz.ch/R-manual/R-devel/library/base/html/serialize.html)
distinguishes portable XDR from native binary order and documents the associated
speed tradeoff.

The XDR bytes are then framed and compressed by the platform-independent RDZ
block/container layer. The reader validates the RDZ container, decompresses the
stream, and feeds it to `R_Unserialize()` on the R thread.

R serialization version 3 records the producer's native character encoding and
adds ALTREP serialization support. Store the R stream version in the payload
metadata rather than assuming the current R default. If a future R serialization
version is adopted, assign a new RDZ generic codec version and retain old read
fixtures.

## Character portability

The native character codec defines RDZ-owned encoding tags; it must not persist
the numeric representation of R's `cetype_t` enum. Each non-missing string stores
enough information to distinguish:

- ASCII;
- UTF-8;
- Latin-1;
- bytes (`CE_BYTES` semantics);
- text in the producer's native encoding.

UTF-8, Latin-1, and byte strings retain their explicit bytes and encoding
semantics across OSes. Byte strings are never transcoded.

For a native-encoded string, store the producer's canonical native-encoding name
in payload metadata. On read:

1. If the target native encoding is compatible, restore the native bytes/tag.
2. Otherwise transcode from the recorded source encoding to UTF-8 and construct a
   UTF-8 `CHARSXP`.
3. If the source encoding is unknown, unsupported, or the bytes are invalid,
   return a clear error; never reinterpret them using the target locale.

This follows the semantic goal of R serialization version 3: preserve the text
across different native encodings even when the target cannot preserve the
original unmarked/native byte representation. Same-locale tests require exact
bytes and tags; cross-locale tests require identical Unicode text and exact
preservation for explicitly tagged UTF-8, Latin-1, ASCII, and bytes.

If the native character encoder cannot identify a source encoding reliably, the
automatic writer must select whole-root XDR R serialization rather than emitting
an ambiguous native RDZ character record.

## Object and class portability

Class names, attribute names, factor levels, column names, and row names use the
portable character rules. No class is treated as native merely because its
underlying storage is portable.

Automatic mode selects whole-root R serialization for unsupported classes,
environments, closures, byte code, external pointers, weak references, and other
rare graph types. This preserves base R's cross-platform behavior and caveats but
cannot guarantee that:

- a package or namespace exists on the reader;
- native code compiled for another OS exists or has the same ABI;
- external resources can be reconstructed;
- an unsupported third-party ALTREP class is installed;
- closures depending on OS-specific paths, commands, or DLLs behave identically.

RDZ guarantees faithful transport of the serialized representation, not the
availability of external dependencies.

The native adapter may omit an explicitly registered runtime-only
external-pointer attribute, initially `data.table::.internal.selfref`. Such an
attribute has no portable pointer value: the address belongs to the writer
process. Omission is preferable to persisting an address or null placeholder, but
is allowed only for a known owning class with semantic behavior tests. All other
external pointers select whole-root R fallback and retain base R's limitations.

## Compression and SIMD portability

Every transform and compressor needs:

- a portable scalar implementation or dependency fallback;
- runtime SIMD detection rather than compile-host assumptions;
- identical decoded values for scalar, SSE, AVX, NEON, and other paths;
- explicit behavior on CPUs without an accelerated checksum instruction;
- no requirement that the reader support the writer's instruction set.

SIMD choices are implementation details and are never persisted as required
reader capabilities. Codec IDs describe the byte algorithm, not the CPU path used
to produce it.

Any dependency with native code must be built and tested on Windows, macOS, and
Linux and reviewed under the CRAN native-library rules in `AGENTS.md`.

## Filesystem behavior

Write a temporary file in the destination directory, close it, preserve the mode
of an existing destination, and use the safest available same-filesystem
replacement operation. The ordinary mode is visibility-atomic when the platform
can rename over an existing target, but it is not crash-durable because it does
not sync the file or parent directory. The backup-and-rollback fallback used on
platforms without replace-by-rename has a brief missing-destination window and
can strand a recoverable backup after a crash. These guarantees are filesystem
semantics, not part of the portable wire representation.

Tests must cover:

- binary open modes on Windows;
- destination replacement when the target exists;
- cleanup after serialization, compression, seek, close, and rename
  failures where they can be injected;
- preservation of existing destination permissions where the OS exposes modes;
- Unicode and non-ASCII file paths through R's path APIs;
- files larger than 2 GiB where CI or dedicated release infrastructure permits.

File paths themselves are API inputs and are not stored as part of an ordinary
serialized object's format metadata.

## Cross-platform validation matrix

Run package tests on at least:

| Producer/reader environment | Required validation |
|---|---|
| Windows x86_64, current supported R | Read committed native and generic fixtures; write artifacts for cross-job reads |
| macOS arm64 and/or x86_64 | Read the same fixtures and verify exact native semantics |
| Linux x86_64 | Read the same fixtures; run sanitizer/fuzz jobs where available |
| Big-endian synthetic decoder tests | Decode hand-constructed canonical bytes and byte-swapped invalid controls |
| Different supported R minor versions | Read stable native fixtures and supported R-stream fixtures |
| UTF-8 and non-UTF-8/C locales | Exercise native-encoding conversion and byte-string invariants |

Committed fixtures must include:

- every native scalar/vector representation;
- multiblock values and footer/index offsets;
- positive/negative integers and boundary lengths;
- NA and unusual floating-point bit patterns;
- UTF-8, Latin-1, bytes, and recorded native-encoding examples;
- a generic XDR R stream containing closures/environments/shared references;
- compressed and raw blocks.

Pre-0.1.0 fixtures are replaceable test assets and create no backward-
compatibility obligation. The fixtures shipped with 0.1.0 become the permanent
compatibility baseline.

At least one CI workflow should exchange an `.rdz` artifact produced on one OS
and read it on another, rather than merely reading a fixture committed from one
machine. Exact output-byte comparison is required for uncompressed canonical
records. For compressed output, require successful decoding and exact R values
unless deterministic compressed bytes are an explicit codec promise.

## Release gate

An on-disk codec version is not stable until:

- all multibyte fields and bit layouts are documented;
- no host-layout writes remain;
- XDR is enforced for the generic codec;
- character conversion behavior is implemented and tested;
- committed fixtures read on Windows, macOS, and Linux;
- overflow behavior is tested independently of host pointer width;
- dependency SIMD/scalar paths agree;
- external-resource and package-availability caveats are documented.

A portability failure in a released format is a compatibility bug, not an
optional performance issue.

## Related documents

- [Architecture](architecture.md)
- [R SEXP coverage matrix](sexp-coverage.md)
- [Implementation roadmap](roadmap.md)
- [Validation and benchmarking](validation.md)
- [Performance design](performance.md)
- [Metadata and selective attribute access](metadata-access.md)
- [Encoding and format research](encoding-research.md)
