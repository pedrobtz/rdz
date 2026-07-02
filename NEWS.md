# rdz 0.0.1

- Initial release.
- `write_rdz()` and `read_rdz()` serialize R objects with a low-latency native
  codec for atomic vectors, strings, lists, matrices, and data frames, falling
  back to R's general serialization for unsupported objects.
- The `"balanced"` preset adds size-reducing transforms (logical bit-packing,
  frame-of-reference integers, numeric dictionaries, constant-delta sequences,
  and 'LZ4'-compressed blocks) that are selected only when they demonstrably
  reduce size.
- `explain_rdz()` reports the codec and per-node strategy that would be used.
- `rdz_threads()` reports the automatic worker-thread count used by rdz,
  respecting process affinity and cgroup CPU quotas on Linux.
- Top-level `data.table` objects use the native codec while preserving class,
  columns, and key metadata; runtime self-reference state is reconstructed on
  read when data.table is installed.
- Integer and double ALTREP vectors use bounded region reads instead of being
  materialized or serialized by R. The balanced preset stores exact
  constant-delta sequences compactly; the speed preset and irregular balanced
  vectors retain direct layouts.
- Numeric sequence detection now verifies the second reconstructed value
  bit-for-bit, preventing cancellation from selecting an inexact encoding.
- Native serialization preserves arbitrary attributes on R 4.1 and later;
  R 4.1--4.5 use the public base `attributes()` interface while R 4.6 and later
  use the C attribute-iteration API.
