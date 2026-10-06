# Helpers of test-native.R, shared at load time (devtools::test(shuffle = TRUE)
# runs a test file's top-level code in any order).

with_dictionary_policy <- function(policy, code) {
  old <- Sys.getenv("RDZ_STRING_DICT", unset = NA)
  Sys.setenv(RDZ_STRING_DICT = policy)
  on.exit(if (is.na(old)) Sys.unsetenv("RDZ_STRING_DICT") else Sys.setenv(RDZ_STRING_DICT = old))
  force(code)
}
native_specs <- function() {
  manifest <- utils::read.delim(
    file.path(rust_fixture_dir(), "manifest.tsv"),
    colClasses = "character", quote = ""
  )
  Filter(function(spec) manifest$codec[manifest$name == spec$name] == "native_v1",
         rust_fixture_specs())
}
