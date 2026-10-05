# The native codecs (logical vectors and their names) still go through the
# Rust reference implementation until plan-c Stage E; a build without cargo
# writes everything through the generic codec. Tests of native files call this.
skip_without_rust <- function() {
  testthat::skip_if_not(
    rdz:::rdz_has_rust(),
    "rdz was built without the Rust reference implementation"
  )
}
