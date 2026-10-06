# RDZ Pre-0.1 Container Format

## Status

This document is the wire contract implemented by Phase 0B. It is intentionally
pre-release: files may still be replaced until `rdz` 0.1.0. The current
container version is 3 (2026-10-05, plan-c.md §3): every checksum is an
eight-byte XXH3-64 with seed 0, bit-identical to the reference xxHash and to
zufast's `zuf_hash64()`, in place of version 2's four-byte IEEE CRC32. Readers
reject version 2 files; no version 2 file was ever released. All multibyte integers are unsigned little-endian values;
there is no implicit padding and every reserved field must be zero. Since
plan-c Stage I the format has the extension points of "Compatibility and
extension" below, decided before the 0.1.0 freeze.

The container and directory layouts are shared by the generic `R_SERIAL_V3`
codec and `NATIVE_V1`. Phase 1 populates native object records for logical
vectors and their optional `names` attribute. Other roots continue through the
whole-root generic codec.

## Identifiers and limits

| Kind | ID/version | Meaning |
|---|---:|---|
| Container | 3 | Seekable block container with XXH3-64 checksums |
| Directory | 1 | Object, attribute, block, and synopsis directory |
| Payload codec | 1/3 | One whole-root R serialization version-3 XDR stream |
| Payload codec | 2/1 | RDZ native typed codec; logical vectors implemented |
| Block encoding | 0 | Raw stored bytes |
| Block encoding | 1 | Legacy two-bit logical reference |
| Block encoding | 2 | Length-prefixed character records |
| Block encoding | 3 | Constant logical block |
| Block encoding | 4 | Modal-default logical bitplanes |
| Block encoding | 5 | Sparse logical patches |
| Block encoding | 6 | Logical run ends |
| Block encoding | 7 | Short periodic logical pattern |
| Block encoding | 8 | Character dictionary entries (experimental) |
| Block encoding | 9 | Character dictionary indices (experimental) |
| Block encoding | 10 | Integer raw (`i32` LE) |
| Block encoding | 11 | Integer shuffled raw (4 byte planes) |
| Block encoding | 12 | Integer frame of reference, bit-packed |
| Block encoding | 13 | Integer delta, bit-packed |
| Block encoding | 14 | Integer runs |
| Block encoding | 20 | Double raw (`f64` bits LE) |
| Block encoding | 21 | Double shuffled raw (8 byte planes) |
| Block encoding | 22 | Double runs |
| Block encoding | 23 | Double decimals (ALP), since plan-c Stage J |
| Compression | 0 | No compression |
| Compression | 1 | One Zstandard frame (since plan-c Stage D) |
| Checksum | implicit v3 | XXH3-64, seed 0, stored little-endian |

The writer uses 1 MiB blocks. Readers accept at most 64 MiB per block (the hard
cap; the file header records the file's own maximum), 1,000,000 blocks, and a
64 KiB generic synopsis. Counts, lengths, offsets, additions, and host-size
conversions are checked before allocation or seeking.

Native type tags are reserved as `0=NULL`, `1=logical`, `2=integer`, `3=double`,
`4=character`, `5=factor`, `6=list`, `7=data frame`, and `8=reference` (since
plan-c Stage O). A tag is not usable
until its codec phase defines its logical and physical record representation.
Unknown IDs, versions, mandatory flags, or nonzero reserved fields are errors.

## Compatibility and extension

Decided in plan-c Stage I, before the 0.1.0 freeze, so that later writers can
add to the format without locking 0.1.0 readers out of every new file:

- **Versions bump only for changed meaning.** Every block encoding, compression,
  type tag, role, attribute kind and flag bit is versioned by its own ID: a
  reader rejects one it does not know. A writer adds a new one without
  bumping the container, directory or codec version, so a new writer's file
  that uses only 0.1.0 features stays readable by 0.1.0 readers. A version
  bumps only when the meaning of an existing field or ID changes.
- **Flags have two halves.** In every flags word the low half (bits 0 to 15 of
  a `u32`, 0 to 7 of a `u16`) must be understood: a reader rejects an unknown
  bit there. The high half may be ignored: readers drop bits they do not know.
  All flags written today are zero but the defined bits named below. A block
  header's `u16` flags repeat the low 16 bits of its directory entry's flags.
- **Directory records may grow.** The directory header's length and each entry
  width may exceed the widths below (up to 256 bytes each); readers read the
  fields they know at their offsets and skip the rest. Fields are only ever
  appended.
- **Block sizes are the writer's policy.** A block holds from 1 value to its
  type's maximum (65,536 logicals, which the `u16` sparse positions require;
  262,144 integers; 131,072 doubles); only an empty object has one block of 0.
  Today's writer fills every block but an object's last.
- **The writer is recorded** (file header bytes 20 to 23, below), so readers can
  work around a writer's bugs in files already written.
- **Fixtures.** The frozen 0.1.0 fixtures are read-compatibility tests: every
  later reader reads them to the same values. Byte equality is required only
  of uncompressed (`speed` preset) native files: zstd's output may change
  between its versions, and a generic payload's R serialization header records
  the R version that wrote it.

## File sequence

```text
[32-byte file header]
[48-byte block header | stored block bytes] ...
[40-byte directory header]
[48-byte object entries] ...
[32-byte attribute entries] ...
[64-byte block entries] ...
[bounded generic synopsis]
[40-byte closing trailer]
```

The writer collects checked block offsets during a forward pass, appends the
directory, and finishes with the trailer. The trailer is discovered with one
bounded seek from end-of-file. It points to an independently checksummed
directory; metadata inspection never scans or allocates the data payload.

## File header: 32 bytes

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Magic bytes `52 44 5a 1a` |
| 4 | 2 | Container version, currently 3 |
| 6 | 2 | Header length, 32 |
| 8 | 4 | Flags, two halves (see "Compatibility and extension"); zero today |
| 12 | 2 | Payload codec ID |
| 14 | 2 | Payload codec version |
| 16 | 4 | Maximum decoded block size |
| 20 | 4 | Writer: implementation `u8`, then the major, minor and patch version `u8`s |
| 24 | 8 | XXH3-64 of bytes 0 through 23 |

The writer field is informational and never rejected: implementation `0` means
not recorded (the Rust reference and earlier files), `1` this package; bit 7 of
the implementation byte marks a development build, whose version is the release
it follows (0.1.0.9000 is written as 0.1.0 with bit 7). `rdz_info()` reports it
as `writer`, for example `"rdz 0.1.0"`.

## Block header: 48 bytes

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Magic `RBLK` |
| 4 | 2 | Header length, 48 |
| 6 | 2 | Flags: the low 16 bits of the block entry's flags |
| 8 | 4 | Zero-based consecutive sequence number |
| 12 | 2 | Encoding ID |
| 14 | 2 | Compression ID |
| 16 | 8 | Logical unit count; bytes for the generic raw stream |
| 24 | 8 | Decoded byte length |
| 32 | 4 | Stored byte length |
| 36 | 4 | Reserved zero |
| 40 | 8 | XXH3-64 of the exact stored bytes |

Generic (raw-encoded) blocks require logical count and decoded length to agree.
An uncompressed block's stored and decoded lengths agree; for a native block the
logical count records the number of R elements those codec bytes represent.

Compression 1 stores the block's decoded bytes as exactly one Zstandard frame
(RFC 8878): no dictionary, no skippable or legacy frames, an optional content
size that must equal the decoded length when present, and an optional content
checksum that readers verify (it covers the decoded bytes, which the block
checksum does not). The frame fills the stored bytes and decompresses to exactly
the decoded length,
and the checksum covers the stored (compressed) bytes, so a reader verifies a
block before it decompresses it. A block is compressed only when that makes it
smaller, and readers reject a compressed block whose stored length is not less
than its decoded length; an incompressible block is therefore always raw. The
writer's presets choose the level (`rdz.preset`: level 1 by default, 6 for
`"compact"`, no compression for `"speed"`); the level is not recorded and no
reader needs it. Neither the preset nor the thread count changes a reader's
work, and the same object written with any thread count gives the same bytes.
Every decoded block length must be less than or equal to the maximum recorded in
the file header; readers reject a contradictory header even when its checksum
is valid.
An empty object stream is represented by one zero-length block. Packed logical
elements are numbered from the least-significant unused bits of each byte;
unused high bits in the final byte must be zero.

## Directory header: 40 bytes

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Magic `RDIR` |
| 4 | 2 | Directory version, 1 |
| 6 | 2 | Directory header length, at least 40 |
| 8 | 2 | Object entry width, at least 48 |
| 10 | 2 | Attribute entry width, at least 32 |
| 12 | 2 | Block entry width, at least 64 |
| 14 | 2 | Flags, two halves (see "Compatibility and extension"); zero today |
| 16 | 4 | Object entry count |
| 20 | 4 | Attribute entry count |
| 24 | 4 | Block entry count |
| 28 | 4 | Synopsis byte length |
| 32 | 8 | XXH3-64 of bytes 0 through 31 |

Generic files set object and attribute counts to zero. Native logical files have
one root object and either no attributes or one `names` attribute represented by
an attribute-name object and an attribute-value object.

The 48-byte object entry contains, in order: `object_id:u32`, `parent_id:u32`,
`role:u16`, `type_tag:u16`, `flags:u32`, `logical_length:u64`, then six `u32`
fields for child start/count, attribute start/count, and block start/count.
Root `parent_id` is `0xffffffff`. The 32-byte attribute entry contains
`owner_id:u32`, `name_object_id:u32`, `value_object_id:u32`, `ordinal:u32`,
`flags:u32`, one reserved `u32`, and one reserved `u64`. Rust models and
byte-round-trip tests enforce these layouts. Roles are `0=root`,
`1=attribute-name`, and `2=attribute-value`; the only Phase 1 attribute flag is
bit 0 for `names`. All other role, object, and attribute flags are rejected.

### Shared objects (since plan-c Stage O)

An object of type 8 is a reference: it stands for the earlier object its
`first_child` names, and reads back as that same R object, shared (the
memo of pickle, the references of Kryo). It has no length, children,
attributes or blocks. Its target comes before it and is not itself a
reference, an attribute name, or one of its ancestors; checks of a
container's children and of attribute values look through it to the
target's type and length. Writers share a vector met twice in one value
when its data is at least 4 KiB (1,024 logicals or integers, 512 doubles or
strings): smaller ones are not worth it, and R's byte compiler shares small
literal constants, which would make a value's representation depend on how
its code ran. Sharing is part of what a file stores, so it enters the
content hash: `list(x, x)` and `list(x, copy_of_x)` hash differently, while
a value read back keeps its sharing and its hash.

### Content hash (since plan-c Stage M)

A directory header of at least 64 bytes holds, after its 40 known bytes, the
file's content hash: XXH3-128 (seed 0) as a low then a high `u64`, a
`scheme:u16`, two zero bytes and `metadata_len:u32` (below). Scheme 1 is defined below; readers ignore a
hash whose scheme they do not know, and a file without one (the Rust
reference's, any 40-byte header) is as valid as before. `rdz_info()` reports
the hash as `content_hash`, 32 hexadecimal digits, high `u64` first (the
canonical XXH128 text).

Scheme 1 hashes the value, not its bytes on disk:

- **Native files:** the eight bytes `RDZH 01 N 00 00`, then every object in
  file order as `type:u16 role:u16 flags:u32 parent:u32 length:u64
  first_child:u32 child_count:u32 first_attribute:u32 attribute_count:u32`
  followed by its values: logical, integer and factor codes as little-endian
  `i32`, doubles as little-endian `u64` bits, each string as its record
  digest (XXH3-64 of `tag:u8 length:u32` and its bytes) as a little-endian
  `u64`; then every attribute entry as `owner name_object value_object
  ordinal flags`, each a `u32`. No encoding, compression, block size,
  dictionary, preset or thread count enters it, nor whether a vector was
  ALTREP.
- **Generic files:** the eight bytes `RDZH 01 G 00 00`, then R serialization
  version 2 (XDR) of the root without its first 14 bytes (format and R
  versions), as the digest package hashes R values; so neither the writing
  R nor ALTREP enters it.

`rdz_hash(x, mode)` computes the same hash for an object in memory, the
native one when `mode` would write `x` natively.

### User metadata (since plan-c Stage N)

A 64-byte directory header's `metadata_len` (bytes 60 to 63, at most 64 KiB)
is the length of a metadata section that ends the directory, after the
synopsis: `count:u32` (at most 1,024), then per entry a `u32` length and the
key's UTF-8 bytes, a `u32` length and the value's. Keys are non-empty and
distinct; readers reject a section that is truncated, has trailing bytes,
is not UTF-8 or repeats a key. The directory's length includes it, so the
directory checksum covers it. It describes the file (source, versions, a
cache key's inputs) and is not part of the content hash; `rdz_info()` reads
it as `metadata`, a named character vector.

## Block directory entry: 64 bytes

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Sequence number |
| 4 | 4 | Flags, two halves (see "Compatibility and extension"); zero today |
| 8 | 8 | Block-header file offset |
| 16 | 8 | Stored-payload file offset |
| 24 | 4 | Stored byte length |
| 28 | 4 | Reserved zero |
| 32 | 8 | Logical element/unit count |
| 40 | 8 | Decoded byte length |
| 48 | 2 | Encoding ID |
| 50 | 2 | Compression ID |
| 52 | 4 | Reserved zero |
| 56 | 8 | XXH3-64 of the stored bytes |

The container requires canonical, consecutive, non-overlapping block spans from the
end of the file header to the start of the directory. This deliberately rejects
gaps, overlap, aliases, and unindexed bytes.

## Native logical representation

The root object uses type tag 1 and independently selected physical encodings in
blocks of at most 65,536 values. Runtime-dispatched scalar, AVX2, and AArch64
NEON classifiers produce the same canonical TRUE and NA bitplanes; SIMD changes
execution only, never file bytes. `FALSE` is implied where neither plane is set,
and overlapping TRUE/NA bits are invalid.

Encoding 3 is a four-byte constant record: one state byte (`0=FALSE`, `1=TRUE`,
`2=NA`) followed by three zero bytes. Encoding 4 starts with
`default_state:u8`, `stored_state_mask:u8`, and two reserved zero bytes. It then
stores one or two `ceil(logical_count / 8)` bitplanes in state order for the
non-default states present in the mask. Bits are low-bit first and padding bits
must be zero. Empty vectors have one canonical four-byte dense record with
FALSE as the default and an empty mask.

Encoding 5 starts with the default state, first patched state, optional second
patched state (`0xff` when absent), and one zero byte; two little-endian `u32`
patch counts and one reserved `u32` follow. Each state stream then contains
strictly increasing little-endian `u16` positions. Encoding 6 has a `u32` run
count and reserved `u32`, followed by eight-byte records containing an exclusive
`u32` run end, one state byte, and three zero bytes. Ends increase strictly,
adjacent states differ, and the last end equals the block length.

Encoding 7 represents exact repetition with a `u16` period from 2 through 64,
six reserved zero bytes, and the period's values in the encoding-1 two-bit
layout (`00=FALSE`, `01=TRUE`, `10=NA`; `11` invalid). It is considered only for
high-transition blocks and requires at least four repetitions. Encoding 1
remains a decoder/reference path but is no longer emitted by the native writer.

The selector obtains exact counts and transition statistics during
classification, compares exact record sizes, and emits constant, dense, sparse,
run, or periodic bytes without trial-compressing every mode. The reader rejects
unknown states, overlapping planes or patches, nonzero padding/reserved fields,
contradictory lengths, non-canonical block sizes, and logical counts that do not
sum to the root descriptor length.

An optional `names` attribute adds two character objects and one attribute
entry. Encoding 2 stores each string as a one-byte tag, a little-endian `u32`
byte length, and exact bytes. Tag 0 is `NA_STRING` and requires length zero;
tags 1 through 4 mean R native, UTF-8, Latin-1, and bytes encodings. Phase 1
accepts native-encoded names only when all bytes are ASCII; non-ASCII native
strings select whole-root fallback because their interpretation is locale
dependent. Today's writer puts each character record in one 1 MiB block, a
writer policy: readers bound a block only by the header's maximum, at most
64 MiB, so a later writer may give a long string its own larger block. Attribute-name and
value blocks are independently addressable, so reading `names` does not touch
the logical data blocks.

### Native integer and double representation

Since plan-c Stage F the native root may be an integer (type tag 2) or double
(type tag 3) vector, with the same optional `names` attribute as a logical
root. Blocks hold up to 262,144 integers or 131,072 doubles (1 MiB raw); the
writer fills every block but the last; an empty vector is one empty raw block. Each
block's record, before any compression:

- **10, integer raw:** `n` little-endian `i32`, `NA` as `INT32_MIN`.
- **11, integer shuffled raw:** the same bytes as four planes, byte `k` of
  value `i` at `k * n + i`.
- **12, frame of reference:** `width:u8` (0 to 32), `has_na:u8` (0 or 1),
  two zero bytes, `base:i32`, then `n` codes of `width` bits, packed least
  significant bit first; a value is `base + code`, and with `has_na` the
  all-ones code is `NA` (the width leaves room for it). Unused high bits of
  the last byte are zero. An all-`NA` block has base 0.
- **13, delta:** `width:u8`, three zero bytes, `first:i32`, `min_delta:i64`,
  then the `n - 1` codes `value[i] - value[i - 1] - min_delta`, packed as
  above; the width is at most 32. No `NA`; `n >= 2`.
- **14, integer runs:** `runs:u32`, `0:u32`, then per run `value:i32` and
  `end:u32` (exclusive); consecutive runs differ, ends increase to `n`.
- **20, double raw:** `n` little-endian 64-bit patterns, exact: every NaN
  payload, `NA` and `-0` survive.
- **21, double shuffled raw:** the same bytes as eight planes.
- **22, double runs:** `runs:u32`, `0:u32`, then per run `bits:u64`,
  `end:u32`, `0:u32`; runs compare bit patterns.
- **23, double decimals** (since plan-c Stage J; ALP, Afroozeh, Kuffó and
  Boncz, SIGMOD 2024, with a division decode): vectors of 1,024 values (the
  last shorter), back to back, nothing else. Each vector is `e:u8` (0 to 18),
  `f:u8` (0 to `e`), `width:u8` (0 to 64), `flags:u8` (bit 0: delta; the rest
  zero), `exceptions:u16`, `0:u16`, `base:i64`; with delta, `first:i64`; then
  the codes, `width` bits each, packed as above (the vector's length of them,
  or one fewer with delta), unused high bits zero; then the exceptions'
  positions (`u16`, strictly increasing, below the vector's length) and their
  values (`u64` bits). Without delta, value `i` is `n = base + code[i]`; with
  delta, `n[0] = first` and `n[i] = n[i-1] + base + code[i-1]`, in
  two's-complement 64-bit arithmetic. Every `n` satisfies
  `|n × 10^f| < 2^53`, and the value is the double nearest
  `n × 10^f ÷ 10^e` (ties to even): one division in IEEE double arithmetic,
  both operands exact, and the double R parses or rounds that decimal to. An
  exception replaces the value at its position with its bits, so NA, NaN
  payloads, `-0`, infinities and full-precision values survive exactly. The
  record is exactly as long as its vectors. Writers choose `e` and `f` by
  sampling, and use this record only when compressing (not the `speed`
  preset) and when it is smaller than raw and runs; the pipeline then
  compresses it like any record. Readers on x87 extended precision decode
  through `strtod()`, which rounds once.

Every decoded value is checked: an integer record cannot produce `INT32_MIN`
except through its `NA` code. The writer chooses the smallest record from one
pass of block statistics; between raw and shuffled raw (the same size) it
stores shuffled planes when the block will be compressed, unless a sample of
4,096 values is under half distinct, when plain bytes keep the repeats zstd
finds. Neither choice is recorded beyond the encoding ID, and readers accept
any record that decodes.

### Native character and factor representation

Since plan-c Stage G the native root may also be a character vector (type tag
4), optionally named, stored as the same character records as `names`
(encodings 2, 8 and 9 below; the R thread packs them, workers compress them),
or a factor (type tag 5). A factor root's blocks are its integer codes (the
integer encodings 10 to 14, `NA` as `INT32_MIN`); its levels are its one child
object (object 1, role 3 = levels, type character, a leaf); root flag bit 0
marks an ordered factor (class `c("ordered", "factor")`, otherwise
`"factor"`). A native factor has no `names` and no other attribute. Readers
reject a code outside `1..length(levels)` that is not `NA`. Objects and their
blocks come in the order root, levels, attribute name, attribute value.

Strings are bytes plus R's encoding tag. A native-encoded string that is not
ASCII is not portable (portability.md) and is never written natively: the
whole root goes to the generic codec in automatic mode, and strict native
mode rejects it. Today's writer sends a string longer than its 1 MiB blocks
the same way (a writer policy; see above). Since Stage G the default dictionary policy is `auto`.

### Native object graphs

Since plan-c Stage H a native file holds an object graph. Object types add
`0` NULL, `6` list and `7` data frame; roles add `4` child. Object 0 is the
root. Every other object comes after its parent and is one of: a child (a
list's element or a data frame's column; a container's children are
contiguous, `first_child` and `child_count`), a factor's levels (role 3), or
an attribute's name or value (roles 1 and 2). Writers number objects
breadth-first: each object, then its levels and attribute objects, then its
children. A list's `logical_length` is its element count; a data frame's is
its row count, which every column must match. Containers and NULL have no
blocks; every other object's blocks follow the previous object's, so blocks
run in object order. A file may have no block at all (a list of empty
lists). Nesting is at most 1,000 levels below the root.

Attributes are contiguous per owner, in owner order, `ordinal` counting from
0. Their flags: `1` names (on any vector, factor excepted, list or data
frame; as long as the vector or the container's children), `2` row.names (a
data frame's explicit row names, character or integer, one per row) and `4`
class (a data frame's class, when it is not exactly `"data.frame"`). The
attribute-name object holds the R attribute's name, which must match the
flag. Since plan-c Stage I, flag `8` is a general attribute: any other
attribute, its name in the name object (ASCII, not empty, and never one a
codec holds: `names` and `row.names` anywhere, `class` on a factor or data
frame, `levels` on a factor), its value any object but NULL -- a vector, a
factor, a list or a data frame, with children and attributes of its own,
each level counted against the nesting limit. NULL has no attributes. A
reader sets general attributes in ordinal order after the codec's own, and
a value R refuses (a `dim` that does not fit) is a format error. This is how
a Date's class, a POSIXct's `tzone`, a matrix's `dim` and `dimnames`, or a
data.table's `sorted` key are native. A data frame without a row.names attribute has compact row names; one
without a class attribute has class `"data.frame"`. A data.table's
`.internal.selfref` is not stored; readers restore it. Compact row names read
back as R's own compact form `c(NA, -n)` whatever sign they were written with:
`identical()` holds, and only `.row_names_info(x, 1)`'s sign can differ.

Since Stage H every block, logical ones included, is compressed under the
writer's preset (raw when that is not smaller): the `speed` preset writes
the Phase 1 bytes the Rust reference wrote.

### Character dictionary blocks (experimental)

The `names` value object may mix encoding 2 with dictionary blocks. Encoding 8
holds dictionary entries in exactly the encoding-2 record layout; its logical
count is the number of entries, which do not count toward the object's length.
Entries are numbered from zero in file order across all encoding-8 blocks of
the object. Encoding 9 holds elements as dictionary ids: `width:u8` (1, 2, or
4), three zero bytes, `base:u32`, then `logical_count` little-endian unsigned
values of `width` bytes. Element `i` is entry `base + stored[i]`, which must
refer to an entry in an earlier block; the stored length must be exactly
`8 + logical_count * width`.

The object's length is the sum of the logical counts of its encoding-2 and
encoding-9 blocks. The total of its encoding-8 counts may not exceed that
length, which bounds the dictionary a reader allocates before decoding.

Writers emit index blocks of at most 65,536 elements, each preceded by the
entry blocks for values first seen in it, so reads are one forward pass. The
writer policy is not recorded: a `global` writer keeps one dictionary for the
whole vector, a `block` writer restarts it every index block, and `plain`
emits encoding 2 only. `auto` estimates the share of distinct values from
16,384 positions drawn with a fixed-seed generator (bias-corrected Chao1) and
writes `global` below a threshold, otherwise `plain`. Any dictionary policy
writes the remainder of a vector as encoding 2 once its dictionary would
exceed 2^22 entries, so one object may contain dictionary blocks followed by
plain blocks. Deduplication uses CHARSXP addresses, which R's global string
cache makes unique per bytes and encoding; addresses are never written. The
experimental `RDZ_STRING_DICT` environment variable selects the policy; the
default is `auto` (since plan-c Stage G).

## Generic synopsis

The synopsis is a bounded R serialization version-3 XDR stream containing only
root type, bounded class names, length, bounded dimensions, bounded attribute
names, and truncation/authority flags. It is informative, not an independently
decoded object graph. It never contains arbitrary attribute values and explicitly
states that exact generic attributes require a full read.
If optional synopsis collection encounters an unusual R representation or
encoding it cannot summarize safely, the writer stores a smaller bounded
synopsis with `synopsis_error = TRUE`; this never prevents the already-supported
whole-root generic codec from preserving the object.
Successful collection emits the same schema with `synopsis_error = FALSE`.

## Closing trailer: 40 bytes

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Magic `RDZT` |
| 4 | 2 | Container version, 3 |
| 6 | 2 | Trailer length, 40 |
| 8 | 8 | Directory file offset |
| 16 | 8 | Directory byte length |
| 24 | 8 | XXH3-64 of the complete directory bytes |
| 32 | 4 | Reserved zero |
| 36 | 4 | Closing magic `ZEND`, the last four bytes of the file |

The directory must end exactly where the trailer begins. A metadata read verifies
the header checksum, trailer, directory checksum, directory-header checksum, and
all structural bounds. A full read additionally verifies every indexed block
header and block checksum before passing generic bytes to `unserialize()`.

## Write and fallback rules

The native adapter attempts eligibility and native writing in one call. It
returns success only after the native file is complete, the distinguished
unsupported-native outcome, or a fatal error. `auto` may fall back only for the
distinguished outcome; `native` surfaces it; `r` bypasses the attempt and forces
`R_SERIAL_V3`. Since plan-c Stage I the native codecs take logical, integer,
double and character vectors (ALTREP ones from their materialised data),
factors, lists and data frames, nested, each with any attributes whose names
are ASCII and whose values are native in turn. S4 objects, other types
anywhere (environments, calls, complex, raw), row names off a data frame, a
data frame column longer than its rows, and a non-ASCII native-encoded string
produce the distinguished whole-root fallback. Automatic mode also leaves to
the generic codec an object of at least 1,024 parts with data averaging under
1 KiB each: every part costs about 160 bytes of directory and headers and a
compression frame of its own (a list of 500,000 pairs is 88 MB native, 1 MB
generic). This is writer policy; forced native mode writes such objects
natively.

The generic payload is streamed (since plan-c Stage C): `R_Serialize()` writes
through a callback that fills 1 MiB blocks into a same-directory temporary
file, the directory and trailer follow, and the file is closed, given an
existing destination's permissions, and renamed over the destination. An
error or interrupt during serialization removes the temporary file at once. A platform rename that can replace the target is atomic for process
observers but is not crash-durable because RDZ does not call `fsync`/`sync_all`.
On platforms where rename cannot replace an existing target, the backup-and-
rollback fallback has a brief missing-destination window and can strand the
backup after a process or machine crash. Dropping an uncommitted writer removes
its temporary file. A future opt-in durable mode may sync the file and parent
directory.

Decided 2026-10-05 for the C implementation, and implemented at Stage C: the
generic payload is streamed.
`R_Serialize()` writes through an `R_outpstream` whose callback fills the current
block, and `R_Unserialize()` reads through an `R_inpstream` that pulls verified
blocks in order, so neither the whole raw vector nor a separate whole-payload
integrity pass exists. The stored bytes are unchanged: the concatenated generic
blocks are exactly `serialize(x, NULL, version = 3, xdr = TRUE)`.
