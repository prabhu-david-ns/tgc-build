/*
 * tiny_gc.c -- a tiny conservative mark-and-sweep garbage collector.
 *
 * Design notes (the WHY behind each mechanism; see prep/ for the study
 * layer these come from):
 *
 * OUT-OF-BAND METADATA
 *   No per-object header is prepended to allocations.  Instead a hash
 *   table keyed by block address stores {size, flags, dtor} for every
 *   tracked block (BDWGC-style block bookkeeping held outside user data).
 *   Why: payloads stay untouched, and metadata can never be corrupted by
 *   writes through interior pointers.  The cost is one O(1) lookup per
 *   metadata access and the obligation to DELETE entries when blocks die
 *   -- a stale entry would poison a reused address, so every free path
 *   removes its entry.  The table uses open addressing with linear
 *   probing: it is cache friendly (one array scan per probe), needs no
 *   per-entry allocation, and deletions are handled with tombstones that
 *   get squeezed out by the post-sweep rehash.  Capacity is a power of
 *   two so masking replaces division; the table grows at >90% load and
 *   shrinks after sweeps when mostly empty.
 *
 * ROOT DISCOVERY = CONSERVATIVE STACK SCAN
 *   Liveness is defined as transitive reachability from roots.  Roots are:
 *     (a) objects explicitly flagged TGC_ROOT, marked before the stack
 *         pass, and
 *     (b) any word on the machine stack between `stack_anchor` (captured
 *         at tgc_start) and a local variable inside the scanning function.
 *   The window covers both stack growth directions by scanning between
 *   min(anchor, local) and max(anchor, local).  Because the collector has
 *   no type information, EVERY properly aligned word in the window is
 *   treated as a potential pointer ("if it might be a pointer it's
 *   treated as a pointer").  This is safe-by-default: a misidentified
 *   integer can only RETAIN a dead object (bounded waste), never free a
 *   live one, because the collector is non-moving and only adds liveness.
 *
 * REGISTER SPILLING (setjmp)
 *   At collection time hot pointers may live in registers, invisible to a
 *   stack scan.  setjmp(jmp_buf) saves "the calling environment" --
 *   including callee-saved registers -- into an automatic jmp_buf which
 *   sits inside the scan window, so register-held pointers become scannable
 *   for free.  We never longjmp; we merely read the bytes afterwards, the
 *   same trick BDWGC uses when it stops a thread.
 *
 * VOLATILE FUNCTION POINTER
 *   The scanner is called through a `void (*volatile fp)(tiny_gc_t *)`.
 *   A volatile function pointer must be loaded and called indirectly at
 *   runtime, so the compiler cannot inline the callee into tgc_run's
 *   caller chain, cannot tail-call it, and must keep the scanner's frame
 *   (and thus its locals, including the young-bound address) live and
 *   distinct during the call.
 *
 * MARKING
 *   Iterative depth-first marking over an explicit worklist (recursion
 *   could overflow the C stack on deep graphs).  Each entry carries a
 *   visited/mark bit set before descending, which makes cycles safe.
 *   Interior-pointer identification: for each non-leaf object, every
 *   aligned word of its contents is a candidate; candidates outside
 *   [heap_min, heap_max) of currently known blocks are rejected without a
 *   table probe; survivors are looked up EXACTLY -- only block starts are
 *   accepted, so interior pointers into a block do not keep it alive.
 *   Leaf-flagged objects skip content scanning entirely.
 *
 * SWEEPING (staged)
 *   The sweep pass first walks the table collecting doomed {pointer,
 *   destructor} pairs into a staging array and clearing mark bits on
 *   survivors.  Only after traversal completes does it remove table
 *   entries, run destructors, and finally libc-free() the blocks.  Running
 *   arbitrary user code mid-collection is dangerous (a finalizer may
 *   allocate -> re-enter the allocator mid-mutation, or resurrect other
 *   doomed objects), so finalization happens strictly outside the mark
 *   phase, mirroring PEP 442 / BDWGC's "enqueue now, run later" rule.
 *
 * TRIGGER POLICY
 *   Automatic collection when the tracked count exceeds 1.5x the count
 *   left at the end of the previous sweep (one integer of state,
 *   self-tuning: dead-heavy heaps trigger immediately, live-heavy heaps
 *   stretch their interval).  tgc_pause/resume suppress/restore automatic
 *   runs; explicit tgc_run always collects.  Collections never re-enter
 *   themselves (busy flag), even from destructors that allocate.
 *
 * PLATFORM ASSUMPTIONS (documented per requirement 7)
 *   - Flat address space; sizeof(void*) == sizeof(size_t) == word size;
 *     addresses fit in uintptr_t and can be meaningfully ordered/compared.
 *   - libc malloc/calloc/realloc return blocks aligned at least to
 *     alignof(max_align_t) >= sizeof(void*); block starts are therefore
 *     word aligned, and NULL (0) can never be a valid block address.
 *   - The region between two stack addresses is mapped readable memory
 *     (true on Linux/x86-64 and every mainstream hosted target).
 *   - <setjmp.h> exists and jmp_buf holds callee-saved registers in
 *     ordinary memory (POSIX/C99 hosted environments).
 */
#include "tiny_gc.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal flag bits (live in the high half of t_flags).              */
/* ------------------------------------------------------------------ */
#define TGC_MARK      0x80000000u  /* visited bit used during marking    */
#define TGC_TOMBSTONE 0x40000000u  /* deleted slot, keeps probe chains   */
#define TGC_USER_MASK 0x0000ffffu  /* flags users may set                */

#define TGC_INIT_CAP   ((size_t)64)
#define MIN_ALLOC_SIZE ((size_t)1)

static void oom_abort(void)
{
    /* A collector cannot report allocation failure mid-collection. */
    abort();
}

/* ------------------------------------------------------------------ */
/* Hash table (open addressing + linear probing + tombstones).         */
/* ------------------------------------------------------------------ */

/* Multiplicative (Fibonacci-style) mixing of the pointer bits.  Shifting
 * out the low alignment bits first means consecutive malloc results spread
 * well across the power-of-two capacity. */
static size_t tgc_hash(const void *key)
{
    uint64_t x = (uint64_t)(uintptr_t)key >> 3;
    x *= UINT64_C(0x9E3779B97F4A7C15);
    x ^= x >> 32;
    return (size_t)x;
}

static void table_init(tiny_gc_t *gc, size_t cap)
{
    gc->t_cap    = cap;
    gc->t_count  = 0;
    gc->t_tombs  = 0;
    gc->t_keys   = calloc(cap, sizeof *gc->t_keys);
    gc->t_sizes  = calloc(cap, sizeof *gc->t_sizes);
    gc->t_flags  = calloc(cap, sizeof *gc->t_flags);
    gc->t_dtors  = calloc(cap, sizeof *gc->t_dtors);
    if (!gc->t_keys || !gc->t_sizes || !gc->t_flags || !gc->t_dtors)
        oom_abort();
}

/* Rebuild the table at a new capacity, dropping tombstones. */
static void table_rehash(tiny_gc_t *gc, size_t newcap)
{
    void    **ok  = gc->t_keys;
    size_t   *osz = gc->t_sizes;
    unsigned *ofl = gc->t_flags;
    tgc_dtor *odt = gc->t_dtors;
    size_t    ocap = gc->t_cap, i;

    table_init(gc, newcap);

    for (i = 0; i < ocap; i++) {
        if (ok[i] != NULL && !(ofl[i] & TGC_TOMBSTONE)) {
            size_t mask = gc->t_cap - 1;
            size_t j = tgc_hash(ok[i]) & mask;
            while (gc->t_keys[j] != NULL)
                j = (j + 1) & mask;
            gc->t_keys[j]  = ok[i];
            gc->t_sizes[j] = osz[i];
            gc->t_flags[j] = ofl[i];
            gc->t_dtors[j] = odt[i];
            gc->t_count++;
        }
    }
    free(ok);
    free(osz);
    free(ofl);
    free(odt);
}

/* Find the entry for key, or NULL.  Tombstones do not terminate probes. */
static size_t table_find(const tiny_gc_t *gc, const void *key)
{
    size_t mask = gc->t_cap - 1;
    size_t i = tgc_hash(key) & mask;
    while (gc->t_keys[i] != NULL) {
        if (gc->t_keys[i] == key && !(gc->t_flags[i] & TGC_TOMBSTONE))
            return i;
        i = (i + 1) & mask;
    }
    return (size_t)-1;
}

/* Insert or update.  Grows the table when load exceeds ~90%. */
static void table_insert(tiny_gc_t *gc, void *key, size_t size,
                         unsigned flags, tgc_dtor dtor)
{
    size_t mask = gc->t_cap - 1;
    size_t i = tgc_hash(key) & mask;
    size_t first_tomb = (size_t)-1;

    if ((gc->t_count + gc->t_tombs + 1) * 10 > gc->t_cap * 9) {
        table_rehash(gc, gc->t_cap * 2);   /* grow: keep probing short */
        mask = gc->t_cap - 1;
        i = tgc_hash(key) & mask;
    }

    for (;;) {
        if (gc->t_keys[i] == NULL) {
            /* Prefer reusing the oldest tombstone seen on this probe. */
            if (first_tomb != (size_t)-1) {
                i = first_tomb;
                gc->t_tombs--;      /* the reused tombstone goes away */
            }
            gc->t_keys[i]  = key;
            gc->t_sizes[i] = size;
            gc->t_flags[i] = flags & TGC_USER_MASK;
            gc->t_dtors[i] = dtor;
            gc->t_count++;
            return;
        }
        if (gc->t_keys[i] == key) {
            if (!(gc->t_flags[i] & TGC_TOMBSTONE)) {
                gc->t_sizes[i] = size;
                gc->t_flags[i] = flags & TGC_USER_MASK;
                gc->t_dtors[i] = dtor;
                return;
            }
            /* Reusing a tombstoned slot for the same key: it revives.
               The entry becomes live again, so count must grow back. */
            gc->t_flags[i] = flags & TGC_USER_MASK;
            gc->t_sizes[i] = size;
            gc->t_dtors[i] = dtor;
            gc->t_tombs--;
            gc->t_count++;
            return;
        }
        if ((gc->t_flags[i] & TGC_TOMBSTONE) && first_tomb == (size_t)-1)
            first_tomb = i;
        i = (i + 1) & mask;
    }
}

static void table_remove(tiny_gc_t *gc, void *key)
{
    size_t i = table_find(gc, key);
    if (i == (size_t)-1)
        return;
    gc->t_flags[i] |= TGC_TOMBSTONE;
    gc->t_count--;
    gc->t_tombs++;
}

/* Same removal keyed by a raw address (used by realloc when libc moved a
   block and the old pointer must not be dereferenced or "used" as such). */
static void table_find_and_remove_key(tiny_gc_t *gc, uintptr_t key)
{
    table_remove(gc, (void *)key);
}

/* ------------------------------------------------------------------ */
/* Worklist / staging scratch                                          */
/* ------------------------------------------------------------------ */

static void push_work(tiny_gc_t *gc, void *p)
{
    if (gc->work_n >= gc->work_cap) {
        size_t ncap = gc->work_cap ? gc->work_cap * 2 : 1024;
        void **nw = realloc(gc->work, ncap * sizeof *nw);
        if (!nw)
            oom_abort();
        gc->work = nw;
        gc->work_cap = ncap;
    }
    gc->work[gc->work_n++] = p;
}

static void stage_push(tiny_gc_t *gc, void *p, tgc_dtor d)
{
    if (gc->stage_n >= gc->stage_cap) {
        size_t ncap = gc->stage_cap ? gc->stage_cap * 2 : 256;
        void **np = realloc(gc->stage, ncap * sizeof *np);
        tgc_dtor *nd;
        if (!np)
            oom_abort();
        nd = realloc(gc->stage_d, ncap * sizeof *nd);
        if (!nd)
            oom_abort();
        gc->stage = np;
        gc->stage_d = nd;
        gc->stage_cap = ncap;
    }
    gc->stage[gc->stage_n] = p;
    gc->stage_d[gc->stage_n] = d;
    gc->stage_n++;
}

/* ------------------------------------------------------------------ */
/* Marking                                                             */
/* ------------------------------------------------------------------ */

/* Mark an exact block start (if tracked and unmarked) and enqueue it.
 * The bounds are INCLUSIVE of heap_max (= the highest tracked key):
 * rejecting candidates outside [heap_min, heap_max] is only a cheap
 * pre-filter -- correctness comes from the exact-match lookup below,
 * so a candidate equal to the boundary key must not be dropped. */
static void mark_and_push(tiny_gc_t *gc, void *p)
{
    size_t i;
    if ((uintptr_t)p < gc->heap_min || (uintptr_t)p > gc->heap_max)
        return;                     /* range filter: cheap rejection */
    i = table_find(gc, p);
    if (i != (size_t)-1 && !(gc->t_flags[i] & TGC_MARK)) {
        gc->t_flags[i] |= TGC_MARK; /* set BEFORE descending: cycles safe */
        push_work(gc, p);
    }
}

/* Drain the worklist: scan contents of non-leaf objects conservatively. */
static void drain_worklist(tiny_gc_t *gc)
{
    while (gc->work_n > 0) {
        void *p = gc->work[--gc->work_n];
        size_t i = table_find(gc, p);
        uintptr_t a, end;
        if (i == (size_t)-1 || (gc->t_flags[i] & TGC_LEAF))
            continue;               /* leaf: contents not scanned */
        a = (uintptr_t)p;
        end = a + gc->t_sizes[i];
        for (; a + sizeof(void *) <= end; a += sizeof(void *)) {
            void *cand;
            memcpy(&cand, (void *)a, sizeof cand);  /* aliasing-safe read */
            if (cand != NULL)
                mark_and_push(gc, cand);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Conservative stack scan (two phases: capture, then process)         */
/* ------------------------------------------------------------------ */

/*
 * Phase 2: process the captured window snapshot.  Every word-aligned
 * word is a candidate pointer; the range filter rejects words outside
 * [heap_min, heap_max] cheaply and the exact-match lookup does the rest.
 */
static void process_snapshot(tiny_gc_t *gc)
{
    uintptr_t a, end;
    end = (uintptr_t)gc->snap + gc->snap_len;
    for (a = (uintptr_t)gc->snap; a + sizeof(void *) <= end;
         a += sizeof(void *)) {
        void *cand;
        memcpy(&cand, (void *)a, sizeof cand);  /* aliasing-safe read */
        if (cand != NULL)
            mark_and_push(gc, cand);
    }
}

/*
 * Phase 1 body: copy the scan window into the snapshot buffer.
 *
 * The young bound (`young`) is supplied by the PUBLIC entry point that
 * started this collection (tgc_run / tgc_alloc_opt ...): it is the
 * address of a local in that frame -- the GC/mutator boundary.  Everything
 * ABOVE it belongs to mutator code and is scanned; everything BELOW it is
 * GC-internal (this frame, setjmp's jmp_buf, worklist/sweep loops) and is
 * deliberately EXCLUDED.
 *
 * WHY EXCLUDE OUR OWN FRAMES: any C function that keeps a value live
 * across a call must store it either in a callee-saved register or in its
 * own frame; the ABI then forces callees to preserve those registers,
 * so stale heap addresses left by earlier mutator code keep resurfacing
 * in them.  If the collector copied its own frames, those stale values
 * (plus the collector's own working key values) would enter every
 * snapshot as false roots and recently-touched blocks would survive
 * forever.  Cutting at the API boundary makes the snapshot see only
 * genuine mutator state.
 *
 * Called ONLY through a volatile function pointer (see
 * capture_stack_window) so it cannot be inlined or tail-called away and
 * its frame stays honest during the copy.
 */
static void capture_window_body(tiny_gc_t *gc, char *young)
{
    char *anchor = (char *)gc->stack_anchor;
    char *lo, *hi;

    if (!anchor || !young)
        return;
    lo = (anchor < young) ? anchor : young; /* handle either growth dir. */
    hi = (anchor < young) ? young : anchor;

    /* Align the deep bound UP to a word boundary so snapshot offset 0
     * corresponds to an aligned stack word -- the processing phase strides
     * the buffer by sizeof(void*), exactly like an in-place scan would. */
    {
        uintptr_t lot = (uintptr_t)lo;
        lot = (lot + sizeof(void *) - 1) & ~((uintptr_t)sizeof(void *) - 1);
        if ((uintptr_t)hi <= lot)
            return;
        lo = (char *)lot;
    }

    {
        size_t len = (size_t)(hi - lo);
        if (gc->snap_cap < len) {
            char *ns = realloc(gc->snap, len);
            if (!ns)
                oom_abort();
            gc->snap = ns;
            gc->snap_cap = len;
        }
        memcpy(gc->snap, lo, len);
        gc->snap_len = len;
    }
}

/*
 * Spill callee-saved registers with setjmp, then snapshot the window.
 *
 * WHY setjmp: per the classic conservative-GC recipe, setjmp() saves the
 * calling environment -- including callee-saved registers -- into an
 * automatic jmp_buf, spilling register-held values into scannable memory.
 * We never longjmp anywhere; the spill exists so that a build which WANTS
 * register-inclusive roots can find them on the stack.  This
 * implementation intentionally does NOT add those bytes to the root set:
 * at the collection point the register file may still carry dead values
 * restored by ABI epilogues, and scanning them would retain arbitrary
 * recently-touched garbage.  Liveness is therefore defined by REACHABILITY
 * FROM MUTATOR MEMORY (stack frames above the API boundary + object
 * contents), which is deterministic; keep references in locals/globals --
 * not solely in registers -- across tgc_* calls (normal C code does).
 *
 * WHY capture before anything else: see collect() -- the rest of the
 * collection dirties registers and stack slots with tracked addresses,
 * which must NOT enter the root set as false positives.
 *
 * WHY the volatile function pointer: calling capture_window_body through
 * a volatile-loaded pointer forces a real indirect call, so the compiler
 * cannot inline the capturer into this function nor discard its frame
 * boundary.
 */
static void capture_stack_window(tiny_gc_t *gc, char *young)
{
    static void (*volatile fp)(tiny_gc_t *, char *) = capture_window_body;
    jmp_buf regs;

    memset(&regs, 0, sizeof regs);       /* deterministic bytes */
    if (setjmp(regs) == 0) {             /* spills callee-saved registers      */
        fp(gc, young);
    }
}

/* ------------------------------------------------------------------ */
/* Sweep                                                               */
/* ------------------------------------------------------------------ */

static void sweep(tiny_gc_t *gc)
{
    size_t i, n;

    /* Pass 1 (traversal): stage the doomed, clear marks on survivors. */
    gc->stage_n = 0;
    for (i = 0; i < gc->t_cap; i++) {
        if (gc->t_keys[i] == NULL)
            continue;
        if (gc->t_flags[i] & TGC_TOMBSTONE)
            continue;
        if (gc->t_flags[i] & TGC_MARK)
            gc->t_flags[i] &= ~(unsigned)TGC_MARK;
        else
            stage_push(gc, gc->t_keys[i], gc->t_dtors[i]);
    }

    /* Pass 2: drop metadata entries FIRST so destructors that allocate or
       free other objects see a consistent table. */
    for (n = 0; n < gc->stage_n; n++)
        table_remove(gc, gc->stage[n]);

    /* Pass 3: run destructors then free(), strictly after traversal. */
    for (n = 0; n < gc->stage_n; n++) {
        tgc_dtor d = gc->stage_d[n];
        if (d)
            d(gc->stage[n]);
        free(gc->stage[n]);
    }

    /* Policy bookkeeping: remember how much survived this sweep. */
    gc->sweep_floor = gc->t_count;

    /* Squeeze tombstones out; shrink a mostly-empty table.  Keep at least
       2x headroom over survivors so the next burst does not immediately
       regrow. */
    if (gc->t_tombs > 0 ||
        (gc->t_cap > TGC_INIT_CAP && gc->t_count * 4 <= gc->t_cap)) {
        size_t want = gc->t_count * 2;
        if (want < TGC_INIT_CAP)
            want = TGC_INIT_CAP;
        else {
            size_t c = TGC_INIT_CAP;
            while (c < want)
                c *= 2;
            want = c;
        }
        if (want != gc->t_cap || gc->t_tombs > 0)
            table_rehash(gc, want);
    }
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

static void collect(tiny_gc_t *gc, char *young)
{
    size_t i;

    if (gc->busy)
        return;                    /* re-entry guard (dtor -> alloc case) */
    gc->busy = 1;

    gc->work_n = 0;

    /* PHASE 1: capture machine context FIRST -- registers (setjmp spill)
       and the mutator stack window -- before any metadata is touched, so
       no collector-internal key values leak into the root set. */
    capture_stack_window(gc, young);

    /* PHASE 2 setup: range-filter bounds over currently known blocks.
       From here on we may dirty registers freely; the snapshot is taken. */
    gc->heap_min = (uintptr_t)-1;
    gc->heap_max = 0;
    for (i = 0; i < gc->t_cap; i++) {
        uintptr_t k;
        if (gc->t_keys[i] == NULL || (gc->t_flags[i] & TGC_TOMBSTONE))
            continue;
        k = (uintptr_t)gc->t_keys[i];
        if (k < gc->heap_min)
            gc->heap_min = k;
        if (k > gc->heap_max)
            gc->heap_max = k;
    }

    /* Explicit roots are marked before the stack pass. */
    for (i = 0; i < gc->t_cap; i++) {
        if (gc->t_keys[i] != NULL && !(gc->t_flags[i] & TGC_TOMBSTONE) &&
            (gc->t_flags[i] & TGC_ROOT))
            mark_and_push(gc, gc->t_keys[i]);
    }

    process_snapshot(gc);          /* conservative roots from the window   */
    drain_worklist(gc);            /* transitive closure, cycle-safe      */
    sweep(gc);                     /* staged teardown outside traversal   */

    gc->collections++;
    gc->busy = 0;
}

/* Automatic trigger: tracked count exceeds 1.5x last sweep's survivors.
 * A small absolute minimum damps thrash in near-empty heaps (the same
 * seeded-floor idea as the P2 policy studied in prep/trigger-policy.py:
 * collecting every second allocation of a 3-object program buys nothing). */
#define TGC_MIN_AUTO ((size_t)64)

static void maybe_autocollect(tiny_gc_t *gc, char *young)
{
    if (!gc->paused && !gc->busy &&
        gc->t_count > gc->sweep_floor + gc->sweep_floor / 2 &&
        gc->t_count > TGC_MIN_AUTO)
        collect(gc, young);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void tgc_start(tiny_gc_t *gc, void *stack_anchor)
{
    memset(gc, 0, sizeof *gc);
    table_init(gc, TGC_INIT_CAP);
    gc->stack_anchor = stack_anchor;
    /* Seed the growth policy so tiny programs are not collected on every
       second allocation; the floor is refreshed by every sweep anyway. */
    gc->sweep_floor = 8;
}

void tgc_stop(tiny_gc_t *gc)
{
    size_t i;
    /* Final teardown: free every still-tracked block (destructors run),
       then release the internal tables.  Nothing is reachable-safe here by
       definition -- the collector is shutting down -- so no marking pass. */
    for (i = 0; i < gc->t_cap; i++) {
        if (gc->t_keys[i] != NULL && !(gc->t_flags[i] & TGC_TOMBSTONE)) {
            tgc_dtor d = gc->t_dtors[i];
            if (d)
                d(gc->t_keys[i]);
            free(gc->t_keys[i]);
        }
    }
    free(gc->t_keys);
    free(gc->t_sizes);
    free(gc->t_flags);
    free(gc->t_dtors);
    free(gc->work);
    free(gc->stage);
    free(gc->stage_d);
    free(gc->snap);
    memset(gc, 0, sizeof *gc);
}

/* `young` is the caller's GC/mutator stack boundary; see
 * capture_window_body for why every collection is given one. */
static void *alloc_common(tiny_gc_t *gc, void *block, size_t size,
                          unsigned flags, tgc_dtor dtor, char *young)
{
    if (!block)
        return NULL;
    /* Collect BEFORE the new block enters the table: a collection
       triggered here must not see the just-created pointer, whose only
       references live below the young boundary (this frame / registers).
       Sweeping it would hand the caller a dangling pointer. */
    maybe_autocollect(gc, young);
    table_insert(gc, block, size, flags, dtor);
    return block;
}

void *tgc_alloc_opt(tiny_gc_t *gc, size_t size, unsigned flags, tgc_dtor dtor)
{
    char young;                   /* GC/mutator boundary for auto-collect */
    void *block;
    if (size == 0)
        size = MIN_ALLOC_SIZE;    /* keep every block a real, scannable span */
    block = malloc(size);
    return alloc_common(gc, block, size, flags, dtor, &young);
}

void *tgc_alloc(tiny_gc_t *gc, size_t size)
{
    return tgc_alloc_opt(gc, size, 0, NULL);
}

void *tgc_calloc(tiny_gc_t *gc, size_t num, size_t size)
{
    char young;                   /* GC/mutator boundary for auto-collect */
    void *block;
    if (num != 0 && size > SIZE_MAX / num)
        return NULL;              /* overflow check */
    block = calloc(num ? num : 1, size ? size : 1);
    return alloc_common(gc, block, num * size, 0, NULL, &young);
}

void *tgc_realloc(tiny_gc_t *gc, void *ptr, size_t size)
{
    char young;                   /* GC/mutator boundary for auto-collect */
    size_t i;
    unsigned flags;
    tgc_dtor dtor;
    void *block;

    if (ptr == NULL)
        return tgc_alloc(gc, size);
    if (size == 0) {
        tgc_free(gc, ptr);
        return NULL;
    }

    i = table_find(gc, ptr);
    if (i == (size_t)-1)
        return NULL;              /* realloc of an untracked pointer */

    flags = gc->t_flags[i] & TGC_USER_MASK;
    dtor  = gc->t_dtors[i];

    /* Collect BEFORE touching libc realloc: at this point the block is
       still tracked and still rooted by the caller's mutator memory, so a
       collection is safe.  After realloc() the block may have MOVED, and
       its new address exists only in this frame (below the young scan
       boundary) until the caller re-roots it -- a collection in that
       window would sweep the fresh block and return a dangling pointer. */
    maybe_autocollect(gc, &young);

    {
        /* Keep the old address as an integer: after a successful moving
           realloc the C standard forbids *using* the old pointer value,
           though we only need its numeric bits as the dead table key. */
        uintptr_t old_key = (uintptr_t)ptr;

        block = realloc(ptr, size);   /* libc may move the block */
        if (block == NULL)
            return NULL;              /* original untouched and still tracked */

        if ((uintptr_t)block != old_key) {
            table_find_and_remove_key(gc, old_key);
            table_insert(gc, block, size, flags, dtor);
        } else {
            gc->t_sizes[i] = size;
        }
    }

    return block;
}

void tgc_free(tiny_gc_t *gc, void *ptr)
{
    size_t i;
    tgc_dtor d;
    if (ptr == NULL)
        return;
    i = table_find(gc, ptr);
    if (i == (size_t)-1)
        return;                   /* unknown pointer: ignore, like free(0) */
    d = gc->t_dtors[i];
    table_remove(gc, ptr);
    if (d)
        d(ptr);                   /* explicit frees finalize immediately */
    free(ptr);
}

void tgc_run(tiny_gc_t *gc)
{
    char young;                   /* GC/mutator stack boundary */
    collect(gc, &young);          /* always allowed, even while paused */
}

void tgc_pause(tiny_gc_t *gc)
{
    gc->paused = 1;
}

void tgc_resume(tiny_gc_t *gc)
{
    gc->paused = 0;
}

unsigned tgc_get_flags(tiny_gc_t *gc, void *ptr)
{
    size_t i = table_find(gc, ptr);
    if (i == (size_t)-1)
        return 0;
    return gc->t_flags[i] & TGC_USER_MASK;
}

size_t tgc_get_size(tiny_gc_t *gc, void *ptr)
{
    size_t i = table_find(gc, ptr);
    if (i == (size_t)-1)
        return 0;
    return gc->t_sizes[i];
}

size_t tgc_tracked_count(tiny_gc_t *gc)
{
    return gc->t_count;
}

size_t tgc_collection_count(tiny_gc_t *gc)
{
    return gc->collections;
}
