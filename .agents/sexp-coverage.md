# R SEXP Coverage Matrix

> **Historical.** This document records the type registry as the Rust implementation saw it. What is native and what goes generic is now stated in `?write_rdz` ("Native and generic") and enforced by the planner in `src/adapter/rdz_native_r.c`; ALTREP vectors are materialised and written natively, not only under an explicit policy; rdz takes no refhook. Where it disagrees with the code, the code and [architecture.md](architecture.md) are right.

## Purpose and policy

This is the normative registry for how RDZ handles every R `SEXPTYPE`. The type
list and numeric codes follow R's public `Rinternals.h` and the
[R Internals SEXPTYPE table](https://cran.r-project.org/doc/manuals/r-devel/R-ints.html#SEXPTYPEs).
The numeric values matter because R stores them in serialized objects; codes 11
and 12 are retired and cannot simply be reused.

The matrix was checked against the installed R 4.5.2 public header. Re-audit it
when the package's minimum R version, Savvy version, or supported R major/minor
range changes.

RDZ has two payload codecs:

- the dedicated native codec accelerates common vectors and compositions;
- the generic codec stores one whole-root base-R serialization stream.

Therefore “generic fallback” still means supported by the public RDZ API. Strict
native mode exposes the boundary of the dedicated codec. Automatic mode uses the
native codec only if the complete root is eligible; otherwise it writes the
complete root with base R. Never serialize an unsupported child as an independent
island inside a native graph.

Status vocabulary:

- **Native phase N**: scheduled dedicated encoding in the current roadmap.
- **Native foundation**: part of the native structural format from its first
  vertical slice.
- **Native candidate**: potentially useful later, but not committed.
- **Generic**: whole-root R serialization is the intended handling.
- **Internal/reject**: not a valid user object; reject if seen as a native value or
  file tag.

## Complete SEXPTYPE table

| Code | `SEXPTYPE` | R meaning | Automatic-mode handling | Dedicated native intention |
|---:|---|---|---|---|
| 0 | `NILSXP` | `NULL` | Native when inside an eligible root | Native foundation: tag only |
| 1 | `SYMSXP` | Symbol/name | Whole-root generic | Candidate with language objects; preserve print name and symbol semantics |
| 2 | `LISTSXP` | Pairlist/dotted-pair list | Whole-root generic | Candidate after native lists/references; requires `CAR`, `CDR`, `TAG`, attributes, and cycle limits |
| 3 | `CLOSXP` | R closure | Whole-root generic | No early native codec; formals, body, and environment make this a graph object |
| 4 | `ENVSXP` | Environment | Whole-root generic | Graph-codec candidate, explicitly not encoded as an ordinary list |
| 5 | `PROMSXP` | Promise | Whole-root generic | No early native codec; must preserve expression, value state, and evaluation environment without forcing it |
| 6 | `LANGSXP` | Call/language object | Whole-root generic | Candidate only with symbols and tagged pairlists |
| 7 | `SPECIALSXP` | Special primitive function | Whole-root generic | No native codec planned; let R restore its runtime identity |
| 8 | `BUILTINSXP` | Builtin primitive function | Whole-root generic | No native codec planned; let R restore its runtime identity |
| 9 | `CHARSXP` | Internal scalar string | Native only as an element of eligible `STRSXP`; unusual root falls back | Phase 4 character component, not a public standalone vector tag |
| 10 | `LGLSXP` | Logical vector | Native when class/attributes/ALTREP are supported | Native Phase 1 |
| 11 | retired | Historical internal factor | Not a valid current object | Reserved forever; reject in native files |
| 12 | retired | Historical internal ordered factor | Not a valid current object | Reserved forever; reject in native files |
| 13 | `INTSXP` | Integer vector | Native when eligible; otherwise whole-root generic | Native Phase 2; factors specialize this storage in Phase 5 |
| 14 | `REALSXP` | Numeric/double vector | Native when eligible; otherwise whole-root generic | Native Phase 3 |
| 15 | `CPLXSXP` | Complex vector | Whole-root generic | Later native candidate using exact pairs of double bit patterns |
| 16 | `STRSXP` | Character vector of `CHARSXP` elements | Native when eligible; otherwise whole-root generic | Native Phase 4 with exact NA, bytes, and encoding tags |
| 17 | `DOTSXP` | `...` pairlist, normally promises | Whole-root generic | No separate early codec; tied to promises and language semantics |
| 18 | `ANYSXP` | Type-matching placeholder | No actual objects of this type | Internal/reject |
| 19 | `VECSXP` | Generic vector/R list | Native only when the entire recursive graph and metadata are eligible | Native Phase 6; data frames specialize this storage in Phase 7 |
| 20 | `EXPRSXP` | Expression vector | Whole-root generic | Later candidate after native language objects; vector layout alone is insufficient |
| 21 | `BCODESXP` | Compiled R byte code | Whole-root generic | No native codec planned; implementation/runtime coupled |
| 22 | `EXTPTRSXP` | External pointer | Whole-root generic with base-R caveats and optional `refhook`; a registered transient attribute may be omitted by its owning native class adapter | No generic native representation of an external resource |
| 23 | `WEAKREFSXP` | Weak reference | Whole-root generic with base-R caveats and optional `refhook` | No native codec planned without a complete reference/finalizer model |
| 24 | `RAWSXP` | Raw byte vector | Whole-root generic initially | Straightforward later native candidate |
| 25 | `OBJSXP` / `S4SXP` | S4 or other object without a basic underlying type | Whole-root generic | No early native codec; class/slot semantics remain with R serialization |
| 26–29 | reserved | Currently unassigned | Not valid current object types | Reserve and reject |
| 30 | `NEWSXP` | Internal fresh GC node marker | Never a serializable user value | Internal/reject |
| 31 | `FREESXP` | Internal released GC node marker | Never a serializable user value | Internal/reject |
| 99 | `FUNSXP` | Pseudo-type grouping closures/builtins/specials in some C switches | No actual `TYPEOF()` value stored in a normal SEXP | Dispatch convenience only; never an RDZ tag |

The public native tag space must not reuse R's retired/reserved numbers merely
because RDZ uses its own tags. Keeping the conceptual distinction makes corrupt
input checks and future mapping easier to audit.

## Environments are graph containers, not lists

An environment is superficially list-like because it contains named bindings to
R values. The similarity is useful for traversal: visit binding names and values
recursively, apply the object/reference table, and schedule supported leaf vectors
through the same block encoders.

Its semantics are materially different. R describes an environment as a frame
plus an enclosing environment, and internally the frame may be hashed. Environments
also have reference rather than copy-on-modify semantics. See
[R's environment documentation](https://stat.ethz.ch/R-manual/R-devel/library/base/help/environment.html)
and [R Internals](https://cran.r-project.org/doc/manuals/r-devel/R-ints.html#Environments-and-variable-lookup).
A future native environment codec would need to define and test:

- object identity and repeated references;
- allocation and object-table registration before filling bindings;
- enclosure links and environment cycles through bound values;
- ordinary, locked, active, and delayed bindings;
- locked environments and bindings;
- empty, global, base, package, and namespace environment identities;
- closures and promises pointing back to the environment;
- deterministic binding serialization without treating hash-table order as
  semantics;
- `refhook` behavior and external-resource bindings.

Encoding an environment as a named list would lose at least identity, mutability,
the enclosure, special bindings, and cycles. The initial design therefore uses
one whole-root R serialization stream whenever an `ENVSXP` occurs. Native
environment support is a later graph-codec project that may reuse list traversal
and leaf codecs but needs a distinct physical record and decoder lifecycle.

## Class, attribute, and representation overlays

`SEXPTYPE` alone does not determine native eligibility. Before storage dispatch,
the adapter must check class, attributes, object/S4 state, and ALTREP state.
Important overlays include:

| R-level object | Underlying type commonly seen | Initial handling |
|---|---|---|
| Factor/ordered factor | `INTSXP` plus `levels`/`class` | Native Phase 5 |
| Data frame | `VECSXP` plus `names`, `row.names`, `class` | Native Phase 7 |
| `data.table` | Data frame plus class/package metadata and `.internal.selfref` | Native data-frame codec with a `data.table` overlay: preserve `c("data.table", "data.frame")` and defined table metadata; omit only the registered transient `.internal.selfref`; never coerce to a base data frame |
| Matrix/array | Atomic or list vector plus `dim`/`dimnames` | Whole-root generic until native dimension attributes are defined |
| Date/POSIXct/difftime and similar S3 vectors | Usually `REALSXP` or `INTSXP` plus class metadata | Whole-root generic until each semantic class is defined |
| Named ordinary vector | Atomic vector plus `names` | Native with the corresponding vector phase |
| S3 object | Any basic type plus class/attributes | Native only for an explicitly supported class; otherwise whole-root generic |
| S4 object extending a basic type | Basic storage type plus S4/class state | Whole-root generic initially |
| S7 or future object systems | Depends on actual runtime representation | Inspect class/type, then whole-root generic unless explicitly supported |
| ALTREP vector | Same logical vector `SEXPTYPE` plus ALTREP state | Materialize only under an explicit native policy; otherwise whole-root generic |
| Long vector | Same vector `SEXPTYPE` with long length | Native only when length/offset arithmetic and tests support it |

An unknown class or attribute is the distinguished unsupported-native outcome,
not permission to strip metadata and encode the underlying bytes. The narrow
exception is an attribute named in the external-pointer metadata registry for an
explicitly supported owning class. The initial entry is
`data.table::.internal.selfref`; see
[architecture.md](architecture.md#external-pointer-metadata). It does not permit
dropping arbitrary external-pointer attributes.

Attributes are recursively typed R values. Native eligibility therefore includes
every attribute name and value, not just the object's storage `SEXPTYPE`. If any
attribute is outside the owning codec's explicit contract, automatic mode must
fall back for the unchanged complete root. This preserves custom attributes and
their graph relationships even before RDZ has a general native attribute map.

## Dispatch rules

The automatic writer applies these rules to the complete root:

1. Check mode and `refhook`; forced R mode or a `refhook` selects generic
   serialization immediately.
2. Check supported semantic classes before `TYPEOF()` storage dispatch.
3. Traverse native composite candidates with a capability/reference table.
4. If the complete graph is eligible, write `NATIVE_V1`.
5. On the distinguished unsupported-native outcome, discard temporary native
   output and write the original root once as `R_SERIAL_V3`.
6. Do not fallback on IO errors, invalid arguments, allocation failures, or
   internal bugs.

The reader does not guess. It selects the decoder from the payload codec ID in
the validated RDZ container.

## Coverage validation

For every constructible public type in the table:

- automatic mode must round-trip through either the expected native or generic
  payload;
- forced generic mode must match base R serialization behavior;
- strict native mode must either round-trip or return the documented
  unsupported-native error;
- reference objects use graph and behavior assertions rather than original
  pointer identity;
- external pointers and weak references test and document base R/refhook caveats;
- registered transient external-pointer attributes test both their omission and
  the restored object's content and usable behavior;
- invalid internal, retired, pseudo, and reserved type tags are rejected by pure
  codec tests.

Whenever a type moves from generic-only to native, update this table, the roadmap,
the compatibility fixtures, and the benchmark matrix in the same change.

## Related documents

- [Architecture](architecture.md)
- [Implementation roadmap](roadmap.md)
- [Validation and benchmarking](validation.md)
- [Performance design](performance.md)
- [Cross-OS portability](portability.md)
- [Encoding and format research](encoding-research.md)
- [Direct R competitor research](research.md)
