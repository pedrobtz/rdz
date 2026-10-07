/*
 * rdz_init.c -- routine registration. The Rust reference implementation,
 * the C port's oracle (plan-c.md section 2), was retired at Stage I.
 */
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

SEXP rdz_c_info(SEXP path);
SEXP rdz_c_read(SEXP path, SEXP settings, SEXP select, SEXP window);
SEXP rdz_c_attributes(SEXP path, SEXP settings, SEXP object, SEXP names, SEXP allow_full);
SEXP rdz_c_zstd_version(void);
SEXP rdz_c_try_write_native(SEXP x, SEXP path, SEXP strict, SEXP policy, SEXP settings,
                            SEXP metadata);
SEXP rdz_c_directory(SEXP path);
SEXP rdz_c_hash_native(SEXP x, SEXP strict);
SEXP rdz_c_hash_generic(SEXP x);
SEXP rdz_c_verify(SEXP path);
SEXP rdz_c_read_objects(SEXP path, SEXP ids, SEXP settings);
SEXP rdz_c_logical_kernel(SEXP force_scalar);
SEXP rdz_c_root_length(SEXP x);
SEXP rdz_c_write_generic(SEXP x, SEXP synopsis, SEXP path, SEXP settings, SEXP metadata);
SEXP rdz_test_write_generic_unwind(SEXP x, SEXP path, SEXP blocks, SEXP settings);
SEXP rdz_test_records(void);
SEXP rdz_test_opens(void);
SEXP rdz_test_read_generic(SEXP path);
SEXP rdz_test_write_generic(SEXP payload, SEXP synopsis, SEXP path);


static const R_CallMethodDef call_entries[] = {
    {"rdz_c_info", (DL_FUNC)&rdz_c_info, 1},
    {"rdz_c_read", (DL_FUNC)&rdz_c_read, 4},
    {"rdz_c_attributes", (DL_FUNC)&rdz_c_attributes, 5},
    {"rdz_c_zstd_version", (DL_FUNC)&rdz_c_zstd_version, 0},
    {"rdz_c_try_write_native", (DL_FUNC)&rdz_c_try_write_native, 6},
    {"rdz_c_directory", (DL_FUNC)&rdz_c_directory, 1},
    {"rdz_c_hash_native", (DL_FUNC)&rdz_c_hash_native, 2},
    {"rdz_c_hash_generic", (DL_FUNC)&rdz_c_hash_generic, 1},
    {"rdz_c_verify", (DL_FUNC)&rdz_c_verify, 1},
    {"rdz_c_read_objects", (DL_FUNC)&rdz_c_read_objects, 3},
    {"rdz_c_logical_kernel", (DL_FUNC)&rdz_c_logical_kernel, 1},
    {"rdz_c_root_length", (DL_FUNC)&rdz_c_root_length, 1},
    {"rdz_c_write_generic", (DL_FUNC)&rdz_c_write_generic, 5},
    {"rdz_test_write_generic_unwind", (DL_FUNC)&rdz_test_write_generic_unwind, 4},
    {"rdz_test_records", (DL_FUNC)&rdz_test_records, 0},
    {"rdz_test_opens", (DL_FUNC)&rdz_test_opens, 0},
    {"rdz_test_read_generic", (DL_FUNC)&rdz_test_read_generic, 1},
    {"rdz_test_write_generic", (DL_FUNC)&rdz_test_write_generic, 3},
    {NULL, NULL, 0}};

/* Everything else is hidden ($(C_VISIBILITY) in Makevars), zstd included. */
void attribute_visible R_init_rdz(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, call_entries, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
}
