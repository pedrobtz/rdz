# rdz: Versioned Serialization for R Objects

Writes and reads R objects using a versioned, block-structured container
format. The format preserves R serialization semantics while adding
explicit format identification, length validation, and integrity
checking.

## Compatibility

The file format is frozen as of rdz 0.1.0: every later rdz reads every
file that rdz 0.1.0 or later wrote, to the same value. A later writer
may add block encodings, attribute kinds or directory fields, each under
an identifier of its own, but never changes what an existing one means;
a reader refuses, rather than misreads, a file that uses one it does not
know. A version number in the file changes only if an existing meaning
ever does. The tests read a corpus of files written by rdz 0.1.0, which
is never rewritten. Files written by development builds before 0.1.0
(whose writer
[`rdz_info()`](https://pedrobtz.github.io/rdz/reference/rdz_info.md)
reports as `"rdz 0.0.0 (development)"`, or does not report) have no
compatibility guarantee.

## See also

Useful links:

- <https://pedrobtz.github.io/rdz/>

- <https://github.com/pedrobtz/rdz>

- Report bugs at <https://github.com/pedrobtz/rdz/issues>

## Author

**Maintainer**: Pedro Baltazar <pedrobtz@gmail.com> \[copyright holder\]

Authors:

- Pedro Baltazar <pedrobtz@gmail.com> \[copyright holder\]

Other contributors:

- Meta Platforms, Inc. and affiliates (Zstandard, bundled in
  src/vendor/zstd) \[copyright holder\]
