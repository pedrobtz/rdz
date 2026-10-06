# Helpers of test-rows.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

rows_frame <- function(n) {
  i <- seq_len(n)
  data.frame(
    l = c(TRUE, FALSE, NA)[i %% 3L + 1L],
    int = i * 3L,
    dbl = round(i / 7, 2), # decimal (ALP) blocks when compressing
    full = i / 3, # full precision
    chr = c("alpha", "beta", NA, "gamma")[i %% 4L + 1L], # a dictionary
    uniq = sprintf("u%07d", i), # plain strings
    fct = factor(c("x", "y", "z"))[i %% 3L + 1L],
    day = as.Date("2020-01-01") + i %% 1000L,
    stringsAsFactors = FALSE
  )
}
