/*
 * rdz's input to zstd's amalgamation script (tools/vendor/fetch): upstream's
 * build/single_file_libs/zstd-in.c at the pinned release, less two things.
 * ZSTD_MULTITHREAD is not defined, because rdz owns the one thread pool and
 * its workers call single-threaded zstd (performance.md, "Threading
 * policy"); and the dictionary builder is not included, because rdz builds
 * no dictionaries. Everything else is upstream's, in upstream's order.
 */
#define DEBUGLEVEL 0
#define MEM_MODULE
#undef  XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#undef  XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef  XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_TRACE 0
#define ZSTD_DISABLE_ASM 1

#define ZSTD_DEPS_NEED_MALLOC
#define ZSTD_DEPS_NEED_MATH64
#include "common/zstd_deps.h"

#include "common/debug.c"
#include "common/entropy_common.c"
#include "common/error_private.c"
#include "common/fse_decompress.c"
#include "common/threading.c"
#include "common/pool.c"
#include "common/zstd_common.c"

#include "compress/fse_compress.c"
#include "compress/hist.c"
#include "compress/huf_compress.c"
#include "compress/zstd_compress_literals.c"
#include "compress/zstd_compress_sequences.c"
#include "compress/zstd_compress_superblock.c"
#include "compress/zstd_preSplit.c"
#include "compress/zstd_compress.c"
#include "compress/zstd_double_fast.c"
#include "compress/zstd_fast.c"
#include "compress/zstd_lazy.c"
#include "compress/zstd_ldm.c"
#include "compress/zstd_opt.c"

#include "decompress/huf_decompress.c"
#include "decompress/zstd_ddict.c"
#include "decompress/zstd_decompress.c"
#include "decompress/zstd_decompress_block.c"
