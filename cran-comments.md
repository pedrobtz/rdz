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
  pragmas, in the amalgamation generated from its unmodified sources (below);
  editing them would make the vendored library differ from the release its
  checksums pin.

## Test environments

In GitHub Actions, through R CMD check --as-cran:

* Linux: R-devel with clang 23 and with GCC 16 (R-hub's CRAN-like images,
  compiling with CRAN's flags); R-release and R-oldrel-1 on Ubuntu 24.04.
* macOS (arm64): R-release.
* Windows (GitHub's windows-latest): R-devel, R-release.
* Address and undefined-behaviour sanitizers, valgrind, LTO, rchk and
  gctorture, with the package's own tests.

Also, weekly and before release, R CMD check (without --as-cran) with each
distribution's own R: Debian i386 and Debian s390x (big-endian), R 4.2.2;
Alpine (musl), R 4.4.

## Bundled code

src/vendor/zstd is generated from the unmodified Zstandard 1.5.7 sources by
the release's own amalgamation script (BSD-3-Clause). Its copyright holder is
listed in Authors@R, and inst/COPYRIGHTS gives its licence in full and its
provenance. None of its symbols is exported.

## Threads

Compression and decompression run on a pool of POSIX threads (winpthreads
on Windows). One thread is the default (`options(rdz.threads)`); worker
threads never call R. The tests use at most two threads unless NOT_CRAN is
"true".

## Method references

There are no published references describing the methods in this package.
