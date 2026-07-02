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
