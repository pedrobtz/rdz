## Submission

This is a new submission. rdz writes and reads R objects in a versioned,
checksummed block container: common objects natively, everything else through
R's own serialization, streamed.

It links against zubin and zufast (LinkingTo), by the same maintainer, which
are submitted to CRAN first; this submission follows their acceptance.

## R CMD check results

0 errors | 0 warnings | 2 notes

* New submission.
* Files which contain pragma(s) suppressing diagnostics:
  src/vendor/zstd/zstd.c and zstd.h. These are the Zstandard release's own
  pragmas, in its unmodified amalgamation (below); editing them would make
  the vendored library differ from the release its checksums pin.

## Test environments

All in GitHub Actions, through R CMD check --as-cran:

* Linux (Ubuntu 24.04): R-devel, R-release, R-oldrel-1; clang and gcc.
* macOS (arm64): R-release.
* Windows (Server 2022): R-devel, R-release.
* Debian i386, Alpine (musl) and Debian s390x (big-endian): R-release.
* Address and undefined-behaviour sanitizers, valgrind, LTO, rchk and
  gctorture, with the package's own tests.

## Bundled code

src/vendor/zstd is the Zstandard 1.5.7 release's single-file amalgamation,
unmodified (BSD-3-Clause). Its copyright holder is listed in Authors@R and in
inst/COPYRIGHTS, with its provenance and the checksums tools/vendor/verify
checks. None of its symbols is exported.

## Threads

Compression and decompression run on a pool of POSIX threads (winpthreads
on Windows). One thread is the default (`options(rdz.threads)`); worker
threads never call R.

## Method references

There are no published references describing the methods in this package.
