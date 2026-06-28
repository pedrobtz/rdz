#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

extern SEXP C_rdz_save(SEXP, SEXP, SEXP, SEXP);
extern SEXP C_rdz_read(SEXP);

static const R_CallMethodDef call_methods[] = {
    {"C_rdz_save", (DL_FUNC) &C_rdz_save, 4},
    {"C_rdz_read", (DL_FUNC) &C_rdz_read, 1},
    {NULL, NULL, 0}
};

void attribute_visible R_init_rdz(DllInfo *dll) {
    R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
}
