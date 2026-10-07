# Files opened while `code` runs (a test-only counter in src/rdz_r.c), for
# test-read-request.R and test-attributes-request.R: each read opens its
# file once. Shared here because devtools::test(shuffle = TRUE) runs a test
# file's top-level code in any order.
opens_during <- function(code) {
  before <- .Call(rdz:::rdz_test_opens)
  force(code)
  .Call(rdz:::rdz_test_opens) - before
}
