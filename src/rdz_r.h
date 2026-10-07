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

/* The one place the three one-open reads' inputs meet (rdz_read_source()).
   An entry point starts from rdz_read_args_none() and sets only its own
   fields; a native reader reads only its own. */
typedef struct {
    SEXP select;   /* read_rdz(): R_NilValue, a character or double vector,
                      or FALSE for one of neither type */
    SEXP window;   /* read_rdz(): R_NilValue or c(lo, hi), the rows' span */
    SEXP steps;    /* rdz_attributes(): R_NilValue (the root), a list of
                      strings and doubles, or FALSE for a malformed path */
    SEXP names;    /* rdz_attributes(): R_NilValue (all) or a character vector */
    int recursive; /* rdz_schema(): every level, or down to depth 1 */
    double lo;     /* the one output: read_rdz()'s row the value starts after,
                      the window's start when windowed, else 0 */
} rdz_read_args;
rdz_read_args rdz_read_args_none(void);

/* A read of an open native file (or, for generic_open, of an open generic
   one): it takes the reader over. *failed: 0, 1 (e), or 2 (the result is a
   request failure, rdz_request()). */
typedef SEXP (*rdz_native_fn)(rdz_reader *opened, int threads, rdz_read_args *args,
                              rdz_error *e, int *failed);
/* One open of a file (a path or a raw vector) for an entry point that reads
   it once: a native file goes to native_read; a generic one is unserialized
   whole, or, with whole_generic 0, not read at all: generic_open's result
   from the open reader when it is given, else R_NilValue. The reader is
   closed once, there, on every path, and an R error meanwhile frees it
   through an external pointer. *native: whether the file was native.
   (adapter/rdz_generic.c) */
SEXP rdz_read_source(SEXP path, SEXP settings, rdz_native_fn native_read,
                     rdz_native_fn generic_open, rdz_read_args *args, int whole_generic,
                     int *native);
/* The native reads (adapter/rdz_native_r.c): read_rdz()'s select and
   window, rdz_attributes()' steps and names, rdz_schema()'s recursive. */
SEXP rdz_native_read_request(rdz_reader *opened, int threads, rdz_read_args *args,
                             rdz_error *e, int *failed);
SEXP rdz_native_read_attributes(rdz_reader *opened, int threads, rdz_read_args *args,
                                rdz_error *e, int *failed);
SEXP rdz_native_read_schema(rdz_reader *opened, int threads, rdz_read_args *args,
                            rdz_error *e, int *failed);

/* A content hash's text: the canonical XXH128 form, high then low u64 in
   hex (adapter/rdz_native_r.c). */
SEXP rdz_hash_text(const uint8_t digest[16]);
void rdz_reader_finalize(SEXP ptr);

#endif /* RDZ_R_H */
