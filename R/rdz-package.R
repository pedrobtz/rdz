#' @section Compatibility:
#' The file format is frozen as of rdz 0.1.0: every later rdz reads every file
#' that rdz 0.1.0 or later wrote, to the same value. A later writer may add
#' block encodings, attribute kinds or directory fields, each under an
#' identifier of its own, but never changes what an existing one means; a
#' reader refuses, rather than misreads, a file that uses one it does not
#' know. A version number in the file changes only if an existing meaning
#' ever does. The tests read a corpus of files written by rdz 0.1.0, which is
#' never rewritten. Files written by development builds before 0.1.0 (whose
#' writer [rdz_info()] reports as `"rdz 0.0.0 (development)"`, or does not
#' report) have no compatibility guarantee.
#'
#' @keywords internal
"_PACKAGE"

## usethis namespace: start
#' @useDynLib rdz, .registration = TRUE
## usethis namespace: end
NULL
