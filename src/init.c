
// clang-format sorts includes unless SortIncludes: Never. However, the ordering
// does matter here. So, we need to disable clang-format for safety.

// clang-format off
#include <stdint.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>
// clang-format on

#include "rust/api.h"

static uintptr_t TAGGED_POINTER_MASK = (uintptr_t)1;

SEXP handle_result(SEXP res_) {
    uintptr_t res = (uintptr_t)res_;

    // An error is indicated by tag.
    if ((res & TAGGED_POINTER_MASK) == 1) {
        // Remove tag
        SEXP res_aligned = (SEXP)(res & ~TAGGED_POINTER_MASK);

        // Currently, there are two types of error cases:
        //
        //   1. Error from Rust code
        //   2. Error from R's C API, which is caught by R_UnwindProtect()
        //
        if (TYPEOF(res_aligned) == CHARSXP) {
            // In case 1, the result is an error message that can be passed to
            // Rf_errorcall() directly.
            Rf_errorcall(R_NilValue, "%s", CHAR(res_aligned));
        } else {
            // In case 2, the result is the token to restart the
            // cleanup process on R's side.
            R_ContinueUnwind(res_aligned);
        }
    }

    return (SEXP)res;
}

SEXP savvy_rdz_file_info__impl(SEXP c_arg__path) {
    SEXP res = savvy_rdz_file_info__ffi(c_arg__path);
    return handle_result(res);
}

SEXP savvy_rdz_read__impl(SEXP c_arg__path) {
    SEXP res = savvy_rdz_read__ffi(c_arg__path);
    return handle_result(res);
}

SEXP savvy_rdz_read_native_names__impl(SEXP c_arg__path) {
    SEXP res = savvy_rdz_read_native_names__ffi(c_arg__path);
    return handle_result(res);
}

SEXP savvy_rdz_root_length__impl(SEXP c_arg__x) {
    SEXP res = savvy_rdz_root_length__ffi(c_arg__x);
    return handle_result(res);
}

SEXP savvy_rdz_try_write_native__impl(SEXP c_arg__x, SEXP c_arg__path, SEXP c_arg__strict) {
    SEXP res = savvy_rdz_try_write_native__ffi(c_arg__x, c_arg__path, c_arg__strict);
    return handle_result(res);
}

SEXP savvy_rdz_write_generic__impl(SEXP c_arg__payload, SEXP c_arg__synopsis, SEXP c_arg__path) {
    SEXP res = savvy_rdz_write_generic__ffi(c_arg__payload, c_arg__synopsis, c_arg__path);
    return handle_result(res);
}


static const R_CallMethodDef CallEntries[] = {
    {"savvy_rdz_file_info__impl", (DL_FUNC) &savvy_rdz_file_info__impl, 1},
    {"savvy_rdz_read__impl", (DL_FUNC) &savvy_rdz_read__impl, 1},
    {"savvy_rdz_read_native_names__impl", (DL_FUNC) &savvy_rdz_read_native_names__impl, 1},
    {"savvy_rdz_root_length__impl", (DL_FUNC) &savvy_rdz_root_length__impl, 1},
    {"savvy_rdz_try_write_native__impl", (DL_FUNC) &savvy_rdz_try_write_native__impl, 3},
    {"savvy_rdz_write_generic__impl", (DL_FUNC) &savvy_rdz_write_generic__impl, 3},
    {NULL, NULL, 0}
};

void R_init_rdz(DllInfo *dll) {
    R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);

    // Functions for initialization, if any.

}
