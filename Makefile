# Builds the tiny conservative GC and its acceptance tests.
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2

all: test_tiny_gc

test_tiny_gc: tiny_gc.c tiny_gc.h test_tiny_gc.c
	$(CC) $(CFLAGS) -o $@ tiny_gc.c test_tiny_gc.c

# Leak sanity check: tiny_gc.c is compiled twice -- once plainly, once with
# its libc allocations routed through an interposing ledger (see audit.c).
# Only the ledger build is linked into the binary; the test then reports
# "0 still live" after tgc_stop() if the collector returned every block.
# (AddressSanitizer is unavailable in this environment -- no libasan
# runtime -- so this is the equivalent ownership audit for GC-owned
# memory.)
leak-check: test_tiny_gc_audit
	./test_tiny_gc_audit

test_tiny_gc_audit: tiny_gc.c tiny_gc.h test_tiny_gc.c audit.c audit.h
	$(CC) $(CFLAGS) -Dmalloc=audit_malloc -Dcalloc=audit_calloc \
	    -Drealloc=audit_realloc -Dfree=audit_free -c tiny_gc.c -o $@.ledger.o
	$(CC) $(CFLAGS) -c audit.c -o $@.audit.o
	$(CC) $(CFLAGS) -DTGC_AUDIT -c test_tiny_gc.c -o $@.test.o
	$(CC) $(CFLAGS) -o $@ $@.ledger.o $@.audit.o $@.test.o

run: test_tiny_gc
	./test_tiny_gc

clean:
	rm -f test_tiny_gc test_tiny_gc_audit *.o

.PHONY: all leak-check run clean
