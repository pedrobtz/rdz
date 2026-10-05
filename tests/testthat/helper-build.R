# Writing and reading still go through the Rust reference implementation
# until the C port replaces them (plan-c.md); a build without cargo has only
# the C container reader. Files that write or read call this first.
skip_without_rust <- function() {
  testthat::skip_if_not(
    rdz:::rdz_has_rust(),
    "rdz was built without the Rust reference implementation"
  )
}
