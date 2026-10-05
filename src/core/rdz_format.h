/*
 * rdz_format.h -- the wire constants of container version 3 and the core's
 * error type. The authority is .agents/container-format.md; every value here
 * is one of its table entries.
 *
 * The core (src/core/) never includes an R header: it compiles standalone
 * into the fuzz targets and the C test harness.
 */
#ifndef RDZ_FORMAT_H
#define RDZ_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define RDZ_FILE_MAGIC      "RDZ\x1a"
#define RDZ_BLOCK_MAGIC     "RBLK"
#define RDZ_DIRECTORY_MAGIC "RDIR"
#define RDZ_TRAILER_MAGIC   "RDZT"
#define RDZ_CLOSING_MAGIC   "ZEND"

#define RDZ_CONTAINER_VERSION 3u
#define RDZ_DIRECTORY_VERSION 1u

#define RDZ_HEADER_LEN           32u
#define RDZ_BLOCK_HEADER_LEN     48u
#define RDZ_DIRECTORY_HEADER_LEN 40u
#define RDZ_OBJECT_ENTRY_LEN     48u
#define RDZ_ATTRIBUTE_ENTRY_LEN  32u
#define RDZ_BLOCK_ENTRY_LEN      64u
#define RDZ_TRAILER_LEN          40u

#define RDZ_CODEC_R_SERIAL_V3   1u
#define RDZ_CODEC_NATIVE_V1     2u
#define RDZ_R_SERIAL_CODEC_VERSION 3u
#define RDZ_NATIVE_CODEC_VERSION   1u

#define RDZ_ENCODING_RAW                     0u
#define RDZ_ENCODING_LOGICAL_2BIT            1u
#define RDZ_ENCODING_STRING_PLAIN            2u
#define RDZ_ENCODING_LOGICAL_CONSTANT        3u
#define RDZ_ENCODING_LOGICAL_DENSE_PLANES    4u
#define RDZ_ENCODING_LOGICAL_SPARSE_PATCHES  5u
#define RDZ_ENCODING_LOGICAL_RUN_ENDS        6u
#define RDZ_ENCODING_LOGICAL_PERIODIC        7u
#define RDZ_ENCODING_STRING_DICT_ENTRIES     8u
#define RDZ_ENCODING_STRING_DICT_INDICES     9u

#define RDZ_COMPRESSION_NONE 0u
#define RDZ_COMPRESSION_ZSTD 1u /* one zstd frame (RFC 8878) */

#define RDZ_TYPE_NULL      0u
#define RDZ_TYPE_LOGICAL   1u
#define RDZ_TYPE_INTEGER   2u
#define RDZ_TYPE_DOUBLE    3u
#define RDZ_TYPE_CHARACTER 4u
#define RDZ_TYPE_FACTOR    5u
#define RDZ_TYPE_LIST      6u
#define RDZ_TYPE_DATA_FRAME 7u
#define RDZ_ROLE_ROOT            0u
#define RDZ_ROLE_ATTRIBUTE_NAME  1u
#define RDZ_ROLE_ATTRIBUTE_VALUE 2u
#define RDZ_ROLE_LEVELS          3u /* a factor's levels, its one child */
#define RDZ_ROLE_CHILD           4u /* an element of a list or a data frame's column */
#define RDZ_OBJECT_FLAG_ORDERED  1u /* a factor root: class c("ordered", "factor") */
#define RDZ_ATTRIBUTE_FLAG_NAMES     1u
#define RDZ_ATTRIBUTE_FLAG_ROW_NAMES 2u /* a data frame's explicit row names */
#define RDZ_ATTRIBUTE_FLAG_CLASS     4u /* a data frame's class */
#define RDZ_MAX_DEPTH 1000u /* nesting below the root */

/* Every flags word: the low half must be understood (a reader rejects an
   unknown bit there), the high half may be ignored (a reader drops it). */
#define RDZ_FLAGS32_REQUIRED 0x0000FFFFu
#define RDZ_FLAGS16_REQUIRED 0x00FFu

/* The file header's writer field (bytes 20 to 23): implementation, then the
   major, minor and patch version of its release, or for a development build
   of the release it follows. Readers never reject it. 0: not recorded. */
#define RDZ_WRITER_RDZ   1u    /* this package */
#define RDZ_WRITER_DEV   0x80u /* or-ed into the implementation byte */
#define RDZ_WRITER_MAJOR 0u    /* the package version; test-format.R checks it */
#define RDZ_WRITER_MINOR 0u
#define RDZ_WRITER_PATCH 0u
#define RDZ_WRITER_IS_DEV 1

/* Directory entries may be wider than these, never narrower; readers skip
   the extra bytes. */
#define RDZ_MAX_ENTRY_WIDTH 256u
#define RDZ_ROOT_PARENT_ID 0xFFFFFFFFu

#define RDZ_BLOCK_SIZE           ((uint32_t)1024 * 1024)
#define RDZ_LOGICAL_BLOCK_VALUES ((uint32_t)65536)
#define RDZ_MAX_BLOCK_SIZE       ((uint64_t)64 * 1024 * 1024)
#define RDZ_MAX_BLOCKS     1000000u
#define RDZ_MAX_OBJECTS    1000000u
#define RDZ_MAX_ATTRIBUTES 1000000u
#define RDZ_MAX_SYNOPSIS_LEN ((uint32_t)64 * 1024)

/* Codec record constants the container validates lengths against. */
#define RDZ_LOGICAL_CONSTANT_HEADER_LEN 4u
#define RDZ_LOGICAL_DENSE_HEADER_LEN    4u
#define RDZ_LOGICAL_SPARSE_HEADER_LEN   16u
#define RDZ_LOGICAL_RUN_HEADER_LEN      8u
#define RDZ_LOGICAL_RUN_RECORD_LEN      8u
#define RDZ_LOGICAL_PERIODIC_HEADER_LEN 8u
#define RDZ_LOGICAL_MAX_PERIOD          64u
#define RDZ_DICT_INDEX_HEADER_LEN       8u

/* Fuzz builds compare no checksum, so that the fuzzer reaches the structure
   the checksums guard (tools/run-fuzz). Never defined in the package. */
#ifdef RDZ_FUZZ_SKIP_CHECKSUMS
#define RDZ_CHECKSUM_DIFFERS(stored, computed) ((void)(stored), (void)(computed), 0)
#else
#define RDZ_CHECKSUM_DIFFERS(stored, computed) ((stored) != (computed))
#endif

/* The kinds of failure. Each formats its message as the Rust implementation
   did, so a test matching one matches the other. */
typedef enum {
    RDZ_OK = 0,
    RDZ_E_INVALID = 1, /* "invalid rdz file: <what>" */
    RDZ_E_LIMIT = 2,   /* "invalid rdz file: <what> exceeds its format limit" */
    RDZ_E_VERSION = 3, /* "unsupported rdz format version <n>" */
    RDZ_E_CODEC = 4,   /* "unsupported rdz codec <id> version <n>" */
    RDZ_E_IO = 5,      /* "rdz file IO failed: <reason>" */
    RDZ_E_MEMORY = 6,  /* "rdz could not allocate memory for <what>" */
    RDZ_E_UNSUPPORTED = 7 /* a value a native codec cannot write: automatic mode
                             falls back to R serialization, never an error */
} rdz_code;

typedef struct {
    rdz_code code;
    char message[240];
} rdz_error;

void rdz_error_clear(rdz_error *e);
int rdz_invalid(rdz_error *e, const char *what);
int rdz_invalid_block(rdz_error *e, const char *fmt, uint32_t sequence);
int rdz_limit(rdz_error *e, const char *what);
int rdz_version_error(rdz_error *e, unsigned version);
int rdz_codec_error(rdz_error *e, unsigned id, unsigned version);
int rdz_io_error(rdz_error *e, const char *reason);
int rdz_io_errno(rdz_error *e, int errnum);
int rdz_memory(rdz_error *e, const char *what);
/* "native serialization is not implemented for <what>; use `mode = "auto"`
   or `mode = "r"`" */
int rdz_unsupported(rdz_error *e, const char *what);

/* Checked arithmetic on file offsets: nonzero on overflow, which is
   "invalid rdz file: offset arithmetic overflow". */
int rdz_add_u64(uint64_t a, uint64_t b, uint64_t *out, rdz_error *e);

#endif /* RDZ_FORMAT_H */
