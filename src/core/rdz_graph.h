/*
 * rdz_graph.h -- native object graphs: a root vector, factor, list or data
 * frame, its attributes, and (for lists and data frames) its children,
 * written and read without recursion (plan-c Stage H; container-format.md,
 * "Native object graphs").
 *
 * The caller numbers the nodes so that every node's levels and attribute
 * objects, and its children, come after it, the children contiguous (a
 * breadth-first order does this). The writer emits each node's blocks in
 * node order through one pipeline, so every column of a data frame is
 * compressed in parallel; the reader streams every block once through one
 * pipeline into each object's destination.
 *
 * Threads: the caller's thread only; workers see only pipeline slots.
 */
#ifndef RDZ_GRAPH_H
#define RDZ_GRAPH_H

#include "rdz_vector.h"

typedef struct {
    uint16_t type;                 /* RDZ_TYPE_* */
    uint16_t role;                 /* RDZ_ROLE_* */
    uint32_t flags;                /* RDZ_OBJECT_FLAG_* */
    uint32_t parent;               /* RDZ_ROOT_PARENT_ID for the root */
    uint64_t length;               /* values; a list: children; a data frame: rows */
    uint32_t first_child, child_count;
    uint32_t first_attribute, attribute_count;
    const void *values;            /* logical, integer, factor codes: int32; double */
    const rdz_str_source *strings; /* character */
} rdz_node;

int rdz_graph_write(rdz_vec *v, const char *path, const rdz_node *nodes, uint32_t nnodes,
                    const rdz_attribute *attributes, uint32_t nattributes, int policy,
                    int level, int threads, rdz_tick_fn tick, void *tick_ctx, rdz_error *e);

/* Where the reader puts each object's values. */
typedef struct {
    void *ctx;
    /* the int32 or double array for a logical, integer, factor or double
       object */
    void *(*values)(void *ctx, uint32_t object);
    /* the sink for a character object */
    const rdz_names_sink *(*strings)(void *ctx, uint32_t object);
} rdz_graph_sinks;

/* Reads every block of an open native file, in order, into the sinks. */
int rdz_graph_read(rdz_vec *v, rdz_reader *r, const rdz_graph_sinks *sinks, int threads,
                   rdz_tick_fn tick, void *tick_ctx, rdz_error *e);
/* The same for the objects whose `want` byte is set (r->nobjects of them):
   the others' blocks are neither read nor decoded, and their sinks never
   called. NULL wants every object. */
int rdz_graph_read_some(rdz_vec *v, rdz_reader *r, const rdz_graph_sinks *sinks,
                        const uint8_t *want, int threads, rdz_tick_fn tick, void *tick_ctx,
                        rdz_error *e);

/* A window of a vector's elements, [lo, hi), for row ranges (Stage R). */
typedef struct {
    uint64_t lo, hi;
    int on;
} rdz_window;

/* The same, with windows (r->nobjects of them, or NULL): a windowed
   logical, integer, double, factor or character object is read only over
   its window, into a destination (and sink) of hi - lo elements; only the
   blocks overlapping it are read, and for strings the dictionary blocks
   before its end. */
int rdz_graph_read_window(rdz_vec *v, rdz_reader *r, const rdz_graph_sinks *sinks,
                          const uint8_t *want, const rdz_window *windows, int threads,
                          rdz_tick_fn tick, void *tick_ctx, rdz_error *e);

/* One character object's strings into the sink, block by block, through
   the reader's own buffers: the reader stays open for a read after it (the
   names a selection is resolved against). The object is a character
   object, or a reference to one. */
int rdz_graph_read_strings(rdz_reader *r, uint32_t object, const rdz_names_sink *sink,
                           rdz_error *e);

/* Validates the object graph of a native file's directory (the container
   reader calls it). */
int rdz_graph_check(const rdz_reader *r, rdz_error *e);

#endif /* RDZ_GRAPH_H */
