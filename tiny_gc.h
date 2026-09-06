/*
 * tiny_gc.h -- a small conservative mark-and-sweep garbage collector.
 *
 * Public API (C99, libc only).  See the design notes at the top of
 * tiny_gc.c for WHY each mechanism works the way it does.
 *
 * Usage pattern:
 *
 *     int main(void) {
 *         tiny_gc_t gc;
 *         void *anchor = &gc;              // any address of a local in main
 *         tgc_start(&gc, &anchor);
 *         ... tgc_alloc(&gc, n) ...        // forget pointers freely
 *         tgc_stop(&gc);                   // final sweep + free tables
 *     }
 *
 * The collector finds roots by conservatively scanning the machine stack
 * between `stack_anchor` (captured at tgc_start) and a local inside its own
 * scanning function, so anything reachable from live locals survives.
 */
#ifndef TINY_GC_H
#define TINY_GC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tiny_gc tiny_gc_t;

/* User-visible per-object flag bits (pass to tgc_alloc_opt). */
#define TGC_ROOT 0x0001u   /* explicit root: never collected while registered */
#define TGC_LEAF 0x0002u   /* leaf: contents are NOT scanned for pointers    */

typedef void (*tgc_dtor)(void *self);

/*
 * The collector state.  Users may embed/allocate this struct directly; all
 * fields are PRIVATE and must not be touched -- they are exposed only so the
 * struct has a known size for stack/embedded use.
 */
struct tiny_gc {
    /* --- private: out-of-band metadata table (open addressing) --------- */
    void  **t_keys;      /* parallel arrays instead of a struct array so the
                            hot lookup loop touches fewer cache lines */
    size_t *t_sizes;
    unsigned *t_flags;   /* user flags | internal mark/tombstone bits */
    tgc_dtor *t_dtors;
    size_t   t_cap;      /* power of two */
    size_t   t_count;    /* live entries */
    size_t   t_tombs;    /* tombstones */

    /* --- private: collection scratch ----------------------------------- */
    void   **work;       /* mark worklist */
    size_t   work_n;
    size_t   work_cap;
    void   **stage;      /* doomed-pointer staging array */
    tgc_dtor *stage_d;
    size_t   stage_n;
    size_t   stage_cap;
    void    *snap;       /* stack-window snapshot buffer */
    size_t   snap_len;
    size_t   snap_cap;

    /* --- private: policy / misc ---------------------------------------- */
    void   *stack_anchor;   /* deep end of the scan window (from tgc_start) */
    uintptr_t heap_min, heap_max;  /* range filter bounds, set per collect  */
    size_t  sweep_floor;    /* tracked count at end of last sweep           */
    int     paused;
    int     busy;           /* re-entry guard while collecting              */
    size_t  collections;    /* number of collections run                    */
};

void  tgc_start(tiny_gc_t *gc, void *stack_anchor);
void  tgc_stop(tiny_gc_t *gc);
void *tgc_alloc(tiny_gc_t *gc, size_t size);
void *tgc_calloc(tiny_gc_t *gc, size_t num, size_t size);
void *tgc_realloc(tiny_gc_t *gc, void *ptr, size_t size);
void  tgc_free(tiny_gc_t *gc, void *ptr);             /* explicit, immediate */
void  tgc_run(tiny_gc_t *gc);                         /* manual collect      */
void  tgc_pause(tiny_gc_t *gc);
void  tgc_resume(tiny_gc_t *gc);

/* Per-object options. */
void  *tgc_alloc_opt(tiny_gc_t *gc, size_t size, unsigned flags, tgc_dtor dtor);
unsigned tgc_get_flags(tiny_gc_t *gc, void *ptr);
size_t   tgc_get_size(tiny_gc_t *gc, void *ptr);

/* Introspection helpers (additions beyond the spec's minimum API; used by
 * the acceptance tests to observe counts without poking privates). */
size_t tgc_tracked_count(tiny_gc_t *gc);
size_t tgc_collection_count(tiny_gc_t *gc);

#ifdef __cplusplus
}
#endif
#endif /* TINY_GC_H */
