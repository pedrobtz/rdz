#include <R.h>
#include <Rinternals.h>

int rdz_logical_attribute_kind(SEXP value) {
  if (TYPEOF(value) != LGLSXP) {
    return -3;
  }
  if (ALTREP(value)) {
    return -2;
  }

  int has_names = 0;
  for (SEXP node = ATTRIB(value); node != R_NilValue; node = CDR(node)) {
    if (TAG(node) != R_NamesSymbol || has_names) {
      return -1;
    }
    SEXP names = CAR(node);
    if (TYPEOF(names) != STRSXP || XLENGTH(names) != XLENGTH(value)) {
      return -3;
    }
    has_names = 1;
  }
  return has_names;
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
