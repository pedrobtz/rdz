# Helpers of test-hash.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

hash_cases <- function() {
  list(
    logical = c(TRUE, NA, FALSE),
    integer = c(1L, NA, -5L),
    double = c(1.5, NA, -2.25),
    character = c("a", NA, "été"),
    factor = factor(c("lo", "hi", NA)),
    frame = data.frame(d = as.Date("2026-10-06") + 0:2, x = c(0.5, 1, 1.5)),
    nested = list(a = 1:3, b = list(c = "x", d = NULL), m = matrix(1:4, 2)),
    generic = list(f = quote(g(x)), e = 1i),
    empty = list()
  )
}
