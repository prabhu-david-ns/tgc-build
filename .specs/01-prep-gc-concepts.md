# Spec 01 — GC Concepts Study Prep (Research + Python Scripts)

## Goal

Create a `prep/` directory in this build repo containing standalone Python scripts that research and demonstrate every garbage-collection concept needed to later implement a tiny conservative mark-sweep GC in C from scratch (Spec 02). These scripts are exploratory teaching artifacts — they become content material. Every concept must be researched via web/docs research and cited; nothing is assumed known.

The final implementation will be in C — these Python scripts are the preparatory study layer (kilo pattern).

## Ground Rules

- Python 3 stdlib only (`ctypes`, `gc`, `sys`, `weakref` are fair game — they are the *research instruments*).
- No pip packages.
- Every script runs standalone: `python3 <script>` produces visible, self-explanatory output with pauses/labels between demonstrations.
- **Every claim, function, or technique MUST cite its source** inline (URL / doc section), like:

```python
# Reference: https://docs.python.org/3/library/gc.html#gc.collect
# gc.collect() runs a full collection regardless of thresholds.
```

- A consolidated bibliography goes in `prep/README.md`, which also documents what each script demonstrates.

## Requirements

1. `prep/refcount-cycles.py` — Research reference counting vs tracing GC:
   - How does CPython's primary memory management work (refcounting)? What happens to two objects referencing each other when all external references drop?
   - Demonstrate: create a cycle, delete external refs, show `sys.getrefcount()` never reaches 0, then show `gc.collect()` reclaims it. Print generation counts before/after (`gc.get_count()`).
   - Explain in output comments WHY mark-and-sweep solves what refcounting cannot.

2. `prep/mark-sweep-sim.py` — Simulate mark-and-sweep over a toy heap in pure Python:
   - Model the heap as a list of fake "objects" (dicts with `id`, `refs`, `marked` fields); model a root set; implement `mark()` (DFS/BFS with visited protection) and `sweep()`.
   - Demo scenarios: unreachable lone object freed; cyclic garbage freed; reachable graph survives; repeated collections are stable.
   - Cite sources describing the mark phase and sweep phase of tracing collectors.

3. `prep/conservative-scan.py` — Research conservative garbage collection:
   - What does it mean for a collector to treat every word of memory as a potential pointer? Why is it called conservative? What are false positives and why do they only cause retention (never corruption)?
   - Using `ctypes`, allocate several real buffers, record their addresses, pack a "fake stack frame" byte buffer containing some real addresses and some random 64-bit words, then scan it word-by-word and report which words would be treated as pointers (range-filtered against recorded allocations).
   - Cite Boehm-Demers-Weiser GC documentation or equivalent on conservative collection.

4. `prep/out-of-band-metadata.py` — Compare metadata strategies:
   - Per-object header (prepend size/flags to each allocation) vs out-of-band table keyed by address.
   - Simulate both in Python (ctypes buffer + prepended header vs dict keyed by `buffer_address`). Discuss trade-offs in comments: zero per-object overhead vs lookup cost, exact-size knowledge for scanning.

5. `prep/stack-scanning.py` — Research how a real collector finds roots on the stack:
   - Stack growth direction, why a collector needs an anchor ("bottom" captured at start) and its own current-frame address as the other bound; register spilling (what `setjmp` achieves for a C collector).
   - Python cannot take addresses of locals like C — simulate: build an array of fake "stack slots" (ints), designate a window between two indices, scan it conservatively against the allocations from script 3's approach. Document in comments exactly how the C version differs (anchor argument, local address inside scanner, setjmp jmp_buf).

6. `prep/trigger-policy.py` — Research when collectors run:
   - Allocation-count threshold policies, generational ideas at a glance, and the simple "collect when live-object count exceeds N× count-at-last-sweep" policy.
   - Simulate alloc/free churn against a threshold policy and print when collections trigger.

7. `prep/destructors-weakrefs.py` — Research finalization:
   - How destructors/finalizers interact with GC ordering; `weakref` semantics; why running arbitrary user code during sweep is dangerous in C.
   - Demo weakref callbacks firing after collection.

8. All scripts include `if __name__ == '__main__': main()` entry points and clean readable output suitable for turning into terminal demos later.

## Acceptance Criteria

- Every script compiles and runs under Python 3 stdlib only.
- Output clearly labels each demonstration step.
- Every non-obvious claim has an inline citation; `prep/README.md` has a full bibliography.
- No file in `prep/` references or imports anything from the upstream `source/` directory — the concepts must stand alone.

## Out of Scope

The C implementation itself (Spec 02). No .c/.h files here.
