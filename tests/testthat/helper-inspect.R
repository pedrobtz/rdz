# Helpers of test-inspect.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

inspect_fixture <- function() {
  list(
    sales = data.frame(
      day = as.Date("2026-01-01") + 0:199,
      amount = round(seq(1, 100, length.out = 200), 2),
      store = factor(rep(c("north", "south"), 100)),
      at = as.POSIXct("2026-01-01", tz = "Europe/Lisbon") + 1:200
    ),
    models = list(list(coef = c(a = 1.5, b = -2)), structure(matrix(1:6, 2), note = "fitted")),
    label = "Q1"
  )
}
