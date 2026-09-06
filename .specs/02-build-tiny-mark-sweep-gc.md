# Spec 02 — Build: tiny conservative mark-sweep GC in C

## Goal

Implement a small conservative mark-and-sweep garbage collector library in C99, plus a test program proving the behaviors below. This build is driven ONLY by this spec and by what `prep/` established. It must be written fresh from these requirements — as close as possible to a real production-quality tiny GC in behavior, without reference implementations.

## Hard Boundary — No Peeking

**You must NOT open, read, grep, list, or copy anything under `source/`, `source-bellard-otcc/`, or any upstream directory.** Your inputs are:
- `.specs/02-build-tiny-mark-sweep-gc.md` (this file)
- `prep/*.py` and `prep/README.md` (the preparatory study layer)

Violating this voids the exercise.

## Deliverables (in this repo root, i.e. `build/`)

- `tiny_gc.h` — public API
- `tiny_gc.c` — implementation
- `test_tiny_gc.c` — acceptance tests
- `Makefile` — builds `test_tiny_gc` with `gcc -std=c99 -Wall -Wextra -O2`

## Public API Requirements

```c
typedef struct tiny_gc tiny_gc_t;

void  tgc_start(tiny_gc_t *gc, void *stack_anchor);   /* anchor: address of a local in main */
void  tgc_stop(tiny_gc_t *gc);                        /* final sweep + free internal tables */
void *tgc_alloc(tiny_gc_t *gc, size_t size);
void *tgc_calloc(tiny_gc_t *gc, size_t num, size_t size);
void *tgc_realloc(tiny_gc_t *gc, void *ptr, size_t size);
void  tgc_free(tiny_gc_t *gc, void *ptr);             /* explicit, immediate */
void  tgc_run(tiny_gc_t *gc);                         /* manual collect */
void  tgc_pause(tiny_gc_t *gc);
void  tgc_resume(tiny_gc_t *gc);
/* per-object options */
void *tgc_alloc_opt(tiny_gc_t *gc, size_t size, unsigned flags, void (*dtor)(void*));
unsigned tgc_get_flags(tiny_gc_t *gc, void *ptr);
size_t    tgc_get_size(tiny_gc_t *gc, void *ptr);
```

Flag bits (at least): one marking the object an explicit **root** (never collected while registered), one marking it a **leaf** (skip interior scanning of its contents).

## Design Requirements

1. **Out-of-band metadata table.** No per-object header. Maintain your own hash table keyed by pointer address storing {actual_size, flags, dtor}. Must grow when load exceeds ~0.9 and shrink after sweeps. Any correct O(1) open-addressing scheme is acceptable; document which and why in comments.
2. **Allocation via system malloc/calloc/realloc**, tracked in the table on success; realloc/free keep the table consistent.
3. **Root discovery = conservative stack scan.**
   - Stack window: from `stack_anchor` (passed at start) to the address of a local variable inside the scanning function — handle both stack growth directions.
   - Before scanning, spill callee-saved registers into memory reachable from the scan window using `setjmp(jmp_buf)`.
   - Call the stack-scanning function through a `volatile` function pointer so it cannot be inlined and its locals/frame stay live.
   - Objects flagged root are marked before the stack pass.
4. **Marking.** Depth-first; a visited/marked bit prevents infinite descent on cycles. Interior pointer identification: treat every properly-aligned word of each non-leaf object as a potential pointer; reject candidates outside the [min_address, max_address] of known allocations; only exact block starts count (no interior pointers). Leaf objects skip content scanning.
5. **Sweeping.** Collect doomed pointers into a staging array first; run destructors then `free()` after traversal completes; clear mark bits on survivors; apply the auto-trigger policy bookkeeping.
6. **Trigger policy.** Automatic collection when the number of tracked allocations exceeds a factor (e.g., 1.5×) of the count at the end of the last sweep; `tgc_pause/resume` suppresses automatic runs; explicit `tgc_run` always works.
7. **C99, libc only.** No third-party deps. Comment any platform assumptions.

## Acceptance Tests (`test_tiny_gc.c`)

Print PASS/FAIL lines for each; exit nonzero on any failure:

1. **basic**: allocate an object inside a function; let it go out of scope; run collection; allocation count drops (object freed).
2. **reachability**: build a linked structure (node with children array); run several collections; all nodes survive while rooted from a live local variable.
3. **cycles**: create two structures referencing each other, unrooted; after collection both are freed (mark-sweep handles cycles).
4. **interleaved garbage**: alternate useful allocations with immediately-dead ones across multiple collections; live graph intact, dead ones reclaimed.
5. **realloc**: grow and shrink a node's child array via `tgc_realloc`; graph stays consistent through collections; old block no longer tracked.
6. **explicit free + dtors**: register destructors; verify destructor invocation order/count on explicit free and on sweep.
7. **leaf flag**: leaf objects are not scanned (a pointer stored inside a leaf does NOT keep target alive).
8. **pause/resume**: paused collector never auto-triggers; resume restores policy.
9. **stress**: allocate thousands of random-size blocks with random linkage, drop random subsets, collect repeatedly; report counts; process exits cleanly (no crashes).

## Quality Bar

- `-Wall -Wextra` clean.
- Deterministic PASS output on repeated runs.
- Comments explain WHY (esp. setjmp trick, volatile function pointer, range filter) citing the concepts studied in `prep/`.
