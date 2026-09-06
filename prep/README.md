# GC Concepts Study Prep (Spec 01)

Standalone Python 3 (stdlib-only) research scripts that build up every
concept needed to implement a tiny conservative mark-sweep GC in C
(Spec 02). This is the "kilo" preparatory layer: each script is a runnable
teaching artifact with labeled demo sections and inline source citations.

Run any of them with:

```
python3 prep/<script>.py
```

They are TTY-less-safe: when stdout is a pipe/CI log, the inter-section
pauses are skipped; on an interactive terminal they pause briefly between
demos so output reads like a live lesson. Randomness is seeded for
reproducible runs.

## Scripts

| Script | Demonstrates |
|---|---|
| `refcount-cycles.py` | CPython's primary memory management is reference counting (`sys.getrefcount`, immediate dealloc). A reference cycle keeps both counts >= 1 forever after all external refs drop; `gc.get_count()` before/after shows gen-0 counter state; `gc.collect()` reclaims what refcounting cannot. Prints the core argument: mark-and-sweep defines liveness by global reachability from roots, so unreachable rings die regardless of internal edges. |
| `mark-sweep-sim.py` | Pure-Python toy heap (dicts with `id`, `refs`, `marked`) plus root set. Implements `mark()` as iterative DFS worklist with visited-set cycle protection and `sweep()` in allocation order. Scenarios: lone unreachable object freed; unreachable cyclic pair freed; reachable cycle survives thanks to visited protection; three repeated collections reach a fixed point (idempotence). |
| `conservative-scan.py` | What "every word might be a pointer" means in practice. Allocates real `ctypes` buffers, records their addresses, packs a fake stack frame byte-buffer with true roots, interior pointers, past-end values, planted false positives and random 64-bit words; scans word-aligned words with a range filter against recorded allocations. Shows false positives cause retention only -- never corruption -- because conservative collectors never move objects. |
| `out-of-band-metadata.py` | Per-object headers (`[magic|size|flags] + payload` in one ctypes buffer) vs out-of-band table keyed by block address (`{address: metadata}` dict). Dumps both layouts, sweeps under both rules (header dies with block vs mandatory table-entry delete), demonstrates exact-size knowledge for sequential sweeping, and micro-benchmarks metadata access cost. Verdict for Spec 02: headers. |
| `stack-scanning.py` | How roots are found on the machine stack: growth direction, the startup-captured stack bottom anchor, the scanner's own local address as young bound, and `setjmp` spilling callee-saved registers into a scannable `jmp_buf`. Python can't take `&local`, so it simulates with an array of fake slots scanned conservatively against ctypes allocations. Comments spell out exactly how the C version differs (anchor argument, local address inside scanner, jmp_buf). |
| `trigger-policy.py` | When collectors run. P1: fixed allocation threshold (CPython's `allocations - deallocations > threshold0`). P2: growth policy -- collect when live count exceeds N x live-at-last-sweep (the tiny-GC choice). P3: generational sketch (young collected often, survivors promoted, old generation rarely touched) mirroring CPython's thresholds and BDWGC's expand-vs-collect heuristic. Simulated churn prints every trigger event with its reason. |
| `destructors-weakrefs.py` | Finalization ordering: acyclic objects finalize immediately at refcount zero with weakref callbacks firing after teardown; cyclic finalizers are deferred to `gc.collect()` (PEP 442); resurrection works exactly once (`gc.is_finalized`). Explains why running user code during sweep corrupts a C collector (allocation re-entry, mid-sweep resurrection, use-after-free) and why BDWGC enqueues finalizers to run outside the collector. |

## Shared conventions

- Python 3 stdlib only: `gc`, `sys`, `weakref`, `ctypes`, `struct`,
  `random`, `time`.
- Each script has a `main()` guarded by `if __name__ == '__main__':`.
- Every non-obvious claim carries an inline `# Reference:` comment.
- No file references or imports anything outside this directory or the
  stdlib; these concepts stand alone from the upstream implementation.

## Key takeaways carried into Spec 02

1. Liveness = transitive reachability from roots; cycles are only garbage
   if no root path reaches them.
2. Conservatism (scan all stack/register/global words) buys type-free C
   correctness at the price of occasional retention; never move blocks.
3. Headers per allocation give O(1) size/metadata access and make sweep
   walk trivial; keep GC bookkeeping outside scanned memory.
4. Scan from a startup-captured stack bottom down to the scanner's own
   frame; spill registers first.
5. Trigger collections on heap growth relative to last sweep.
6. Never run finalizers mid-collection; queue them.

## Bibliography

### CPython documentation

- `gc` module (collect, get_count, set_threshold, disable, is_finalized,
  garbage): https://docs.python.org/3/library/gc.html
- `sys.getrefcount` (+1 temporary-reference caveat):
  https://docs.python.org/3/library/sys.html#sys.getrefcount
- Data model, `object.__del__` (when finalizers run; interpreter-exit
  caveat): https://docs.python.org/3/reference/datamodel.html#object.__del__
- `weakref` (no refcount bump, callbacks on clearing):
  https://docs.python.org/3/library/weakref.html
- `ctypes` (`create_string_buffer`, `addressof`):
  https://docs.python.org/3/library/ctypes.html
- `struct` (packed little-endian records used to fake C structs):
  https://docs.python.org/3/library/struct.html

### CPython internals

- Garbage collector design (refcounting primary; cycle detection;
  generations; weak generational hypothesis; threshold0 semantics;
  full-collection damping): 
  https://github.com/python/cpython/blob/main/InternalDocs/garbage_collector.md
  (rendered: https://devguide.python.org/internals/explaining/garbage-collector/)
- PEP 442 -- Safe object finalization (finalize-once, resurrection-safe
  cycle collection, Python 3.4+): https://peps.python.org/pep-0442/

### Boehm-Demers-Weiser conservative GC

- Algorithmic overview (four phases: preparation/mark/sweep/finalization;
  "views all static data areas, stacks and registers as potentially
  containing pointers"; out-of-band block headers; expand-vs-collect
  heuristic via `GC_free_space_divisor`; finalizers run outside the
  collector): https://www.hboehm.info/gc/gcdescr.html --
  maintained mirror: https://github.com/bdwgc/bdwgc/blob/master/docs/gcdescr.md
- BDWGC tutorial slides ("If it might be a pointer it's treated as a
  pointer", "we never move any objects", false-pointer retention,
  black-listing, register contents pushed onto the GC-visible stack):
  https://www.hboehm.info/gc/04tutorial.pdf
- Bounds on unnecessary space retention by conservative collectors
  (bounded extra space; pointers to unallocated memory never valid):
  https://www.hboehm.info/gc/bounds.html
- Boehm, H., & Weiser, M., "Garbage Collection in an Uncooperative
  Environment", Software: Practice and Experience 18(9), 1988
  (the original conservative-GC paper).

### Background reading

- Tracing garbage collection / naive mark-and-sweep / tri-color marking:
  https://en.wikipedia.org/wiki/Tracing_garbage_collection
- Reference counting and the cycle disadvantage:
  https://en.wikipedia.org/wiki/Reference_counting
- Call stacks, frames, stack-and-frame pointers:
  https://en.wikipedia.org/wiki/Call_stack
- `setjmp`/`longjmp` saving the calling environment (registers into
  `jmp_buf`): https://pubs.opengroup.org/onlinepubs/9699919799/functions/setjmp.html

## Out of scope

The C implementation itself -- that is Spec 02. No `.c`/`.h` files here.
