/* audit.h -- allocation ledger used by the `make leak-check` target. */
#ifndef TGC_AUDIT_H
#define TGC_AUDIT_H

#include <stddef.h>

void *audit_malloc(size_t n);
void *audit_calloc(size_t num, size_t n);
void *audit_realloc(void *old, size_t n);
void audit_free(void *p);

size_t audit_live_blocks(void);   /* blocks not yet returned to libc */
size_t audit_total_blocks(void);  /* blocks ever taken from libc     */

#endif /* TGC_AUDIT_H */
