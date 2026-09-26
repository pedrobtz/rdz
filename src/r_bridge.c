#include <R.h>
#include <Rinternals.h>
#include <Rversion.h>

// R 4.6 removed ATTRIB() from the headers; R_getAttribCount() replaces it.
static R_xlen_t rdz_attribute_count(SEXP value) {
#if R_VERSION >= R_Version(4, 6, 0)
  return R_getAttribCount(value);
#else
  R_xlen_t count = 0;
  for (SEXP node = ATTRIB(value); node != R_NilValue; node = CDR(node)) {
    count++;
  }
  return count;
#endif
}

int rdz_logical_attribute_kind(SEXP value) {
  if (TYPEOF(value) != LGLSXP) {
    return -3;
  }
  if (ALTREP(value)) {
    return -2;
  }

  R_xlen_t count = rdz_attribute_count(value);
  if (count == 0) {
    return 0;
  }
  // For an atomic vector getAttrib() returns the stored names without
  // allocating, so no PROTECT is needed.
  SEXP names = Rf_getAttrib(value, R_NamesSymbol);
  if (count != 1 || names == R_NilValue) {
    return -1;
  }
  if (TYPEOF(names) != STRSXP || XLENGTH(names) != XLENGTH(value)) {
    return -3;
  }
  return 1;
}

int rdz_is_altrep(SEXP value) {
  return ALTREP(value);
}

int rdz_charsxp_length(SEXP value) {
  return LENGTH(value);
}

int rdz_charsxp_encoding(SEXP value) {
  return Rf_getCharCE(value);
}
