/*
 * rdz_init.c -- routine registration for the C implementation and, while it
 * remains the oracle (plan-c.md section 2), the Rust one.
 *
 * configure builds the Rust static library and defines RDZ_HAVE_RUST when
 * cargo and rustc are available (and RDZ_RUST is not 0). Without them the
 * Rust routines are registered as stubs that raise an error, so the R code
 * and its registered symbols are the same in either build.
 */
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

SEXP rdz_c_info(SEXP path);
SEXP rdz_c_has_rust(void);
SEXP rdz_c_read(SEXP path, SEXP settings);
SEXP rdz_c_zstd_version(void);
SEXP rdz_c_try_write_native(SEXP x, SEXP path, SEXP strict, SEXP policy);
SEXP rdz_c_read_native_names(SEXP path);
SEXP rdz_c_logical_kernel(SEXP force_scalar);
SEXP rdz_c_root_length(SEXP x);
SEXP rdz_c_write_generic(SEXP x, SEXP synopsis, SEXP path, SEXP settings);
SEXP rdz_test_write_generic_unwind(SEXP x, SEXP path, SEXP blocks, SEXP settings);
SEXP rdz_test_records(void);
SEXP rdz_test_read_generic(SEXP path);
SEXP rdz_test_write_generic(SEXP payload, SEXP synopsis, SEXP path);

#ifdef RDZ_HAVE_RUST
SEXP savvy_rdz_file_info__impl(SEXP path);
SEXP savvy_rdz_read__impl(SEXP path);
SEXP savvy_rdz_read_native_names__impl(SEXP path);
SEXP savvy_rdz_root_length__impl(SEXP x);
SEXP savvy_rdz_try_write_native__impl(SEXP x, SEXP path, SEXP strict);
SEXP savvy_rdz_write_generic__impl(SEXP payload, SEXP synopsis, SEXP path);
#else
static void rdz_no_rust(void)
{
    Rf_error("This rdz was built without the Rust reference implementation, "
             "which writing and reading still need until the C port is complete.");
}
static SEXP savvy_rdz_file_info__impl(SEXP path) { (void)path; rdz_no_rust(); return R_NilValue; }
static SEXP savvy_rdz_read__impl(SEXP path) { (void)path; rdz_no_rust(); return R_NilValue; }
static SEXP savvy_rdz_read_native_names__impl(SEXP path) { (void)path; rdz_no_rust(); return R_NilValue; }
static SEXP savvy_rdz_root_length__impl(SEXP x) { (void)x; rdz_no_rust(); return R_NilValue; }
static SEXP savvy_rdz_try_write_native__impl(SEXP x, SEXP path, SEXP strict)
{
    (void)x; (void)path; (void)strict;
    rdz_no_rust();
    return R_NilValue;
}
static SEXP savvy_rdz_write_generic__impl(SEXP payload, SEXP synopsis, SEXP path)
{
    (void)payload; (void)synopsis; (void)path;
    rdz_no_rust();
    return R_NilValue;
}
#endif

static const R_CallMethodDef call_entries[] = {
    {"rdz_c_info", (DL_FUNC)&rdz_c_info, 1},
    {"rdz_c_has_rust", (DL_FUNC)&rdz_c_has_rust, 0},
    {"rdz_c_read", (DL_FUNC)&rdz_c_read, 2},
    {"rdz_c_zstd_version", (DL_FUNC)&rdz_c_zstd_version, 0},
    {"rdz_c_try_write_native", (DL_FUNC)&rdz_c_try_write_native, 4},
    {"rdz_c_read_native_names", (DL_FUNC)&rdz_c_read_native_names, 1},
    {"rdz_c_logical_kernel", (DL_FUNC)&rdz_c_logical_kernel, 1},
    {"rdz_c_root_length", (DL_FUNC)&rdz_c_root_length, 1},
    {"rdz_c_write_generic", (DL_FUNC)&rdz_c_write_generic, 4},
    {"rdz_test_write_generic_unwind", (DL_FUNC)&rdz_test_write_generic_unwind, 4},
    {"rdz_test_records", (DL_FUNC)&rdz_test_records, 0},
    {"rdz_test_read_generic", (DL_FUNC)&rdz_test_read_generic, 1},
    {"rdz_test_write_generic", (DL_FUNC)&rdz_test_write_generic, 3},
    {"savvy_rdz_file_info__impl", (DL_FUNC)&savvy_rdz_file_info__impl, 1},
    {"savvy_rdz_read__impl", (DL_FUNC)&savvy_rdz_read__impl, 1},
    {"savvy_rdz_read_native_names__impl", (DL_FUNC)&savvy_rdz_read_native_names__impl, 1},
    {"savvy_rdz_root_length__impl", (DL_FUNC)&savvy_rdz_root_length__impl, 1},
    {"savvy_rdz_try_write_native__impl", (DL_FUNC)&savvy_rdz_try_write_native__impl, 3},
    {"savvy_rdz_write_generic__impl", (DL_FUNC)&savvy_rdz_write_generic__impl, 3},
    {NULL, NULL, 0}};

/* Everything else is hidden ($(C_VISIBILITY) in Makevars), zstd included. */
void attribute_visible R_init_rdz(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, call_entries, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
}
