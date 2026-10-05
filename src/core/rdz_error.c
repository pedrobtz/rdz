#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rdz_format.h"

void rdz_error_clear(rdz_error *e)
{
    e->code = RDZ_OK;
    e->message[0] = '\0';
}

static int rdz_set(rdz_error *e, rdz_code code, const char *fmt, ...)
{
    va_list ap;
    e->code = code;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof e->message, fmt, ap);
    va_end(ap);
    return 1;
}

int rdz_invalid(rdz_error *e, const char *what)
{
    return rdz_set(e, RDZ_E_INVALID, "invalid rdz file: %s", what);
}

int rdz_invalid_block(rdz_error *e, const char *fmt, uint32_t sequence)
{
    char what[160];
    snprintf(what, sizeof what, fmt, (unsigned long)sequence);
    return rdz_invalid(e, what);
}

int rdz_limit(rdz_error *e, const char *what)
{
    return rdz_set(e, RDZ_E_LIMIT, "invalid rdz file: %s exceeds its format limit", what);
}

int rdz_version_error(rdz_error *e, unsigned version)
{
    return rdz_set(e, RDZ_E_VERSION, "unsupported rdz format version %u", version);
}

int rdz_codec_error(rdz_error *e, unsigned id, unsigned version)
{
    return rdz_set(e, RDZ_E_CODEC, "unsupported rdz codec %u version %u", id, version);
}

int rdz_io_error(rdz_error *e, const char *reason)
{
    return rdz_set(e, RDZ_E_IO, "rdz file IO failed: %s", reason);
}

int rdz_io_errno(rdz_error *e, int errnum)
{
    return rdz_io_error(e, errnum ? strerror(errnum) : "unknown error");
}

int rdz_memory(rdz_error *e, const char *what)
{
    return rdz_set(e, RDZ_E_MEMORY, "rdz could not allocate memory for %s", what);
}

int rdz_add_u64(uint64_t a, uint64_t b, uint64_t *out, rdz_error *e)
{
    if (a > UINT64_MAX - b) return rdz_invalid(e, "offset arithmetic overflow");
    *out = a + b;
    return 0;
}
