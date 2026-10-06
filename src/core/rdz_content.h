/*
 * rdz_content.h -- the content hash (container-format.md, "Content hash";
 * plan-c Stage M): XXH3-128 of a canonical stream of an object graph, the
 * same whether the writer feeds it while writing or rdz_hash() feeds it from
 * an object: every node in file order (its fields), its values (32-bit
 * integers and 64-bit double bits as little-endian bytes, each string as
 * its record digest), then every attribute entry.
 *
 * A string's record digest is XXH3-64 of its tag, length (u32 LE) and
 * bytes, so a writer that has deduplicated strings (a dictionary) feeds each
 * distinct one's digest once computed. Threads: one accumulator per thread.
 */
#ifndef RDZ_CONTENT_H
#define RDZ_CONTENT_H

#include <zufast/hash.h>

#include "rdz_graph.h"

typedef struct rdz_content_s {
    zuf_hasher h;
    uint8_t buf[8192]; /* fed in batches: the hasher's per-call cost */
    size_t at;
} rdz_content;

void rdz_content_begin(rdz_content *c);
/* a node's fields (its values follow) */
void rdz_content_node(rdz_content *c, const rdz_node *n);
/* n values of `width` (4 or 8) bytes, from native order */
void rdz_content_values(rdz_content *c, const void *values, size_t n, size_t width);
void rdz_content_string(rdz_content *c, const rdz_str *s);
void rdz_content_digest(rdz_content *c, uint64_t digest); /* a string's, computed before */
/* digests[ids[i]] for i in [0, n): a dictionary's elements */
void rdz_content_digests(rdz_content *c, const uint64_t *digests, const uint32_t *ids, size_t n);
void rdz_content_attributes(rdz_content *c, const rdz_attribute *a, size_t n);
void rdz_content_end(rdz_content *c, uint8_t out[16]);

uint64_t rdz_string_digest(const rdz_str *s);

#endif /* RDZ_CONTENT_H */
