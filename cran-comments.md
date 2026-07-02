## R CMD check results

0 errors | 0 warnings | 1 note

* This is a new release.

## Test environments

- local macOS, R 4.5.2
- GitHub Actions (R-CMD-check): Ubuntu, macOS, Windows on R release
- Additional native checks on GitHub Actions: ASAN/UBSAN sanitizers, valgrind,
  LTO, gctorture, and rchk

## Bundled code

* This package bundles the LZ4 compression library (v1.10.0) under
  src/vendor/lz4/. Its author, Yann Collet, is listed as a copyright holder in
  Authors@R, and its BSD 2-Clause license is reproduced in inst/COPYRIGHTS.
