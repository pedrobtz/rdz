# Serialization Research: `qs2` and `fst`

## Scope and source versions

This review covers `qs2` 0.2.3 at commit
[`fda2888`](https://github.com/qsbase/qs2/tree/fda2888f4568963429f7879ed6f981171b597e3f)
and `fst` 0.9.9 at commit
[`b356063`](https://github.com/fstpackage/fst/tree/b35606332483699aef44b6edbcf38de68bd7fa9e).
`qs2` pins its format/IO implementation to `qdata-cpp` commit
[`c09efd9`](https://github.com/qsbase/qdata-cpp/tree/c09efd9bc22f761791aa9b7ada0fe2688a5471a7).
The `fst` R package is mostly validation and Rcpp wrappers; the actual format is
implemented by its separately installed `fstcore` dependency. The implementation
details below were checked against `fstcore` 0.10.0 at commit
[`bea65eb`](https://github.com/fstpackage/fstcore/tree/bea65ebdf1f74be42a1cf1c422d3aae79a97b982).
Because `fst` does not pin a minimum or exact `fstcore` version, details may vary
with the resolved dependency.

## Summary

| Format | Logical model | Main compression | Random access | R-object coverage |
|---|---|---|---|---|
| `qs2` | R serialization stream | Zstandard, optional byte shuffle | No | Broad: whatever R serialization can represent |
| `qdata` | Custom typed object tree | Same block layer as `qs2` | No | Atomic vectors, lists, and supported attributes only |
| `fst` | Typed, indexed column store | LZ4/Zstandard with type filters | Columns and row ranges | Data frames with a fixed set of column types only |

`qs2` and `qdata` share an IO/compression envelope but produce different
uncompressed byte streams. `fst` solves a different problem: it sacrifices
general object support to make tabular projection and slicing cheap.

## `qs2`: R serialization inside a faster block stream

### Object encoding

`qs2` initializes an `R_outpstream` in binary mode at R serialization version 3
and calls `R_Serialize()`. Its output callbacks feed bytes directly to the block
writer; reading supplies decompressed bytes to `R_Unserialize()`. Thus the package
does not reimplement R's graph, type, attribute, class, or reference encoding and
does not need to construct a complete uncompressed serialization in memory.

This is why a `qs2` file can be converted to RDS: decompressing and concatenating
its blocks yields a standard R binary serialization stream; the reverse operation
chunks an uncompressed RDS stream into the `qs2` envelope.

### File and block layer

Both `qs2` and `qdata` start with a 24-byte header containing a four-byte format
magic, format version, Zstandard codec flag, producer endianness, shuffle flag,
reserved bytes, and an eight-byte checksum at offset 16. The reader rejects a
different endianness.

The uncompressed stream is divided into blocks of at most 1 MiB. Each block is
stored as:

```text
uint32 compressed_size_and_flags
Zstandard frame bytes
```

The high bit of the size word records whether the block was byte-shuffled. The
remaining 31 bits give the compressed byte count. A streaming XXH3-64 checksum is
calculated over every block size word and compressed block, then written into the
file header. A zero digest is remapped to one because zero denotes a missing
checksum. Reads may validate the checksum before deserialization, or compute and
compare it while reading.

When shuffle is enabled, a learned heuristic decides per block whether a Blosc
eight-byte shuffle should precede Zstandard. Blocks smaller than 256 KiB skip the
heuristic. For larger blocks it samples four 32 KiB regions, compresses shuffled
and unshuffled samples at Zstandard level -1, and passes the eight sample sizes
plus the requested level to an embedded XGBoost model. At levels 14 and above,
an accepted candidate is fully compressed both ways and the smaller result wins;
at lower levels the selected shuffled path is used directly. Bytes left after
the last complete eight-byte element are copied unchanged.

With multiple threads, a TBB flow graph compresses or decompresses blocks in
parallel. A sequencer restores block order before a serial writer hashes and
writes them. Buffer pools and a direct-memory path reduce allocation and copies.

### Does `qs2` cover all R objects?

The `qs2` format has essentially the same broad type coverage as base R
serialization, including ordinary atomic vectors, pairlists, language objects,
closures, environments, S3/S4 objects, attributes, and nested structures. It is
the appropriate comparison target for a general-purpose `rdz` serializer.

It does **not** make every live R value portable. The normal R serialization
caveats still apply: an external-pointer object can be encoded, but its underlying
C address or resource cannot be reconstructed automatically; resources such as
connections or external process state may therefore be unusable after a round
trip. Reference sharing follows R's own rules for environments, external pointers,
and weak references. `qs2` supplies no custom persistence hook, and its
native-endian envelope is not cross-endian. So “all R objects” is accurate for
ordinary serializable R values, not for arbitrary native runtime state.

## `qdata`: custom, restricted R object encoding

`qdata` bypasses `R_Serialize()` for speed and compression. It recognizes only
`NULL`, logical, integer, double, complex, character, list, and raw SEXPs.
Unsupported values—including closures, environments, external pointers,
pairlists/language objects, promises, and S4 SEXPs—are encoded as `NULL` (with an
optional warning). Unsupported attribute values are omitted. Data frames,
matrices, factors, Dates, and many S3 values work because they are supported
vectors/lists plus supported attributes.

The serializer first emits a recursive structure/attribute tree. Small vector
lengths below 32 are packed with the type into one byte; larger lengths select
8-, 16-, 32-, or 64-bit length records. Attribute counts use 5-, 8-, or 32-bit
forms. Each string uses a one-byte length below 253, marker 253 plus a 16-bit
length, marker 254 plus a 32-bit length, or marker 255 for `NA`.

Leaf payloads are not written in traversal order. They are queued and then
written in groups: character, complex, double, integer/logical, then raw. Grouping
similar representations improves compression. Numeric and raw vectors are copied
from R memory at widths 16, 8, 4, and 1 byte respectively. Latin-1 strings, and
non-ASCII native strings outside a UTF-8 locale, are translated to UTF-8; the
reader constructs strings marked UTF-8.

The decoder allocates the entire structure first, queues the leaf destinations,
and fills the same grouped payload sections afterward. The format has no reference
table, so it cannot preserve shared identity or cyclic structures. It uses the
same 24-byte header, 1 MiB Zstandard blocks, adaptive shuffle, checksum, and TBB
pipeline as `qs2`.

## `fst`: indexed, type-aware column serialization

### Table layout and access model

`fst` accepts a data frame rather than an arbitrary R object graph. It writes a
little-endian table header followed by optional data.table key indexes, a chunkset
header, encoded column names, a chunk index/data header, and each column's data.
Metadata records column type, semantic attribute, scale, row count, and absolute
column positions. XXH64 hashes protect table and index metadata; ordinary column
payload blocks do not have a general end-to-end checksum.

Every column has its own block index. A 64-bit index entry packs the block offset
in the low 48 bits and codec/filter identifier in the high 16 bits. Column
positions make column selection direct, while the block index lets a row-range
read seek to and decode only intersecting blocks. Numeric blocks are normally
about 16 KiB: 4,096 integers/logicals, 2,048 doubles/int64 values, or 16,384 raw
bytes. Character blocks contain 2,047 elements.

OpenMP workers compress blocks in parallel, while ordered sections serialize file
writes. Reads batch the selected compressed blocks, decompress them in parallel,
and copy only the requested portions of the first and last blocks. Uncompressed
reads can seek directly into fixed-width columns.

### Compression policy

The public compression setting ranges from 0 to 100 and controls a deterministic
per-block mixture, not just a single codec level:

- At 0, fixed-width data is stored raw, except representations such as logicals
  and small factors still use compact packing.
- From 1 through 50, an increasing fraction (`2 * setting` percent) of blocks uses
  fast LZ4 and the rest stays raw or only packed.
- From 51 through 100, an increasing fraction (`2 * (setting - 50)` percent) uses
  Zstandard and the rest uses LZ4. Each block records the algorithm actually used.

The preprocessing filter is selected by column type:

- Integers use four-byte shuffle before LZ4/Zstandard; int64 uses eight-byte
  shuffle. Current doubles use plain LZ4/Zstandard.
- Logicals are first bit-packed at 32 values per 64-bit word (two bits per value,
  preserving `FALSE`, `TRUE`, and `NA`), then optionally compressed.
- Factors store levels as a character vector. Codes use one byte below 128 levels,
  two shuffled bytes below 32,768 levels, otherwise the integer path.
- Character blocks store 32-bit string lengths, an NA bitmap, and concatenated
  bytes. Lengths use shuffled integer compression; bytes use plain LZ4/Zstandard;
  the NA bitmap is uncompressed.
- Raw/byte columns use plain LZ4/Zstandard.

The supported R columns are character, factor/ordered, integer, double, logical,
raw, and selected classed representations: `Date`, `POSIXct`, `difftime`, `ITime`,
`integer64`, and `nanotime`. Time zones and other needed annotations, factor
levels, units/scales, and data.table keys receive explicit format fields. Arbitrary
attributes and unsupported column/list types are not a general extension point.

## Implications for `rdz`

For complete R-object fidelity, the reusable `qs2` idea is to keep R's serializer
as the semantic layer and optimize only framing, compression, checksums, and IO.
`qdata` demonstrates the performance benefit—and compatibility cost—of separating
a compact structure tree from homogeneous payloads. `fst` contributes the best
ideas for a tabular specialization: per-column metadata, small independently
indexed blocks, type-specific transforms, mixed codecs, and selective reads.

These strategies should remain distinct in `rdz`: a general format needs explicit
tests for every R `SEXPTYPE` and R's native-resource caveats, while an optional
tabular fast path can use `fst`-style indexes without defining the semantics of the
general object format.
