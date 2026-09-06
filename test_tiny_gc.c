/*
 * test_tiny_gc.c -- acceptance tests for the tiny conservative GC.
 *
 * DETERMINISM DISCIPLINE
 * ----------------------
 * A conservative collector scans stale stack bytes, so tests must ensure
 * that no copy of a dropped pointer survives anywhere in the scan window
 * when a collection is expected to reclaim.  Three rules are used:
 *
 * 1. VOLATILE SLOTS: references a test must keep or explicitly drop are
 *    declared `volatile`, giving exactly ONE memory location per
 *    reference that a plain store can clear.
 *
 * 2. PHASE WORKERS: all code that HANDLES object pointers (allocation,
 *    linking, tgc_get_size checks...) lives in noinline "worker"
 *    functions.  A worker's frame -- including compiler spill slots and
 *    the outgoing-argument area where pointer arguments are staged --
 *    dies when it returns, and is then ERASED by clobber_stack().  Test
 *    wrapper functions never touch object pointers themselves, so their
 *    live frames stay clean.  (Without this, libc's same-address reuse
 *    turns every stale copy into a permanent false root: the old address
 *    matches the NEW block living there now.)
 *
 * 3. CLOBBER BEFORE COLLECT: prepare_collect() zeroes a large automatic
 *    buffer placed where dead helper frames used to be.  Zeros can never
 *    look like heap pointers.
 *
 * The stress test additionally uses a fixed-seed PRNG so the operation
 * stream is identical on every run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiny_gc.h"

#define NOINLINE __attribute__((noinline))

static int failures;
static int test_ok;

/* Comfortably above the GC's damped auto-trigger minimum: a pause-phase
 * burst past this size proves suppression is real. */
#define TGC_AUTO_DEMO_THRESHOLD 24

#define REQUIRE(cond) do {                                                  \
    if (!(cond)) {                                                          \
        printf("  !! line %d: %s\n", __LINE__, #cond);                      \
        test_ok = 0;                                                        \
    }                                                                       \
} while (0)

/* Erase stale pointer copies left behind by returned-from helper frames.
   Zeroes are harmless to the conservative scan (NULL is rejected).
   MUST be noinline: as a real function its frame -- and the 16 KB buffer
   inside it -- lands directly BELOW the caller's frame, exactly where the
   dead helpers left their copies.  If inlined, the compiler would hoist
   the buffer into an arbitrary slot of the LIVE frame and miss them. */
NOINLINE static void clobber_stack(void)
{
    volatile char junk[16384];
    memset((void *)junk, 0, sizeof junk);
}

/* Full pre-collection hygiene; called by wrappers immediately before any
   tgc_run whose result is asserted on. */
static void prepare_collect(void)
{
    clobber_stack();
}

/* ------------------------------------------------------------------ */
/* Graph node used across several tests: children live in a trailing   */
/* array inside the object; default objects are non-leaf so their       */
/* contents ARE scanned.                                                */
/* ------------------------------------------------------------------ */
typedef struct node {
    unsigned id;
    size_t nchild;
    struct node *children[];
} node_t;

NOINLINE static node_t *make_node(tiny_gc_t *gc, unsigned id, size_t nchild)
{
    node_t *n = tgc_alloc_opt(gc, sizeof(node_t) + nchild * sizeof(node_t *),
                              0, NULL);
    size_t i;
    if (!n)
        exit(EXIT_FAILURE);
    n->id = id;
    n->nchild = nchild;
    for (i = 0; i < nchild; i++)
        n->children[i] = NULL;
    return n;
}

/* ------------------------------------------------------------------ */
/* Test 1: basic -- garbage dies when its only local goes away.         */
/* ------------------------------------------------------------------ */
static void *observed_dead1;

NOINLINE static void basic_make_garbage(tiny_gc_t *gc, size_t sz)
{
    void *volatile p = tgc_alloc(gc, sz);
    if (!p)
        exit(EXIT_FAILURE);
    ((char *)p)[0] = 1;          /* touch it */
    observed_dead1 = p;          /* remember address for post-mortem check   */
    p = NULL;                    /* volatile store: clears the ONLY reference */
}

NOINLINE static int basic_verify_gone(tiny_gc_t *gc, size_t base)
{
    REQUIRE(tgc_get_size(gc, observed_dead1) == 0);
    return tgc_tracked_count(gc) == base;
}

static int test_basic(tiny_gc_t *gc)
{
    size_t base;
    void *volatile keep;

    test_ok = 1;
    base = tgc_tracked_count(gc);

    basic_make_garbage(gc, 64);  /* created and dropped inside a worker */
    REQUIRE(tgc_tracked_count(gc) == base + 1);

    keep = tgc_alloc(gc, 32);    /* rooted via this wrapper's volatile slot */

    prepare_collect();           /* erase the dead worker frame            */
    tgc_run(gc);

    /* The dropped object is gone; the rooted one survives. */
    REQUIRE(basic_verify_gone(gc, base + 1));
    REQUIRE(tgc_get_size(gc, keep) == 32);

    /* A second run changes nothing (idempotent at the fixpoint). */
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base + 1);

    keep = NULL;
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base);
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 2: reachability -- a rooted structure survives many collects.   */
/* ------------------------------------------------------------------ */
NOINLINE static void reach_build(tiny_gc_t *gc, node_t **out_root)
{
    node_t *volatile tree = make_node(gc, 1, 3);
    node_t *volatile c0 = make_node(gc, 10, 1);
    node_t *volatile c1 = make_node(gc, 11, 0);
    node_t *volatile c2 = make_node(gc, 12, 0);
    node_t *volatile g = make_node(gc, 100, 0);
    if (!tree || !c0 || !c1 || !c2 || !g)
        exit(EXIT_FAILURE);
    c0->children[0] = g;
    tree->children[0] = c0;
    tree->children[1] = c1;
    tree->children[2] = c2;
    *out_root = tree;
}

NOINLINE static int reach_verify(tiny_gc_t *gc, node_t *root, size_t base,
                                 size_t expect)
{
    REQUIRE(tgc_get_size(gc, root) >= sizeof(node_t));
    REQUIRE(root->nchild == 3);
    REQUIRE(root->id == 1);
    REQUIRE(root->children[0]->id == 10);
    REQUIRE(root->children[1]->id == 11);
    REQUIRE(root->children[2]->id == 12);
    REQUIRE(root->children[0]->children[0]->id == 100);
    return tgc_tracked_count(gc) == expect &&
           (base == 0 || tgc_get_size(gc, root) != 0);
}

static int test_reachability(tiny_gc_t *gc)
{
    size_t base;
    node_t *volatile tree = NULL;
    int i;

    test_ok = 1;
    base = tgc_tracked_count(gc);

    reach_build(gc, (node_t **)&tree);   /* build inside a worker */
    REQUIRE(tgc_tracked_count(gc) == base + 5);

    for (i = 0; i < 5; i++) {
        prepare_collect();
        tgc_run(gc);
    }

    REQUIRE(reach_verify(gc, tree, base, base + 5));

    tree = NULL;
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base);
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 3: cycles -- cyclic structures handled by mark-sweep.           */
/* ------------------------------------------------------------------ */
typedef struct ring {
    struct ring *peer;
    unsigned id;
} ring_t;

static void *observed_ring_a;
static void *observed_ring_b;

NOINLINE static void cycle_build_unrooted(tiny_gc_t *gc)
{
    ring_t *volatile a = tgc_alloc_opt(gc, sizeof(ring_t), 0, NULL);
    ring_t *volatile b = tgc_alloc_opt(gc, sizeof(ring_t), 0, NULL);
    if (!a || !b)
        exit(EXIT_FAILURE);
    a->id = 1;
    b->id = 2;
    a->peer = b;
    b->peer = a;
    observed_ring_a = a;
    observed_ring_b = b;
    a = NULL;                    /* drop both references from this frame */
    b = NULL;
}

NOINLINE static int cycle_check_unrooted_gone(tiny_gc_t *gc, size_t base)
{
    REQUIRE(tgc_get_size(gc, observed_ring_a) == 0);
    REQUIRE(tgc_get_size(gc, observed_ring_b) == 0);
    return tgc_tracked_count(gc) == base;
}

NOINLINE static int cycle_rooted_scenario(tiny_gc_t *gc, size_t base)
{
    /* Reachable cycle survives (visited bit prevents infinite descent):
       x carries TGC_ROOT so neither member needs a stack reference. */
    ring_t *volatile x = tgc_alloc_opt(gc, sizeof(ring_t), TGC_ROOT, NULL);
    ring_t *volatile y = tgc_alloc_opt(gc, sizeof(ring_t), 0, NULL);
    if (!x || !y)
        return 0;
    x->peer = y;
    y->peer = x;
    observed_ring_a = x;
    observed_ring_b = y;

    prepare_collect();
    tgc_run(gc);

    REQUIRE(tgc_get_size(gc, x) == sizeof(ring_t));   /* cycle kept */
    REQUIRE(tgc_get_size(gc, y) == sizeof(ring_t));

    tgc_free(gc, x);              /* explicit cleanup of the rooted pair */
    tgc_free(gc, y);
    return tgc_tracked_count(gc) == base;
}

static int test_cycles(tiny_gc_t *gc)
{
    size_t base;

    test_ok = 1;
    base = tgc_tracked_count(gc);

    /* Part 1: reachable cycle survives thanks to the root flag. */
    REQUIRE(cycle_rooted_scenario(gc, base));
    REQUIRE(tgc_tracked_count(gc) == base);

    /* Part 2: unreachable cycle -- both members die together. */
    cycle_build_unrooted(gc);
    REQUIRE(tgc_tracked_count(gc) == base + 2);
    prepare_collect();
    tgc_run(gc);
    REQUIRE(cycle_check_unrooted_gone(gc, base));
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 4: interleaved garbage -- churn around a live list.             */
/* ------------------------------------------------------------------ */
typedef struct lnode {
    struct lnode *next;
    unsigned val;
} lnode_t;

NOINLINE static void inter_round_build(tiny_gc_t *gc, lnode_t **head_io,
                                       int round)
{
    lnode_t *volatile n = tgc_alloc(gc, sizeof(lnode_t));
    if (!n)
        exit(EXIT_FAILURE);
    n->val = (unsigned)round;
    n->next = *head_io;
    *head_io = n;
    n = NULL;

    /* two immediately-dead allocations per round; their addresses pass
       through this dying frame only and never reach the wrapper */
    if (!tgc_alloc(gc, 48) || !tgc_alloc(gc, 80))
        exit(EXIT_FAILURE);
}

NOINLINE static int inter_round_verify(tiny_gc_t *gc, lnode_t *head,
                                       int round, size_t base)
{
    lnode_t *volatile p = head;
    int expect = round, len = 0;
    while (p) {
        REQUIRE(p->val == (unsigned)(expect--));
        len++;
        p = p->next;
    }
    REQUIRE(len == round + 1);
    return tgc_tracked_count(gc) == base + (size_t)(round + 1);
}

static int test_interleaved(tiny_gc_t *gc)
{
    size_t base;
    lnode_t *volatile head = NULL;
    int round;

    test_ok = 1;
    base = tgc_tracked_count(gc);

    for (round = 0; round < 8; round++) {
        inter_round_build(gc, (lnode_t **)&head, round);
        prepare_collect();       /* erase the builder's dead frame */
        tgc_run(gc);
        REQUIRE(inter_round_verify(gc, head, round, base));
    }

    head = NULL;
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base);
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 5: realloc -- grow/shrink keeps graph and table consistent.     */
/* ------------------------------------------------------------------ */
NOINLINE static node_t *realloc_build_parent(tiny_gc_t *gc)
{
    node_t *volatile parent = make_node(gc, 7, 4);
    int i;
    if (!parent)
        exit(EXIT_FAILURE);
    for (i = 0; i < 4; i++) {
        node_t *volatile kid = make_node(gc, 50u + (unsigned)i, 0);
        if (!kid)
            exit(EXIT_FAILURE);
        parent->children[i] = kid;
        kid = NULL;
    }
    return (node_t *)parent;
}

NOINLINE static int realloc_grow(tiny_gc_t *gc, node_t **parent_io)
{
    node_t *volatile grown = tgc_realloc(
        gc, *parent_io, sizeof(node_t) + 8 * sizeof(node_t *));
    size_t i;
    if (!grown)
        return 0;
    REQUIRE(tgc_get_size(gc, grown) ==
            (size_t)(sizeof(node_t) + 8 * sizeof(node_t *)));
    REQUIRE(grown == *parent_io || tgc_get_size(gc, *parent_io) == 0);
    grown->nchild = 8;
    for (i = 4; i < 8; i++) {
        node_t *volatile kid = make_node(gc, 50u + (unsigned)i, 0);
        if (!kid)
            return 0;
        grown->children[i] = kid;
        kid = NULL;
    }
    *parent_io = grown;
    return 1;
}

NOINLINE static int realloc_shrink(tiny_gc_t *gc, node_t **parent_io)
{
    node_t *volatile shrunk = tgc_realloc(
        gc, *parent_io, sizeof(node_t) + 2 * sizeof(node_t *));
    if (!shrunk)
        return 0;
    shrunk->nchild = 2;
    *parent_io = shrunk;
    return 1;
}

NOINLINE static int realloc_verify_survivors(tiny_gc_t *gc, node_t *parent,
                                             size_t base, size_t expect)
{
    size_t i;
    REQUIRE(tgc_tracked_count(gc) == expect);
    if (tgc_tracked_count(gc) != expect)
        return 0;
    REQUIRE(parent->id == 7);
    REQUIRE(parent->children[0]->id == 50);
    REQUIRE(parent->children[1]->id == 51);
    for (i = 0; i < 2; i++)
        REQUIRE(tgc_get_size(gc, parent->children[i]) == sizeof(node_t));
    return tgc_tracked_count(gc) >= base;
}

static int test_realloc(tiny_gc_t *gc)
{
    size_t base;
    node_t *volatile parent;

    test_ok = 1;
    base = tgc_tracked_count(gc);

    parent = realloc_build_parent(gc);
    REQUIRE(tgc_tracked_count(gc) == base + 5);

    /* Grow 4 -> 8 children through the collector's realloc. */
    REQUIRE(realloc_grow(gc, (node_t **)&parent));
    REQUIRE(tgc_tracked_count(gc) == base + 9);   /* parent + 8 children */

    prepare_collect();
    tgc_run(gc);
    REQUIRE(realloc_verify_survivors(gc, parent, base, base + 9));

    /* Shrink back to 2: slots 2..7 vanish, children 50..55 become garbage. */
    REQUIRE(realloc_shrink(gc, (node_t **)&parent));

    prepare_collect();            /* erase stale copies of old addresses  */
    tgc_run(gc);
    REQUIRE(realloc_verify_survivors(gc, parent, base, base + 3));

    parent = NULL;
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base);
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 6: explicit free + destructors -- ordering and counts.          */
/* ------------------------------------------------------------------ */
static char dtor_log[8];
static size_t dtor_cnt;

static void logging_dtor(void *self)
{
    if (dtor_cnt < sizeof dtor_log)
        dtor_log[dtor_cnt] = *(char *)self;
    dtor_cnt++;
}

NOINLINE static char *dtors_build(tiny_gc_t *gc, char tag)
{
    char *volatile p = tgc_alloc_opt(gc, 16, 0, logging_dtor);
    if (!p)
        exit(EXIT_FAILURE);
    *p = tag;
    return (char *)p;
}

NOINLINE static void dtors_explicit_phase(tiny_gc_t *gc)
{
    /* scratch object created and freed explicitly: dtor runs at once */
    char *volatile s = dtors_build(gc, 'S');
    tgc_free(gc, s);
    s = NULL;
    REQUIRE(dtor_cnt == 1);
    REQUIRE(dtor_log[0] == 'S');
}

NOINLINE static int dtors_verify_sweep(tiny_gc_t *gc, char *kept, size_t base)
{
    /* Both unreachable objects (A and C) are finalized by the sweep --
       exactly once each, strictly after traversal.  Their RELATIVE order
       is table-iteration order (address-hash dependent), so assert set
       membership, which IS deterministic. */
    size_t a_seen = 0, c_seen = 0;
    REQUIRE(dtor_cnt == 3);
    REQUIRE(dtor_log[0] == 'S');
    REQUIRE((dtor_log[1] == 'A') + (dtor_log[1] == 'C') == 1);
    REQUIRE((dtor_log[2] == 'A') + (dtor_log[2] == 'C') == 1);
    a_seen = (unsigned)(dtor_log[1] == 'A') + (unsigned)(dtor_log[2] == 'A');
    c_seen = (unsigned)(dtor_log[1] == 'C') + (unsigned)(dtor_log[2] == 'C');
    REQUIRE(a_seen == 1 && c_seen == 1);
    REQUIRE(tgc_get_size(gc, kept) == 16);
    REQUIRE(*(char *)kept == 'B');
    return tgc_tracked_count(gc) == base + 1;
}

NOINLINE static int dtors_explicit_b(tiny_gc_t *gc, char *kept, size_t base)
{
    tgc_free(gc, kept);           /* explicit free of the survivor */
    REQUIRE(dtor_cnt == 4);
    REQUIRE(dtor_log[3] == 'B');
    return tgc_tracked_count(gc) == base;
}

static int test_dtors(tiny_gc_t *gc)
{
    size_t base;
    char *volatile kept = NULL;
    char *volatile dead_a = NULL, *volatile dead_c = NULL;

    test_ok = 1;
    base = tgc_tracked_count(gc);
    dtor_cnt = 0;
    memset(dtor_log, 0, sizeof dtor_log);

    dead_a = dtors_build(gc, 'A');
    kept = dtors_build(gc, 'B');
    dead_c = dtors_build(gc, 'C');

    dtors_explicit_phase(gc);

    dead_a = NULL;                /* A becomes garbage                  */
    dead_c = NULL;                /* C becomes garbage                  */
    /* kept stays rooted via this wrapper's volatile slot */

    prepare_collect();
    tgc_run(gc);

    REQUIRE(dtors_verify_sweep(gc, kept, base));

    REQUIRE(dtors_explicit_b(gc, kept, base));
    kept = NULL;
    (void)dead_a;                 /* slots were only written, then cleared */
    (void)dead_c;
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 7: leaf flag -- leaf contents are not scanned.                  */
/* ------------------------------------------------------------------ */
NOINLINE static int leaf_setup_and_drop(tiny_gc_t *gc,
                                        void *volatile *holder_out)
{
    void *volatile child = tgc_alloc(gc, 64);
    void *volatile holder = tgc_alloc_opt(gc, sizeof(void *), TGC_LEAF, NULL);
    if (!child || !holder)
        return 0;
    REQUIRE((tgc_get_flags(gc, holder) & TGC_LEAF) != 0);
    memcpy(holder, (const void *)(uintptr_t)&child,
           sizeof child);        /* hide a pointer in the leaf */
    child = NULL;                 /* only reference lives inside the leaf  */
    *holder_out = holder;
    return 1;
}

NOINLINE static int leaf_verify_child_gone(tiny_gc_t *gc, void *holder,
                                           size_t base)
{
    /* Leaf was NOT scanned => child died despite being "referenced". */
    REQUIRE(tgc_get_size(gc, holder) == sizeof(void *));
    return tgc_tracked_count(gc) == base + 1;
}

NOINLINE static int leaf_control_keeps(tiny_gc_t *gc, size_t base)
{
    /* Control: same setup WITHOUT the leaf flag keeps the target alive. */
    void *volatile kid = tgc_alloc(gc, 64);
    void *volatile parent = tgc_alloc_opt(gc, sizeof(void *), 0, NULL);
    if (!kid || !parent)
        return 0;
    memcpy(parent, (const void *)(uintptr_t)&kid, sizeof kid);
    kid = NULL;
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_get_size(gc, parent) == sizeof(void *));
    REQUIRE(tgc_tracked_count(gc) == base + 3);
    tgc_free(gc, parent);
    return 1;
}

static int test_leaf_flag(tiny_gc_t *gc)
{
    size_t base;
    void *volatile holder = NULL;

    test_ok = 1;
    base = tgc_tracked_count(gc);

    REQUIRE(leaf_setup_and_drop(gc, (void *volatile *)&holder));
    prepare_collect();
    tgc_run(gc);
    REQUIRE(leaf_verify_child_gone(gc, holder, base));

    REQUIRE(leaf_control_keeps(gc, base));

    holder = NULL;
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base);
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 8: pause/resume -- auto trigger suppressed then restored.       */
/* ------------------------------------------------------------------ */
NOINLINE static size_t pause_burst(tiny_gc_t *gc, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!tgc_alloc(gc, 24))
            exit(EXIT_FAILURE);
    return tgc_tracked_count(gc);
}

NOINLINE static int resume_refill_until_fired(tiny_gc_t *gc,
                                              size_t collections_before)
{
    int i;
    for (i = 0; i < 400; i++) {
        if (!tgc_alloc(gc, 24))
            exit(EXIT_FAILURE);
        if (tgc_collection_count(gc) > collections_before)
            return 1;             /* the automatic policy fired */
    }
    return 0;
}

static int test_pause_resume(tiny_gc_t *gc)
{
    size_t base, peak_paused, population;
    size_t collections_before;

    test_ok = 1;
    base = tgc_tracked_count(gc);
    collections_before = tgc_collection_count(gc);

    tgc_pause(gc);
    /* A burst far past the 1.5x growth factor must not fire while paused. */
    peak_paused = pause_burst(gc, 200);
    REQUIRE(peak_paused > base + TGC_AUTO_DEMO_THRESHOLD);
    REQUIRE(tgc_collection_count(gc) == collections_before);

    tgc_run(gc);                  /* manual collect still works when paused */
    REQUIRE(tgc_collection_count(gc) == collections_before + 1);
    REQUIRE(tgc_tracked_count(gc) < peak_paused);  /* burst was garbage */

    tgc_resume(gc);
    /* Refill past the trigger point: the automatic policy must fire. */
    population = tgc_tracked_count(gc);
    REQUIRE(resume_refill_until_fired(gc, collections_before + 1));
    REQUIRE(tgc_tracked_count(gc) <= population +
            TGC_AUTO_DEMO_THRESHOLD);  /* burst mostly died */

    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base);
    return test_ok;
}

/* ------------------------------------------------------------------ */
/* Test 9: stress -- thousands of random blocks, linkage and churn.     */
/* ------------------------------------------------------------------ */
#define SLOTS 1024
#define STRESS_OPS 6000

static uint32_t rng_state = 12345u;    /* fixed seed => deterministic ops */

static uint32_t rng_next(void)
{
    /* xorshift32: full determinism independent of libc rand() quality. */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Stress body runs in its own frame so the slot table -- the root set --
 * lives ON THE STACK inside the scan window.  (A static or malloc'd table
 * would NOT be scanned, and every collection would free reachable
 * blocks.)  All slot references are dropped before returning. */
typedef struct {
    size_t total_allocs, peak, final_tracked;
} stress_stats;

NOINLINE static void stress_worker(tiny_gc_t *gc, stress_stats *st)
{
    void *slots[SLOTS];
    int op;

    memset(slots, 0, sizeof slots);
    st->total_allocs = st->peak = 0;
    rng_state = 12345u;

    for (op = 0; op < STRESS_OPS; op++) {
        uint32_t r = rng_next();
        int idx = (int)(rng_next() % SLOTS);

        switch (r % 5) {
        case 0: case 1: {         /* allocate a random-size block */
            size_t sz = 8 + (size_t)(rng_next() % 512);
            void *volatile p = tgc_calloc(gc, 1, sz);
            if (!p)
                exit(EXIT_FAILURE);
            slots[idx] = p;
            st->total_allocs++;
            break;
        }
        case 2: {                 /* random linkage: block <- other slot */
            int j = (int)(rng_next() % SLOTS);
            if (slots[idx] && slots[j])
                memcpy(slots[idx], &slots[j], sizeof(void *));
            break;
        }
        case 3:                   /* drop a subset (garbage accumulates) */
            slots[idx] = NULL;
            break;
        case 4: {                 /* realloc something live */
            size_t sz = 8 + (size_t)(rng_next() % 256);
            if (slots[idx]) {
                void *volatile np = tgc_realloc(gc, slots[idx], sz);
                if (!np)
                    exit(EXIT_FAILURE);
                slots[idx] = np;
            } else {
                void *volatile p = tgc_alloc(gc, sz);
                if (!p)
                    exit(EXIT_FAILURE);
                slots[idx] = p;
                st->total_allocs++;
            }
            break;
        }
        }

        if (tgc_tracked_count(gc) > st->peak)
            st->peak = tgc_tracked_count(gc);

        /* Periodic explicit collections plus reliance on the auto policy. */
        if (op % 512 == 511)
            tgc_run(gc);
    }

    st->final_tracked = tgc_tracked_count(gc);
    memset(slots, 0, sizeof slots);       /* drop every remaining reference */
}

static int test_stress(tiny_gc_t *gc)
{
    size_t base;
    stress_stats st;

    test_ok = 1;
    base = tgc_tracked_count(gc);

    stress_worker(gc, &st);

    printf("  stress: %zu allocs, peak tracked %zu, final %zu, "
           "%zu collections\n",
           st.total_allocs, st.peak, st.final_tracked,
           tgc_collection_count(gc));
    REQUIRE(st.final_tracked > 0);   /* some blocks were still slotted */

    /* Erase the worker frame (stale slot copies) and collect repeatedly. */
    prepare_collect();
    tgc_run(gc);
    prepare_collect();
    tgc_run(gc);
    REQUIRE(tgc_tracked_count(gc) == base);   /* clean exit, no leaks */

    return test_ok;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    tiny_gc_t gc;
    void *anchor = &gc;           /* anchor: address of a local in main */

    tgc_start(&gc, anchor);

    {
        struct { const char *name; int (*fn)(tiny_gc_t *); } tests[] = {
            { "basic",            test_basic        },
            { "reachability",     test_reachability },
            { "cycles",           test_cycles       },
            { "interleaved",      test_interleaved  },
            { "realloc",          test_realloc      },
            { "dtors",            test_dtors        },
            { "leaf_flag",        test_leaf_flag    },
            { "pause_resume",     test_pause_resume },
            { "stress",           test_stress       },
        };
        size_t i;

        for (i = 0; i < sizeof tests / sizeof tests[0]; i++) {
            test_ok = 1;
            if (tests[i].fn(&gc))
                printf("PASS %s\n", tests[i].name);
            else {
                printf("FAIL %s\n", tests[i].name);
                failures++;
            }
        }
    }

    tgc_stop(&gc);

#ifdef TGC_AUDIT
    {
        extern size_t audit_live_blocks(void);
        extern size_t audit_total_blocks(void);
        size_t live = audit_live_blocks();
        printf("audit: %zu blocks allocated, %zu still live after "
               "tgc_stop\n",
               audit_total_blocks(), live);
        if (live != 0) {
            printf("FAIL leak_check\n");
            failures++;
        }
    }
#endif

    printf("%s (%d failure%s)\n",
           failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
           failures, failures == 1 ? "" : "s");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
