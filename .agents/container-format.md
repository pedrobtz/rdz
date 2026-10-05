# RDZ Pre-0.1 Container Format

## Status

This document is the wire contract implemented by Phase 0B. It is intentionally
pre-release: files may still be replaced until `rdz` 0.1.0. The current
container version is 3 (2026-10-05, plan-c.md §3): every checksum is an
eight-byte XXH3-64 with seed 0, bit-identical to the reference xxHash and to
zufast's `zuf_hash64()`, in place of version 2's four-byte IEEE CRC32. Readers
reject version 2 files; no version 2 file was ever released. All multibyte integers are unsigned little-endian values;
there is no implicit padding and every reserved field must be zero.

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
| Compression | 0 | No compression |
| Compression | 1 | zstd frame (reserved; not yet written or accepted) |
| Checksum | implicit v3 | XXH3-64, seed 0, stored little-endian |

The writer uses 1 MiB blocks. Readers accept at most 64 MiB per block, 1,000,000
blocks, and a 64 KiB generic synopsis. Counts, lengths, offsets, additions, and
host-size conversions are checked before allocation or seeking.

Native type tags are reserved as `0=NULL`, `1=logical`, `2=integer`, `3=double`,
`4=character`, `5=factor`, `6=list`, and `7=data frame`. A tag is not usable
until its codec phase defines its logical and physical record representation.
Unknown IDs, versions, mandatory flags, or nonzero reserved fields are errors.

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
| 8 | 4 | Flags, currently zero |
| 12 | 2 | Payload codec ID |
| 14 | 2 | Payload codec version |
| 16 | 4 | Maximum decoded block size |
| 20 | 4 | Reserved zero |
| 24 | 8 | XXH3-64 of bytes 0 through 23 |

## Block header: 48 bytes

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Magic `RBLK` |
| 4 | 2 | Header length, 48 |
| 6 | 2 | Flags, currently zero |
| 8 | 4 | Zero-based consecutive sequence number |
| 12 | 2 | Encoding ID |
| 14 | 2 | Compression ID |
| 16 | 8 | Logical unit count; bytes for the generic raw stream |
| 24 | 8 | Decoded byte length |
| 32 | 4 | Stored byte length |
| 36 | 4 | Reserved zero |
| 40 | 8 | XXH3-64 of the exact stored bytes |

Raw blocks require logical count, decoded length, and stored length to agree.
For every uncompressed native block, decoded and stored byte lengths agree while
logical count records the number of R elements represented by those codec bytes.
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
| 6 | 2 | Directory header length, 40 |
| 8 | 2 | Object entry width, 48 |
| 10 | 2 | Attribute entry width, 32 |
| 12 | 2 | Block entry width, 64 |
| 14 | 2 | Flags, currently zero |
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

## Block directory entry: 64 bytes

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Sequence number |
| 4 | 4 | Flags, currently zero |
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
dependent. Each character record must fit in one 1 MiB block. Attribute-name and
value blocks are independently addressable, so reading `names` does not touch
the logical data blocks.

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
default is `plain`.

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
`R_SERIAL_V3`. Phase 1 accepts non-ALTREP logical roots with no attributes or an
eligible `names` attribute. Any other logical attribute, unsupported name
representation, or root type produces the distinguished whole-root fallback.

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
