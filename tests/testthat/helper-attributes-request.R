# The value test-attributes-request.R reads attributes from: a frame with
# stored row names and a general attribute, nested lists (one unnamed), and
# a large attribute stored twice (the second as a reference). Shared at
# load time (devtools::test(shuffle = TRUE) reorders top-level code).
attribute_value <- function() {
  df <- data.frame(id = 1:3, f = factor(c("a", "b", "a")))
  rownames(df) <- c("x", "y", "z")
  attr(df, "meta") <- list(k = 1)
  v <- seq_len(5000) + 0.5
  x <- list(sales = df, models = list(m1 = 1, m2 = structure(1:3, foo = "bar")), u = list(1, 2))
  attr(x, "big") <- v
  attr(x, "big2") <- v # stored as a reference to "big"
  x
}
