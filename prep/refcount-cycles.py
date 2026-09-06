#!/usr/bin/env python3
"""Reference counting vs tracing GC in CPython (research demo).

Shows why CPython's primary memory manager (reference counting) can never
reclaim reference cycles on its own, and how a tracing (mark-and-sweep)
collector fixes exactly that hole.

Run:  python3 refcount-cycles.py
"""

import gc
import sys
import time
import weakref


def rule(ch="-", width=72):
    print(ch * width)


def section(title):
    print()
    rule("=")
    print(title)
    rule("=")


def step(label):
    print(f"\n-- {label} " + "-" * max(0, 67 - len(label)))


def pause(seconds=0.8):
    # Small delay for live terminal demos; skipped when output is piped,
    # so the script stays TTY-less/CI safe.
    if sys.stdout.isatty():
        time.sleep(seconds)


class Node:
    """A tiny container object; instances are GC-tracked like any container."""

    def __init__(self, name):
        self.name = name
        self.other = None


# Reference: https://docs.python.org/3/library/sys.html#sys.getrefcount
# sys.getrefcount() returns the reference count of an object; classic CPython
# documented it as "one higher than expected" because passing the object as
# an argument creates a temporary reference. Newer builds (e.g. 3.14 with
# deferred reference counting for frame locals) shift absolute values, so
# this script measures a BASELINE in the same call context and reasons
# about DELTAS from it -- version-proof by construction.
#
# Reference: https://devguide.python.org/internals/explaining/garbage-collector/
# "The main garbage collection algorithm used by CPython is reference
# counting": every object stores a count of references to it; when the count
# reaches zero the object is deallocated immediately.


def measure_baseline():
    """Refcount an object owned by exactly ONE variable, same context."""
    probe = Node("probe")            # exactly one program reference
    return sys.getrefcount(probe)


def main():
    # Disable the automatic cyclic GC so *we* control when tracing runs;
    # otherwise it could reap our cycle before we inspect it.
    # Reference: https://docs.python.org/3/library/gc.html#gc.disable
    gc.disable()
    baseline = measure_baseline()
    print(f"interpreter baseline: an object owned by exactly one")
    print(f"variable reports refcount = {baseline} on this build.")
    try:
        demo_solo_object(baseline)
        a_wr, b_wr = demo_cycle_trapped_by_refcounts(baseline)
        demo_tracing_reclaims_the_cycle(a_wr, b_wr)
        explain_why_mark_and_sweep_wins()
    finally:
        gc.enable()


def demo_solo_object(baseline):
    section("PART 1 - An acyclic object: refcounting alone is enough")
    step("Create one object, watch it die the instant the last ref drops")

    solo = Node("solo")
    solo_wr = weakref.ref(solo)  # weak refs do NOT bump the refcount
    rc = sys.getrefcount(solo)
    print(f"solo refcount = {rc}  (baseline {baseline}: exactly one owner)")
    assert rc == baseline
    del solo
    print(f"after 'del solo':  weakref alive? {solo_wr() is not None}")
    print(">> Freed IMMEDIATELY by pure reference counting.")
    print(">> No gc.collect() was needed: refcount hits 0, dealloc runs now.")


def demo_cycle_trapped_by_refcounts(baseline):
    section("PART 2 - A cycle: refcounts never reach zero")
    step("Build A <-> B and read their counts while externally held")

    a, b = Node("A"), Node("B")
    a.other = b          # A holds a ref to B
    b.other = a          # B holds a ref to A  <-- the cycle
    # Reference: https://en.wikipedia.org/wiki/Reference_counting
    # "The reference counting approach has a major disadvantage: if two
    #  objects refer to each other, neither can ever be collected."
    a_wr, b_wr = weakref.ref(a), weakref.ref(b)

    rc_a = sys.getrefcount(a)
    print(f"getrefcount(a) = {rc_a}  (baseline+1 = one variable 'a' PLUS")
    print(f"                  one incoming edge b.other -> a)")
    assert rc_a == baseline + 1

    step("Drop BOTH external references ('del a, b')")
    del a, b
    # Only remaining strong ref to A is the cycle edge b.other -> A
    # (and symmetrically for B). Reading through an explicit handle keeps
    # the arithmetic comparable to the baseline measurement.
    handle = a_wr()
    rc_after = sys.getrefcount(handle)
    print(f"getrefcount via weakref handle = {rc_after}")
    print(f"  (baseline+1 again: the ONLY owner left is the cycle edge)")
    assert rc_after == baseline + 1
    del handle
    print(f"external refs dropped, yet weakref alive? {a_wr() is not None}")

    pause()
    step("Wait... and check again: nobody home, nobody died")
    print(f"weakref STILL alive? {a_wr() is not None}")
    print(">> Refcount can never fall to 0: each node keeps the other alive.")
    print(">> This memory leaks FOREVER under pure reference counting.")
    return a_wr, b_wr


def demo_tracing_reclaims_the_cycle(a_wr, b_wr):
    section("PART 3 - Tracing GC to the rescue: gc.collect()")
    # Reference: https://docs.python.org/3/library/gc.html#gc.get_count
    # Returns (count0, count1, count2): gen-0 counter tracks the delta of
    # (allocations - deallocations) since the last collection; when it
    # exceeds threshold0 an automatic collection would trigger.
    # Reference: https://docs.python.org/3/library/gc.html#gc.set_threshold
    before = gc.get_count()
    print(f"gc.get_count() BEFORE collect = {before}")
    print("   count0 = alloc-minus-dealloc delta driving auto-collections")

    # Reference: https://docs.python.org/3/library/gc.html#gc.collect
    # "With no arguments, run a full collection." Returns the number of
    # unreachable objects found. NOTE: this may include unrelated cycles
    # the interpreter accumulated, not just our two Nodes.
    collected = gc.collect()

    after = gc.get_count()
    print(f"gc.collect() returned               = {collected} objects "
          "(our 2 Nodes + any other cycles)")
    print(f"gc.get_count() AFTER collect  = {after}")
    print(f"Our cycle finally dead?        a: {a_wr() is None}, "
          f"b: {b_wr() is None}")
    print(">> The TRACING collector walked from the roots, found no path")
    print(">> to A or B, and reclaimed the pair refcounting could not.")


def explain_why_mark_and_sweep_wins():
    section("WHY MARK-AND-SWEEP SOLVES WHAT REFCOUNTING CANNOT")
    text = [
        "Refcounting is a LOCAL decision: each object watches only its own",
        "counter. 'Am I referenced?' is answered per-edge, so a ring of N",
        "objects each holding count>=1 looks immortal even when NOTHING in",
        "the program can ever reach them again.",
        "",
        "Mark-and-sweep is a GLOBAL decision over the object graph:",
        "  1. MARK: start from the root set (globals, stack, registers) and",
        "     follow every reference transitively, marking what is reachable.",
        "  2. SWEEP: everything unmarked had NO PATH from the roots - no",
        "     matter how many edges circulate INSIDE it - and is freed.",
        "An unreachable cycle has no inbound edge from the roots, so the",
        "whole ring simply never gets marked. Cycle topology stops mattering.",
        "",
        "Cost trade: refcounting is incremental/prompt but blind to cycles;",
        "tracing must stop and scan, but defines liveness by reachability.",
    ]
    # Reference: https://en.wikipedia.org/wiki/Tracing_garbage_collection
    # Roots are "all the objects referenced from anywhere in the call stack,
    # and any global variables"; anything referenced from a reachable object
    # is itself reachable (transitive closure).
    # Reference: https://github.com/python/cpython/blob/main/InternalDocs/garbage_collector.md
    # "container holds a reference to itself... the reference count never
    #  falls to 0... Therefore it would never be cleaned just by simple
    #  reference counting. For this reason some additional machinery is
    #  needed" -- the cyclic garbage collector.
    for line in text:
        print(line)


if __name__ == "__main__":
    main()
