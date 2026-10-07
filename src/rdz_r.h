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
/* A request the file cannot serve (a `select` name it does not hold, `rows`
   past its length): class "rdz_failure", kind "request", `what` as its
   message and `data` (what R's message prints; it may be unprotected) in
   attribute "data". R's rdz_check() words it as R's argument checks did. */
SEXP rdz_request(const char *what, SEXP data);
/* rdz_info()'s fields of an open reader (it stays open); NULL and e when a
   native file's attribute names cannot be read. Unprotected. */
SEXP rdz_info_list(rdz_reader *r, rdz_error *e);
/* The root's type as rdz_info() reports it. */
const char *rdz_root_type_name(const rdz_reader *r);
/* Raises e as the classed condition rdz_check() makes, from inside a
   callback that cannot return a value (an R_inpstream). Does not return. */
void rdz_raise(const rdz_error *e);
/* The bytes of a one-element character path for the core. */
const char *rdz_path(SEXP path);

/* An external pointer owning a closed reader (protect it), so a longjmp
   frees the reader; rdz_reader_finalize() frees it at once. */
SEXP rdz_reader_handle(rdz_reader **out);
/* Opens a path, or a raw vector holding a file (the caller keeps it alive). */
int rdz_open_source(rdz_reader *r, SEXP src, rdz_error *e);

/* A content hash's text: the canonical XXH128 form, high then low u64 in
   hex (adapter/rdz_native_r.c). */
SEXP rdz_hash_text(const uint8_t digest[16]);
void rdz_reader_finalize(SEXP ptr);

#endif /* RDZ_R_H */
