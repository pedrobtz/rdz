# rdz review — soundness, quality, simplicity

> **Historical document.** This point-in-time review is not maintained. Code
> references and recommendations reflect the repository on 2026-07-02. Use
> [the current roadmap](../roadmap.md) and [development status](../status.md)
> for active decisions and work.

Reviewer: Claude (Opus 4.8). Date: 2026-07-02. Scope: full read of `R/rdz.R`,
`src/rdz.c` (2678 lines), the three codec units, `threading.c`, the test suite,
and the benchmark results, evaluated against the stated goal: **a full
replacement for the `rds` format that is faster than `qs2` and `fst`.**

## Executive summary

The engineering quality is genuinely high. The C is defensive and memory-safe,
the balanced codec's "never regress" gating is a sound and unusual design
choice, and the benchmarks — taken at face value — show consistent wins over
`qs2` and `fst` on the shapes tested. As a *low-latency cache format* this is a
strong, well-built piece of work.

However, the package as it stands is **not** a full replacement for `rds`, and
the distance is structural, not cosmetic. The single biggest gap is that rdz
files are **locked to the writing machine's architecture** — including the R
serialization fallback — so they are not portable the way `.rds` files are.
Three other capability gaps (connections/in-memory serialization, all-or-nothing
fallback granularity, and single-threaded reads) separate it from both the "drop
in for `saveRDS`" goal and the "beat qs2/fst everywhere" goal.

The README is honest — it calls the package "an experiment in minimizing
serialization latency," not an rds replacement. Closing the gap to the stated
goal is a real project, summarized in the roadmap at the end.

---

## What is sound and well done

**Memory safety and error discipline (excellent).** Every allocation checks for
failure and flips `writer->failed` or returns an error code; overflow is guarded
against `SIZE_MAX`/`INT_MAX`/`UINT64_MAX` before every multiply
(`rdz.c:169`, `642`, `791`, `1791`, `numeric_codec.c:132`). Cleanup on error and
R longjmp is routed through `R_UnwindProtect` so the file handle and the mmap/fd
are always released (`rdz.c:2265`, `2593`, `2672`). PROTECT usage is correct.
This is the level of C hygiene the CI (ASAN/UBSAN/valgrind/rchk/gctorture)
demands, and the code lives up to it.

**Decode validation (excellent).** Every encoding tag, block length, padding
bit, dictionary index bound, and the final `position == packed_size` invariant
is checked before use (`numeric_codec.c:320-341`, `reader_lz4_blocks` at
`rdz.c:302`, `reader_real_xor_lz4` at `rdz.c:330`). The malformed-input tests in
`test-roundtrip.R` are unusually thorough — they mutate real files byte-by-byte
and assert clean, specific errors. This is the right posture for a reader that
`mmap`s untrusted bytes.

**The "measured saving or don't bother" contract (sound).** The balanced preset
never loses to speed by more than a one-byte tag. Dictionaries are rejected
unless the full encoded layout beats raw (`numeric_codec.c:184`, `269`),
sequences are accepted only after a *bit-for-bit* reconstruction check
(`numeric_codec.c:356-365` — note it verifies index 1 explicitly to defend
against floating-point cancellation), and the XOR/LZ4 path requires ≥12.5%
saving on both a cheap first-block control point and the whole payload
(`rdz.c:1006`, `1029`). This makes "balanced" safe to recommend unconditionally,
which is a real usability win and a defensible simplicity boundary.

**`explain_rdz()` runs the real serializer** (`rdz.c:2337`), so the reported
plan cannot drift from what is actually written. The root byte count equals the
file size, verified in tests. Honest introspection.

**data.table handling is careful.** Dropping only `.internal.selfref` and
rebuilding it with `setDT()` on read (`rdz.c:408`, `R/rdz.R:54`) preserves class,
columns, and key without copying, and the test even mutates the restored table
by reference to prove the self-ref is real.

---

## Gaps against the goal "full replacement for rds"

### 1. Files are not portable across architectures — the headline blocker
The writer stamps `{endianness, sizeof(int), sizeof(double)}` into the header
and the reader **rejects any mismatch** (`rdz.c:2248`, `2573`). Worse, this is
not limited to the native codec: the R-serialization *fallback* is written with
`R_pstream_binary_format` (`rdz.c:2258`), which is also host-endian. So **no
rdz file is portable** — not even for objects that fall back.

`.rds` defaults to XDR (`R_pstream_xdr_format`), which is why an `.rds` written
on x86-64 reads on ARM/PPC/32-bit. Any workflow that moves files between
machines, commits fixtures to a repo shared across CI architectures, or reads
old files on a migrated host will break. This is the defining difference between
"a fast local cache" and "an rds replacement," and it must be surfaced
prominently (it currently only lives in CLAUDE.md as "not an archival format").

*Options:* (a) accept it and re-scope the goal to "fast local/single-arch
cache"; or (b) add an opt-in portable mode — byte-swap on read when the header
disagrees for the native path, and use `R_pstream_xdr_format` for the fallback.
Little-endian is universal enough that read-side byte-swapping covers ~all real
targets at modest cost.

### 2. No connection / in-memory / streaming support
`write_rdz`/`read_rdz` take a file path and call `fopen`/`mmap` only
(`rdz.c:2303`, `2661`). `saveRDS`/`readRDS` accept **connections** (`gzfile`,
`url`, `pipe`, sockets) and R has `serialize()`/`unserialize()` for raw vectors.
rdz has no equivalent, so it cannot: serialize to memory (Redis/HTTP payloads),
write to a compressed or remote connection, or read from a URL. For a "full
replacement," an in-memory `serialize_rdz(x) -> raw` / `unserialize_rdz(raw)`
pair is close to mandatory and is architecturally cheap (the writer already
targets an abstract sink; only the mmap reader assumes a file).

### 3. Fallback is all-or-nothing, at whole-object granularity
`native_supported` scans the entire tree and if *any* node is unsupported — a
closure, environment, S4 object, or a non-INT/REAL ALTREP (e.g. a deferred-string
or factor ALTREP) — the **whole object** serializes through R
(`rdz.c:2291`, `2296`). A data frame with one function column, or a list holding
a model object beside large vectors, gets **zero** native speedup. `qs2` does
not do this: it encodes each element and only pays R's serializer where needed.
This both narrows "replacement" coverage and directly caps the performance goal
on mixed real-world objects.

### 4. Reference semantics and shared structure are not preserved
Documented, but worth stating against the goal: the native path does not
preserve shared list-node identity, and reference objects (environments, R5/R6)
force fallback. `.rds` preserves the reference table (shared CHARSXPs,
environments, attributes). For objects where sharing is load-bearing, rdz either
falls back (correct, slower) or — for lists with genuine internal sharing —
changes identity on the native path. Fine for a cache, not for a general
`rds` replacement.

### 5. No integrity check
No checksum anywhere. `.rds` also lacks a strong checksum, so this is arguably
parity, but a format positioning itself as a durable replacement that additionally
`mmap`s its input would benefit from at least a length/CRC on the payload. Right
now truncation is caught structurally (good) but silent bit-rot is not.

---

## Gaps against the goal "faster than qs2 and fst"

The current wins are real but come almost entirely from **doing no compression
on the speed path** (raw `memcpy`, `rdz.c:1802`) plus `mmap` reads. That is a
sound way to win on latency, but it leaves three scaling levers unused, and the
fst benchmark already shows the lead shrinking (~16% write / ~10% read) once fst
is allowed its threads.

### 6. Reads are entirely single-threaded
There is no parallelism anywhere in the decode path — confirmed: the only
`rdz_parallel_run` call site is the *encoder's* XOR path (`rdz.c:1015`), and even
`reader_real_xor_lz4` decompresses serially (`rdz.c:330`). fst and qs2
parallelize decompression. rdz's read advantage today is "no decompression to
do"; on balanced/compressed files, or on many-core machines reading large files,
this is the first place the advantage will erode. The XOR blocks are already
independent and self-describing — read-side parallelism is low-hanging fruit.

### 7. Writes parallelize only one encoding
Only the double XOR-delta path uses threads (`rdz.c:1776` → `1015`). Multi-column
data frames are encoded column-by-column, serially (`encode_node` list loop,
`rdz.c:1809`). fst/qs2 parallelize across columns/blocks. For wide frames — the
exact workload fst targets — rdz leaves cores idle. The node tree is a natural
unit of parallelism (independent siblings), though it interacts with the
single sequential output stream, so this needs a block-index/offset table.

### 8. Two full traversals per write
`C_rdz_save` runs `native_supported` over the entire tree *first*
(`rdz.c:2291`), then `encode_node` traverses it *again*. On R < 4.6 the support
scan calls `eval(attributes(object))` at **every node** (`rdz.c:521`), allocating
a list and materializing all attributes per node — then the encoder re-reads the
same attributes. For deep or attribute-heavy structures (nested lists, many
factor columns) this roughly doubles traversal cost and is pure overhead on the
latency-critical path. Consider a single encode pass that buffers into a scratch
sink and only commits if it completes, or fold support-checking into the encoder
with rollback.

### 9. String encoding is the weak column type
The benchmarks confirm it: "strings repeated" and "strings unique" are the only
write losses, and unique-string read is a near-tie. Strings go through per-element
`CHAR()`/`LENGTH()` loops and LZ4 blocking (`rdz.c:1160`, `780`). This is the one
type where qs2's approach is competitive, and it is worth targeted profiling.

---

## Simplicity assessment

Mostly good, with two maintenance liabilities:

- **Dual attribute-iteration paths.** The R ≤4.5 `eval(attributes())` path and
  the R ≥4.6 `R_mapAttrib` path (`rdz.c:437-554`) must be kept behaviorally
  identical forever, and they encode the same data-table-selfref and
  automatic-rownames special cases twice (`compact_automatic_row_names` exists
  only on the old path, `rdz.c:438`). This is real duplicated surface. Given the
  perf note in #8, consider standardizing on one path; if the C API is kept for
  speed, at least unit-test the two paths against each other on identical inputs.

- **Two-tier numeric dictionary** (sampled fast path + hash fallback,
  `numeric_codec.c:161-301`) is intricate. It is justified by profiling intent
  but is the most complex code in the package for a bounded (≤64-value) win.
  Worth a comment block stating the invariant that the sampled path must produce
  byte-identical output to the hash path (the tests check round-trip, not path
  equivalence).

Otherwise the factoring is clean: the FASTRDS1/FASTRDS2 split is a sensible
speed/size boundary, per-type codecs are isolated behind small headers, and the
writer targets an abstract sink that would make the in-memory serializer (#2)
straightforward.

---

## Prioritized recommendations

**Tier 1 — required to legitimately claim "rds replacement":**
1. Decide and document the portability stance (#1). If replacement is the goal,
   add an opt-in portable mode (read-side byte-swap + XDR fallback). If not,
   restate the goal as "fast single-architecture cache" everywhere, including the
   DESCRIPTION.
2. Add in-memory `serialize`/`unserialize` and connection support (#2). This is
   the most-used `saveRDS` capability rdz lacks and is architecturally cheap.

**Tier 2 — biggest performance upside toward "beat qs2/fst everywhere":**
3. Per-element native fallback instead of whole-object (#3) — unlocks native
   speed on the mixed objects that currently get none.
4. Parallelize reads (#6) — the XOR blocks are already independent; add a block
   offset table so decode can fan out.
5. Collapse the two write traversals into one (#8) — pure latency win on the hot
   path, no format change.

**Tier 3 — depth and polish:**
6. Column/node-level write parallelism via an offset table (#7).
7. Profile and improve string encoding (#9).
8. Unify or cross-test the dual attribute paths (simplicity).
9. Optional payload checksum (#5).

**Testing gap noticed:** the suite is a single 1046-line `test-roundtrip.R` with
strong coverage of encodings and malformed input, but has **no test for the
architecture-rejection path** (mangling the header's endian/size bytes and
asserting the "incompatible architecture" error) and no cross-path equivalence
test for the sampled-vs-hash dictionary. Both are cheap to add and guard the two
riskiest correctness surfaces.

## Bottom line

Sound, careful, and fast — a genuinely good low-latency serializer. It is not
yet an `rds` replacement, and the gap is dominated by **portability** and
**connection/in-memory support**, not by codec quality. The performance lead
over qs2/fst is real on the tested shapes but rests on skipping compression
rather than on parallelism; as data and core counts grow, the unused read/write
parallelism (#6, #7) and the all-or-nothing fallback (#3) are where the lead is
most exposed. Address Tier 1 to earn the "replacement" claim; address Tier 2 to
keep the speed claim durable.
