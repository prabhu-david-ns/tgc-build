/* audit.c -- allocation ledger used by the `make audit` leak check.
 *
 * tiny_gc.c is compiled with -Dmalloc=audit_malloc -Dcalloc=audit_calloc
 * -Drealloc=audit_realloc -Dfree=audit_free so every block the collector
 * takes from libc is recorded here; test_tiny_gc.c (compiled WITHOUT the
 * macros, so its own stdio etc. are unaffected) asks audit_live_blocks()
 * at the very end: after tgc_stop() it must be zero -- no leaked blocks,
 * no double accounting.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct link {
    struct link *next;
    void *block;
};

static struct link *live_head;
static size_t live_count;
static size_t total_allocs;

static void ledger_add(void *block)
{
    struct link *l = malloc(sizeof *l);
    if (!l)
        abort();
    l->block = block;
    l->next = live_head;
    live_head = l;
    live_count++;
    total_allocs++;
}

static void ledger_del(void *block)
{
    struct link **pp = &live_head;
    while (*pp && (*pp)->block != block)
        pp = &(*pp)->next;
    if (!*pp)
        abort();                  /* freeing something never tracked */
    {
        struct link *dead = *pp;
        *pp = dead->next;
        free(dead);
    }
    live_count--;
}

void *audit_malloc(size_t n)
{
    void *p = malloc(n);
    if (p)
        ledger_add(p);
    return p;
}

void *audit_calloc(size_t num, size_t n)
{
    void *p = calloc(num, n);
    if (p)
        ledger_add(p);
    return p;
}

void *audit_realloc(void *old, size_t n)
{
    /* Capture the old address as an integer BEFORE realloc: after a moving
       realloc the old pointer's VALUE is indeterminate (C11 7.22.3.5), so it
       may not even be compared afterwards.  The ledger keyed entry is found
       by numeric address, like tiny_gc's own old_key pattern. */
    uintptr_t old_key = (uintptr_t)old;
    void *p = realloc(old, n);
    if (p) {
        if (old_key)
            ledger_del((void *)old_key);
        ledger_add(p);
    }
    return p;
}

void audit_free(void *p)
{
    if (p) {
        ledger_del(p);
        free(p);
    }
}

size_t audit_live_blocks(void)
{
    return live_count;
}

size_t audit_total_blocks(void)
{
    return total_allocs;
}
