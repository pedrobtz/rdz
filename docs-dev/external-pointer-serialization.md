# External Pointer Serialization

Reviewed: 2026-07-08

This note sketches how `rdz` could add Sakura-style support for objects that
contain external pointers, such as Arrow tables and torch tensors.

The goal is not to serialize pointer addresses. External pointers are process
local and cannot be made portable by copying their address bytes. The useful
contract is opt-in marshalling: package-specific functions convert an object to
a raw payload on write and rebuild a fresh live object on read.

## Sakura Precedent

Source checked: `RConsortium/sakura` at
`70860f75747c3201019e9060f38c3d7f22efc4fc`.

Sakura exposes three R-level pieces:

- `serial_config(class, sfunc, ufunc)` builds a hook table. Each class name has
  a serializer that returns raw bytes and an unserializer that consumes those
  bytes.
- `serialize(x, hook = cfg)` serializes with the hook table.
- `unserialize(raw, hook = cfg)` restores with the matching table.

The examples in Sakura use:

```r
cfg <- sakura::serial_config(
  "ArrowTabular",
  arrow::write_to_raw,
  function(x) arrow::read_ipc_stream(x, as_data_frame = FALSE)
)
```

and:

```r
cfg <- sakura::serial_config(
  "torch_tensor",
  torch::torch_serialize,
  torch::torch_load
)
```

The important implementation detail is in `src/core.c`: Sakura initializes R's
native serialization stream with custom reference hooks. During serialization,
the hook checks whether the object inherits from one of the configured classes.
If it matches, Sakura calls the configured serializer, writes the raw payload
into the same R serialization stream, and records which hook entry to use. On
read, the matching hook reads that payload and calls the configured
unserializer.

Sakura also registers C callables:

- `sakura_serialize_init`
- `sakura_unserialize_init`
- `sakura_serialize`
- `sakura_unserialize`

For `rdz`, the two stream-initialization callables are the relevant part
because `rdz` already has its own file writer and reader callbacks.

## Current rdz Shape

`rdz` currently has:

- A native codec for atomic vectors, strings, lists, matrices, data frames,
  tibbles, and top-level data tables.
- A whole-object R serialization fallback for unsupported objects.
- No hook argument on `write_rdz()`, `read_rdz()`, or `explain_rdz()`.
- An open roadmap item for per-node fallback via a `NODE_FALLBACK` native node.

Today, an unsupported child sends the entire object through R serialization. If
that fallback stream does not have Sakura-style hooks, pointer-backed objects
such as Arrow tables can deserialize with invalid null external pointers.

## Recommended API

Add a hook argument compatible with Sakura's configuration shape:

```r
rdz_serial_config <- function(class, sfunc, ufunc) {
  # Same validation and return shape as sakura::serial_config().
}

write_rdz(object, file, codec = c("auto", "native", "r"),
          preset = c("speed", "balanced"), hook = NULL)

read_rdz(file, hook = NULL)

explain_rdz(object, codec = c("auto", "native", "r"),
            preset = c("speed", "balanced"), hook = NULL)
```

`rdz_serial_config()` can be a thin compatibility helper. It should return the
same three-element structure as Sakura:

1. Character vector of class names.
2. List of serializers.
3. List of unserializers.

That lets users reuse existing Sakura examples with minimal translation. `rdz`
could also accept a value created by `sakura::serial_config()` directly.

## Implementation Plan

### 1. Add Hooked R Serialization Helpers

Factor the current fallback stream setup into helpers:

- `rdz_r_serialize(writer_t *writer, SEXP object, SEXP hook)`
- `rdz_r_unserialize(reader_t *reader, size_t nbytes, SEXP hook)`

When `hook == R_NilValue`, these keep using the current `R_InitOutPStream()` and
`R_InitInPStream()` calls.

When `hook != R_NilValue`, initialize the stream with Sakura-compatible
reference hooks. There are two viable ways to do this:

- Prefer using Sakura's registered C callables if `rdz` is willing to add an
  optional or hard dependency on `sakura`. `sakura_serialize_init` and
  `sakura_unserialize_init` accept caller-provided byte callbacks, which fits
  `rdz`'s `writer_t` and `reader_t` streams.
- Alternatively, implement the same refhook mechanism inside `rdz`, using
  Sakura as the executable reference. This avoids a runtime dependency but
  takes ownership of R serialization stream details that need R-devel testing.

The first version should use the Sakura callables if the project is comfortable
with the dependency. They are the cleanest way to avoid copying Sakura's stream
marker logic.

### 2. Support Hooks In Whole-Object Fallback

This is the smallest useful feature:

- Add `hook` to `write_rdz()` and pass it through `C_rdz_save`.
- Store it in `save_context_t`.
- In `save_body()`, when `context->codec == RDZ_CODEC_R`, call
  `rdz_r_serialize(..., hook)` instead of initializing a plain R stream.
- Add `hook` to `read_rdz()` and pass it to `C_rdz_read`.
- In `read_body()`, when `codec == RDZ_CODEC_R`, call
  `rdz_r_unserialize(..., hook)`.

This makes `write_rdz(list(arrow_table), file, hook = cfg)` work as soon as the
object chooses the R fallback path. It does not preserve native speed for the
supported siblings of that object.

### 3. Combine With Per-Node Fallback

The fuller `rdz` version should reuse the existing roadmap idea:

- Add `NODE_FALLBACK = 8` for unsupported subtrees in otherwise native files.
- In `codec = "auto"`, encode supported nodes natively and unsupported nodes as
  bounded R serialization payloads.
- In `codec = "native"`, keep the current strict contract and error at the
  first node that would need fallback.
- Decode `NODE_FALLBACK` by creating a length-bounded sub-reader and calling
  `rdz_r_unserialize(..., hook)`.

Use a new forward-incompatible signal whenever `NODE_FALLBACK` can appear:
either a new native format version, such as `FASTRDS3`, or the roadmap's flagged
codec byte convention. Do not silently write `NODE_FALLBACK` into files that
claim to be plain `FASTRDS1` or `FASTRDS2`.

This is the feature users will actually feel:

```r
cfg <- rdz_serial_config(
  "ArrowTabular",
  arrow::write_to_raw,
  function(x) arrow::read_ipc_stream(x, as_data_frame = FALSE)
)

x <- list(
  metadata = data.frame(id = 1:3),
  table = arrow::as_arrow_table(iris),
  values = runif(1e6)
)

write_rdz(x, "x.rdz", preset = "balanced", hook = cfg)
y <- read_rdz("x.rdz", hook = cfg)
```

With per-node fallback, `metadata` and `values` can stay native while `table`
uses the hooked R fallback stream.

### 4. Make `explain_rdz()` Honest

`explain_rdz()` should report hooked fallback nodes explicitly. Suggested
strategy strings:

- `R serialization stream (forced)`
- `R serialization stream (unsupported subtree)`
- `R serialization stream with refhooks (unsupported subtree)`

If a hook serializer fails or returns a non-raw value, `explain_rdz()` should
surface the same error as `write_rdz()`. Sakura treats a non-raw hook result as
no match and falls back to ordinary R behavior. `rdz` should instead prefer a
clear error once a class matched the hook table.

## Format And Semantics

The file should not imply that hooks are self-contained. A hooked fallback
payload is only readable when the caller supplies compatible unserializers and
the relevant packages are installed.

Recommended semantics:

- `hook = NULL` keeps current behavior.
- A non-null hook is only used for R serialization fallback payloads, not for
  ordinary native atomic/vector nodes.
- Hook serializers must return a raw vector.
- Hook unserializers receive that raw vector and return the rebuilt object.
- Hook order must be stable between write and read if the Sakura-compatible
  stream format is used.
- Reference sharing is preserved within one fallback payload because R's
  serializer handles it. Sharing across two fallback nodes, or between fallback
  and native nodes, is not preserved.
- Hooked reads are trusted operations. They execute user-supplied R functions
  and should be rejected by any future `read_rdz(trusted = FALSE)` mode.

Nice-to-have metadata for a later format version:

- Store the configured class names in a small file-level extension block.
- On read, if the file says hooks were used and `hook = NULL`, error before
  entering `R_Unserialize()`.
- If class names differ from the supplied hook table, error with the missing or
  mismatched names. This is friendlier than relying on an unserialize failure
  inside the R stream.

## Tests

Core tests should not require Arrow or torch:

- A mock class whose serializer is `base::serialize(x$value, NULL)` and whose
  unserializer rebuilds the class.
- A serializer that returns a non-raw value, expecting a clear error.
- A serializer that errors, expecting the original error context to be visible.
- `hook = NULL` preserves current fallback behavior.
- `codec = "native"` still errors for unsupported hookable objects.
- `codec = "r"` uses hooks even for otherwise natively supported containers.
- `explain_rdz()` reports hooked fallback bytes and strategies.
- Corrupt `NODE_FALLBACK` lengths cannot let `R_Unserialize()` read past the
  bounded node.

Optional tests can be skipped unless packages are installed:

- Arrow `ArrowTabular` round trip using `arrow::write_to_raw()` and
  `arrow::read_ipc_stream(..., as_data_frame = FALSE)`.
- torch tensor round trip using `torch::torch_serialize()` and
  `torch::torch_load()`.

## Open Decisions

- Whether `rdz` should import `sakura` and use its C callables, or implement the
  equivalent refhook stream locally.
- Whether to ship `rdz_serial_config()` or simply document that
  `sakura::serial_config()` is accepted.
- Whether hook metadata should be added immediately or deferred until after the
  first working implementation.
- Whether per-node fallback should happen before exposing hooks. The minimal
  whole-object version is smaller, but the per-node version is much more aligned
  with `rdz`'s performance goal.
