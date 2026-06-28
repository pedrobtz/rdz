#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

extern SEXP C_fastrds_save(SEXP, SEXP, SEXP, SEXP);
extern SEXP C_fastrds_read(SEXP);

static const R_CallMethodDef call_methods[] = {
    {"C_fastrds_save", (DL_FUNC) &C_fastrds_save, 4},
    {"C_fastrds_read", (DL_FUNC) &C_fastrds_read, 1},
    {NULL, NULL, 0}
};

void attribute_visible R_init_fastrds(DllInfo *dll) {
    R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
}
