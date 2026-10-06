# RDZ Metadata and Attribute Access

## Purpose

RDZ must support inspecting an object and reading selected R attributes without
materializing the complete object or decoding unrelated vector payloads. This is
a first-class file-format requirement, not merely a future API optimization.

The feature has four goals:

1. inspect the RDZ container, codec, integrity, and size information cheaply;
2. inspect the root type and recursive schema of native objects;
3. read selected native attribute values through independently addressable blocks;
4. report honestly when exact inspection is unavailable for a whole-root R
   serialization fallback.

Metadata access must preserve the attribute rules in
[architecture.md](architecture.md#attribute-policy). It is not permission to
duplicate arbitrary R-serialized fragments, strip attributes, or weaken graph
semantics.

## Three metadata layers

Use precise terms in code, documentation, and APIs:

| Layer | Examples | Requires full object read? |
|---|---|---|
| Container metadata | magic, versions, codec ID, compressed/uncompressed bytes, block counts, checksums | No |
| Object descriptors | root type, vector lengths, dimensions, class summary, list/data-frame shape, column names and types, attribute directory | No for native files |
| Attribute values | `class`, `levels`, `names`, `dim`, `dimnames`, `row.names`, custom attribute values | No when the native directory points to independently addressable attribute records; otherwise yes |

An attribute such as `names` can itself be large. “Without a full read” means the
reader touches only the directory and the requested attribute blocks; it does not
promise that a requested large attribute is free to read.

## Public API direction

The names are provisional, but keep distinct operations rather than overloading a
full reader with ambiguous flags:

```r
rdz_info(path)
rdz_schema(path, recursive = TRUE)
rdz_attributes(path, object = NULL, names = NULL, allow_full = FALSE)
```

`rdz_info()` returns bounded container information plus the root synopsis.
`rdz_schema()` returns native object descriptors without allocating leaf vectors.
`rdz_attributes()` reads all or selected attributes for the root or a stable
object identifier/path. `read_rdz(select =)` (plan-c Stage K) reuses the same
directory for column projection: a data frame's columns or a list's elements,
by name or position, reading only their blocks.

Every result must say:

- the payload codec and codec version;
- whether the result is authoritative or only a synopsis;
- whether exact attributes are independently readable;
- whether satisfying the request would require full deserialization;
- which integrity checks were performed.

With `allow_full = FALSE`, a request that cannot be answered selectively returns a
clear condition rather than silently reading the entire object. With
`allow_full = TRUE`, the implementation may deserialize the complete root and
then return the requested information; the caller asked for that, so the
result does not say so again (`rdz_attributes()` returns the attributes as a
plain list either way).

## Native file layout

Reserve a fixed-size closing trailer that can be found by seeking from the end of
the file. It points to a checksummed metadata directory:

```text
[fixed header]
[native object and leaf-data blocks]
[independently addressable attribute blocks]
[object/attribute/block directory]
[directory offset | directory length | directory checksum | closing magic]
```

This preserves single-pass bulk writing: offsets are collected while payloads are
written, then the directory and trailer are appended. A non-seekable encoder can
still produce the same stream. Selective metadata reads require a seekable source
or a range-readable object store.

The native directory contains bounded, versioned descriptors with at least:

- stable object ID and parent/role information;
- native type tag and representation flags;
- logical length and shape information;
- child object IDs for lists and data frames;
- attribute name, order, and attribute-value object ID;
- block offsets, stored lengths, decoded lengths, codec/filter IDs, and checksums;
- class and common tabular summaries needed by `rdz_info()`;
- explicit flags for registered transient metadata omitted by policy.

Object IDs are file-local identifiers, not memory addresses. Paths presented to R
users are derived views; IDs remain authoritative when names are absent or
duplicated.

Attribute values use ordinary native object records and the same reference table
as their owner. The directory merely indexes those records. It must not contain a
second independently serialized copy of an attribute value, because duplication
could lose sharing or cycles. The native format released in 0.1.0 may expose only
the explicitly supported attribute subset; an object with any other attribute
uses whole-root fallback as already required.

Keep structural descriptors small. Large strings, names, factor levels,
`dimnames`, row names, and custom values belong in separately addressable blocks,
not inline in an unbounded footer.

## Generic R-serialization fallback

An `R_SERIAL_V3` payload is one sequential R serialization graph. R does not
provide a supported API for extracting arbitrary attributes without
unserializing that graph, and RDZ must not create separately R-serialized
attribute islands merely to enable inspection.

The RDZ container may therefore store a bounded root synopsis produced during the
capability pass or generic write. It may include:

- root `SEXPTYPE` and class names;
- common length/dimension summaries;
- data-frame row/column counts and bounded column-type summaries;
- attribute names, subject to explicit count/byte limits;
- source R serialization version and native encoding;
- a flag that exact attribute values require full deserialization.

This synopsis is informative and checksummed but not an independently decodable
replacement for the R object graph. `rdz_schema()` and `rdz_attributes()` must
mark it as non-authoritative. Exact generic attributes require `allow_full = TRUE`
or a normal full read.

The Phase 0B synopsis has a stable success/failure schema. Bounded strings are
cut only at valid encoded-character boundaries, and `truncated` is derived from
the actual bounded result rather than a separate heuristic. `synopsis_error` is
always present: `FALSE` for a complete bounded synopsis and `TRUE` for the
minimal safe synopsis used when optional collection fails.

Do not execute `refhook`, package hooks, active bindings, or arbitrary R code while
performing metadata-only inspection. If obtaining information would require such
behavior, report it as unavailable without a full read.

## R-thread and Rust boundaries

Pure Rust code owns header, trailer, directory, range-read, checksum, and descriptor
validation. It can return a bounded neutral metadata model without R.

The R adapter converts that neutral model into R lists/data frames on the R thread.
Reading an exact attribute value allocates only that selected R value and any
objects it references. Savvy SEXP wrappers and R API calls never move to worker
threads.

Suggested Rust modules are:

```text
container/metadata.rs
container/directory.rs
container/range_reader.rs
r_adapter/metadata.rs
```

## Validation and security

Treat metadata as untrusted before allocating or seeking. Validate:

- directory offset and length against file size;
- every object, child, attribute, and block reference;
- integer conversions and offset-plus-length arithmetic;
- maximum descriptor, attribute, nesting, string, and total metadata sizes;
- non-overlapping spans where required by the format;
- directory and selected-block checksums;
- codec/version compatibility before interpreting descriptors.

A corrupt synopsis or directory is an error, not a reason to fall back to full R
deserialization. Metadata inspection must never panic, abort R, or allocate based
on an unchecked file value.

## Performance contract

Not implemented as instrumentation: rdz does not count the bytes or blocks a
read touches. What is tested instead is the property that matters, by
corrupting the data a read must not need: `test-inspect.R` and `test-rows.R`
corrupt a vector's data block and show that attribute reads, the schema and
row windows elsewhere still succeed while a full read fails. Counting bytes
and blocks (for benchmarks, and for range requests on remote files) is a
candidate for a later version. The benchmarks below are the intended set:

- container information only;
- root schema only;
- data-frame schema and column types;
- `class` or `dim` only;
- a small custom attribute;
- a large `names` or `levels` attribute;
- the equivalent full-object read.

For a large native vector, reading `class` or `dim` must not read or decompress its
data blocks. For a wide native data frame, schema inspection must not allocate
columns. Performance tests should include cold-cache local files and, when a
range-reader exists, count remote range requests.

## Compatibility and portability

Directory fields follow [portability.md](portability.md): explicit fixed widths,
canonical little-endian order, checked offsets, portable strings, and no host
addresses or struct layouts. Unknown optional descriptor fields can be skipped;
unknown required fields or directory versions are errors.

Golden fixtures must freeze header/trailer discovery and directory semantics.
Windows, macOS, and Linux CI must produce the same logical metadata results and
exchange metadata-indexed files. A byte-format change to an authoritative
directory requires a compatible reader or a new directory/codec version.

## Completion gate

Phase 1 satisfies the logical-specific portions of this gate: native logical
schema inspection does not touch data blocks, and `names` can be read from its
own checked blocks. Data-frame descriptors, cross-OS fixtures, and broader
format-stability work remain open.

The native file format is not stable until:

- the fixed trailer and metadata-directory version are defined;
- native logical-vector metadata can be inspected without reading its data;
- selected supported attributes can be read independently;
- data-frame descriptors expose row/column counts and column schemas;
- generic fallback reports synopsis-versus-exact capability honestly;
- malformed metadata and resource-limit tests pass;
- selective-read benchmarks prove that unrelated data blocks were untouched.

## Related documents

- [Architecture](architecture.md)
- [Implementation roadmap](roadmap.md)
- [Performance design](performance.md)
- [Validation and benchmarking](validation.md)
- [R SEXP coverage](sexp-coverage.md)
- [Cross-OS portability](portability.md)
- [Encoding and format research](encoding-research.md)
