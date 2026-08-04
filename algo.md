# Algorithms by R type

How `rdz` encodes each R type, per preset. Derived from the source, not from
the design notes: [rdz.c](src/rdz.c), [logical_codec.c](src/logical_codec.c),
[integer_codec.c](src/integer_codec.c), [numeric_codec.c](src/numeric_codec.c).

Two presets map to two on-disk formats:

| preset | magic | intent |
|---|---|---|
| `"speed"` (default) | `FASTRDS1` | node header + raw `memcpy` of payloads |
| `"balanced"` | `FASTRDS2` | adds size transforms, each gated on an exact or measured check |

**The selection principle**: a balanced transform is chosen only when it is
*exact* (bit-for-bit reconstruction, e.g. sequences and dictionaries keyed on
bit patterns) or *measured* (the fully-encoded layout is compared against the
raw layout before being written). No transform is selected on a guess. Sampling
appears only as a cheap *rejection* filter ahead of an exact full scan.

---

## Container format

**File header — 12 bytes.**

```
magic[8]      "FASTRDS1" (speed) | "FASTRDS2" (balanced)
codec         u8   1 = native, 2 = R serialization
endian        u8   host byte order
sizeof(int)   u8
sizeof(double) u8
```

The reader rejects any file whose architecture bytes don't match the host —
files are not portable across architectures.

**Node layout** (native codec, recursive):

```
type          u8   see node types below
is_object     u8   Rf_isObject flag
length        u64
attr_count    u32
  per attribute: name_len u32, name bytes, <node>
<payload>          type-specific, described below
```

Node types: `0 NIL`, `1 LOGICAL`, `2 INTEGER`, `3 REAL`, `4 COMPLEX`,
`5 RAW`, `6 STRING`, `7 LIST`. Attributes are themselves nodes, so an
attribute gets the same codec selection as any vector. Nesting is capped at
`RDZ_MAX_DEPTH` (10,000).

---

## Logical vectors (`LGLSXP`)

**Speed** — direct `memcpy` of R's `int` array, 4 bytes per value.

**Balanced — two-bit packing.** Always attempted; a 4× reduction is
unconditional, so there is no size test.

Codes: `FALSE → 0`, `TRUE → 1`, `NA → 2`. Code `3` is unused and invalid.
Four values per byte, low bits first:

```
byte = code0 | code1<<2 | code2<<4 | code3<<6
```

The tail (`length % 4`) is written into one byte with the unused high bits
zeroed. Encoding falls back to direct if any element is not 0/1/`NA`.

*Decode validation.* `(byte & (byte >> 1) & 0x55) != 0` rejects any occurrence
of code `3`; the tail byte is additionally checked so unused bits must be zero.
Both guard against corrupt input decoding into a silently wrong vector.

---

## Integer vectors (`INTSXP`)

**Speed** — direct `memcpy`, 4 bytes per value.

**Balanced — decision ladder:**

**1. Constant-delta sequence** — *ALTREP inputs only.* Streams the vector
through `INTEGER_GET_REGION` in 64 KiB windows; if `values[i] == base + i*delta`
holds throughout, stores `base` (u32) + `delta` (u32) — 8 bytes total,
regardless of length. This is what makes `1:n` nearly free.

> Note the asymmetry: sequence detection is **not** applied to materialized
> integer vectors, only ALTREP ones. A materialized `as.integer(seq(...))`
> takes the packing path below.

**2. Frame-of-reference bit packing.**

- *Sample filter*: up to 1,024 stratified samples; if their range already needs
  more than 8 bits, abandon immediately without a full scan.
- *Full scan*: exact `min`, `max`, and `NA` presence.
- `bits = ceil(log2(range + has_na))`, capped at
  **`RDZ_INTEGER_MAX_PACKED_BITS = 8`**.
- `NA` is reserved as code `0` and all values shift up by one — but **only when
  the vector actually contains `NA`**, so NA-free vectors pay nothing.
- *Exact size check*: rejected unless `4 + 1 + 1 + packed_size < 4 * n`.
- *Packing*: groups of 8 values are assembled into a `uint64` word and emitted
  as `bits` bytes (so 8 values always occupy a whole number of bytes); the
  remainder goes through a bit accumulator.

Wire: `base` i32, `bits` u8, `flags` u8, then the packed stream.

**3. Direct** otherwise.

*Decode validation.* Rejects unknown flag bits, `bits > 8`, a base of
`NA_INTEGER`, reconstructed values exceeding `INT_MAX`, trailing bits that
aren't zero, and a declared-`NA` encoding in which no `NA` actually appears.

> ### ⚠ The 8-bit cliff
> There is no 16-bit tier: a range needing 9 bits falls straight to raw 32-bit
> storage. Measured on 1M integers against gzipped `.rds`:
>
> | value range | balanced | `.rds` | ratio |
> |---|---:|---:|---:|
> | 1..256 | 977 KB | 1,512 KB | **0.65×** |
> | 1..300 | 3,906 KB | 1,567 KB | **2.49×** |
> | 1..1000 | 3,906 KB | 1,800 KB | 2.17× |
>
> IDs, counts, and category codes above 256 distinct values are common, which
> makes a 16-bit tier the highest-value size fix available.

---

## Numeric vectors (`REALSXP`)

**Speed** — direct `memcpy`, 8 bytes per value.

**Balanced — decision ladder, first match wins:**

**1. Constant-delta sequence (ALTREP)** — as for integers, storing `base` and
`delta` as raw u64 bit patterns.

**2. Bit-pattern dictionary.**

- *Sample filter*: requires `length >= 64`; samples 1,024 stratified values and
  abandons if they contain more than 64 distinct bit patterns.
- *Build*: two strategies. When the sample found ≤ 8 distinct values, a linear
  scan is used, falling back to the general builder if an unsampled value turns
  up. The general builder uses a 512-slot open-addressing hash with a
  murmur-style `mix64` finalizer and linear probing, capped at
  `RDZ_REAL_DICTIONARY_MAX = 256` entries.
- Keys are **raw bit patterns**, not numeric values — so `+0`/`-0` and distinct
  `NaN` payloads are preserved rather than collapsed. The encoding is exact.
- `bits = ceil(log2(count))` (≤ 8); indexes are bit-packed.
- *Exact size check*: rejected unless
  `1 + 2 + 8*count + packed_size < 8 * n`.
- The packed index block is then offered to LZ4; if it compresses, the encoding
  becomes `DICTIONARY_LZ4`.

**3. Constant-delta sequence (materialized).** Requires `length >= 3` and
finite `base`/`delta`. Verifies `values[1]` bit-for-bit *first* — this
explicitly guards against floating-point cancellation choosing an inexact
delta — then checks every remaining element against `base + i*delta`. Stores 16
bytes. This is what collapses `Date` and `POSIXct` sequences (measured: 200k
dates → 0.1 KB versus 416 KB for `.rds`).

**4. XOR-delta + byte transpose + LZ4.** For high-cardinality doubles.

- Blocks of `RDZ_REAL_XOR_BLOCK_VALUES = 8192` doubles (64 KiB).
- Each value's bit pattern is XORed with its predecessor. Values that are close
  in magnitude share sign and exponent bits, so those bits cancel to zero.
- The 8 bytes of each delta are **transposed into 8 contiguous lanes**
  (most-significant lane first), clustering the now-mostly-zero high bytes
  together so LZ4 has long runs to work with.
- Each block is LZ4-compressed independently.
- *Two-stage gate*: the **first block** is a control point — if it doesn't
  compress to ≤ 7/8 of its raw size, the whole attempt is abandoned before any
  further work. Then the **complete output** must satisfy
  `output <= raw - raw/8`, i.e. **≥ 12.5% saving**, or it is discarded.
- Blocks after the first are compressed in parallel when there are at least
  `RDZ_PARALLEL_XOR_MIN_BLOCKS = 16` of them and `rdz_threads() > 1`.

**5. Direct** otherwise.

---

## Character vectors (`STRSXP`)

Two layouts. The choice is made by a **sampled cardinality heuristic**,
`strings_should_use_flat`: sample up to 1,024 stratified elements and count
distinct `CHARSXP` *pointers* (R's global string cache makes pointer identity
equivalent to value identity, so this is a pointer comparison, not `strcmp`).

```
use flat when  distinct * 4 >= non_missing * 3     (distinct ≥ 75% of sample)
otherwise      use the dictionary layout
```

High cardinality → flat; repeated values → dictionary.

### Dictionary layout (both presets)

```
width      u8    1, 2, or 4 bytes, chosen from dictionary size
dict_size  u64
  per unique string: encoding u8, length u64, bytes
indexes          dict_size-width integers, one per element
```

Deduplication is by `CHARSXP` pointer hash.

### Flat layout

```
layout     u8    0 = FLAT_RAW, 3 = FLAT_LZ4
encoding   u8    the common encoding, or 0xFF when mixed
payload_sz u64
```

- **Speed (`FASTRDS1`)** — raw: an `int32` length per element (`-1` encodes
  `NA`), an optional per-element encoding byte when encodings are mixed, then
  the concatenated bytes.
- **Balanced (`FASTRDS2`)** — additionally tries:
  - the concatenated payload in independent 64 KiB LZ4 blocks, and
  - **RLE-varint length metadata**, used only when it is smaller than the
    `4n` direct length array (fixed-width strings compress to almost nothing).

  LZ4 is accepted only when the **complete encoded layout** —
  `9 + length metadata + mixed-encoding bytes + compressed blocks` — is
  strictly smaller than the raw layout (`payload + 4n + mixed`). The two
  choices are evaluated against the full alternative, not independently.

String encodings are normalized to `CE_NATIVE`/`CE_UTF8`/`CE_LATIN1`/`CE_BYTES`;
a per-element encoding byte is written only when the vector actually mixes them.

---

## Lists (`VECSXP`)

Lists have **no payload compression of their own**. A list node writes its
header and attributes, then recursively encodes each child in order. Each child
independently runs the full selection ladder for its own type — this is where
the columnar behaviour of data frames comes from.

Names are taken from the `names` attribute, which is itself stored as an
ordinary string node (and so gets dictionary or flat encoding like any other
character vector).

**Two structural limits, both deliberate:**

- **Shared references are not preserved.** A `SEXP` referenced twice is written
  twice and read back as two independent objects. R serialization preserves
  such sharing; `rdz` does not.
- **Recursive structures are rejected**, not encoded. `native_supported` walks
  the object with an explicit `sexp_stack_t` and, on detecting a cycle, routes
  the *entire object* to the R codec.

---

## Data frames, tibbles, and data tables

**There is no data-frame node type.** A data frame is a `VECSXP` carrying
`names`, `row.names`, and `class` attributes, so it takes the list path above,
and each column is encoded independently by its own type's ladder. A mixed
frame therefore ends up with a different strategy per column at no extra cost —
`explain_rdz()` shows exactly this.

Type-specific behaviour that follows from that:

- **Factors** are `INTSXP` plus a `levels` attribute, so integer bit-packing
  usually applies to the codes and the levels become a string node. Measured
  0.72× the size of `.rds`.
- **`row.names`** — `compact_automatic_row_names` detects automatic row names
  held as an ALTREP integer sequence (streamed in 256-element windows) and
  normalizes them to R's compact representation, so `1..n` row names cost a
  fixed few bytes rather than `4n`.
- **`Date` / `POSIXct`** columns are doubles with a class attribute and hit the
  constant-delta sequence path whenever they're regularly spaced.
- **`data.table`** — at **depth 0 only**, the `.internal.selfref` external
  pointer attribute is dropped on write (`is_data_table_selfref`) and rebuilt
  on read by `read_rdz()` calling `data.table::setDT()` when the package is
  installed. A **nested** data table still contains an unsupported `EXTPTRSXP`
  and sends the whole object to the R codec.

---

## Cross-cutting mechanics

**LZ4 block framing.** Every LZ4 user (numeric dictionary indexes, XOR lanes,
flat string payloads) shares the same framing: 64 KiB input blocks, each
written as `u32 stored_size` + bytes. A block whose compressed form isn't
smaller is stored raw. The **first block acts as a control point** — if
`compressed + 4 >= block_size`, the entire attempt is rejected before the rest
of the input is touched.

**ALTREP.** Only `INTSXP` and `REALSXP` ALTREP vectors are handled natively;
they are streamed through `*_GET_REGION` in 64 KiB windows rather than
materialized. Every other ALTREP class — including **deferred-string ALTREP**,
which is what `as.character(1:n)` produces — is unsupported and sends the whole
object to the R codec.

**Reading.** Files are memory-mapped (`mmap`) on POSIX, with a Windows
fallback. Payloads of 64 KiB or more are read with `pread` rather than copied
through the mapping.

**Threading.** Parallelism is used in exactly one place: LZ4 compression of
XOR-delta numeric blocks, and only above 16 blocks. Everything else is
single-threaded.

**Decode validation.** Every codec validates encoding tags, index bounds,
declared sizes, padding bits, integer overflow, and truncation, and raises an R
error rather than returning a wrong value. A sweep of 264 truncation lengths
and 512 header-byte corruptions per format produced clean R errors with no
crashes.

---

## Fallback

`native_supported` pre-scans the whole object. Supported: `NULL`, logical,
integer, double, complex, raw, character, and list — plus their attributes,
recursively. Anything else (closures, environments, S4, language objects,
external pointers, non-integer/double ALTREP) makes the scan fail.

**Fallback is all-or-nothing**: one unsupported node sends the *entire* object
through `R_Serialize` with `R_pstream_binary_format`, **uncompressed**, while
`saveRDS()` gzips by default. A data frame with a single closure column
measured 6.67× larger than the equivalent `.rds`. `codec = "native"` errors
instead of falling back; `codec = "r"` forces this path.

---

## Coverage gaps

| type | balanced treatment | vs. gzipped `.rds` |
|---|---|---|
| complex | direct only — no codec | 1.50× larger |
| raw | direct only — no codec | 1.00× |
| `difftime` | double, but irregular → direct | 1.50× larger |
| integer, range > 8 bits | direct — no 16-bit tier | up to 2.49× larger |
| materialized integer sequence | packing only, no sequence path | — |
| ALTREP character | unsupported → R codec | — |
