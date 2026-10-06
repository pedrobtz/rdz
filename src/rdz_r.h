/*
 * rdz_r.h -- what the .Call layer (rdz_r.c) shares with the adapter.
 * R thread only.
 */
#ifndef RDZ_R_H
#define RDZ_R_H

#include <R.h>
#include <Rinternals.h>

#include "core/rdz_container.h"

/* The failure value an entry point returns: the message, class
   "rdz_failure", the kind in attribute "kind". R's rdz_check() raises it. */
SEXP rdz_failure(const rdz_error *e);
/* Raises e as the classed condition rdz_check() makes, from inside a
   callback that cannot return a value (an R_inpstream). Does not return. */
void rdz_raise(const rdz_error *e);
/* The bytes of a one-element character path for the core. */
const char *rdz_path(SEXP path);

/* An external pointer owning a closed reader (protect it), so a longjmp
   frees the reader; rdz_reader_finalize() frees it at once. */
SEXP rdz_reader_handle(rdz_reader **out);
void rdz_reader_finalize(SEXP ptr);

#endif /* RDZ_R_H */
