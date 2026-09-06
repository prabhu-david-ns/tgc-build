#!/usr/bin/env python3
"""Finalization research: destructors, weakrefs, and GC ordering.

Demonstrates:
  1. Acyclic object: __del__ runs immediately at refcount zero; the
     weakref callback fires right after the object is torn down.
  2. Cyclic pair: __del__ does NOT run until gc.collect(); PEP 442 makes
     finalizing cycles safe and guarantees at-most-once semantics.
  3. Resurrection: a finalizer can revive its object (Lazarus); it will
     never be finalized a second time.
  4. Why running arbitrary user code DURING sweep is dangerous in C, and
     how real collectors defer it.

Run:  python3 destructors-weakrefs.py
"""

import gc
import sys
import weakref

# Reference: https://docs.python.org/3/reference/datamodel.html#object.__del__
#   "__del__() ... is called when the object is about to be destroyed."
#   For acyclic objects in CPython that moment is the refcount hitting
#   zero -- immediate and deterministic. The docs also warn: "It is not
#   guaranteed that __del__() methods are called for objects that still
#   exist when the interpreter exits."
#
# Reference: https://peps.python.org/pep-0442/ ("Safe object finalization")
#   Before PEP 442 (Python 3.4), cycles containing objects with __del__
#   were uncollectable and parked in gc.garbage. PEP 442 lets the cyclic
#   collector finalize such objects safely: each object's finalizer runs
#   AT MOST ONCE, resurrection is allowed, and leftovers are collected on
#   the next cycle anyway.
#
# Reference: https://docs.python.org/3/library/weakref.html
#   A weak reference "does not increase the reference count" of the
#   referent. If a callback is supplied, it is invoked when the weakref
#   is cleared -- i.e., after the referent has been finalized/killed --
#   so the callback receives an argument that is None or already dead.
#
# Reference: https://www.hboehm.info/gc/gcdescr.html
#   BDWGC's phase model: "Finalization phase. Unreachable objects which
#   had been registered for finalization are enqueued for finalization
#   OUTSIDE the collector." Deferral is the safety mechanism.
#
# WHY FINALIZERS DURING SWEEP ARE DANGEROUS IN A C COLLECTOR:
#   - A finalizer may ALLOCATE -> re-enters the allocator while the free
#     list is mid-mutation => corruption.
#   - It may RESURRECT the object being swept -> the block must be pulled
#     back from death mid-sweep, invalidating sweep invariants.
#   - It may read OTHER unreachable objects (already on the dead list)
#     whose memory may be reused any instant => use-after-free.
#   - Order between neighbors is arbitrary: a finalizer touching a peer
#     object cannot know if the peer was finalized/freed first.
#   Hence: mark + enqueue during collection; RUN user code only after the
#   collector is back in a consistent state.


def rule(ch="-", width=72):
    print(ch * width)


def section(title):
    print()
    rule("=")
    print(title)
    rule("=")


class Ephemeral:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        print(f"    [finalizer]   Ephemeral({self.name}).__del__ ran")


def make_watched(name):
    """Create an object plus a weakref whose callback reports its death."""
    obj = Ephemeral(name)

    def cb(dead_ref):
        print(f"    [weakref-cb]  {name}: referent cleared "
              f"(ref now returns {dead_ref()})")

    return obj, weakref.ref(obj, cb)


def demo_acyclic():
    section("CASE 1 - Acyclic: deterministic teardown order")
    obj, wr = make_watched("acyclic")
    print("  created; weakref alive?", wr() is not None)
    print("  >> del obj; watch which fires first:")
    del obj
    assert wr() is None
    print(f"  after: weakref alive? {wr() is not None}")
    print("  Refcount hit 0 => dealloc => finalizer => refs cleared =>")
    print("  callbacks. No gc.collect() involved anywhere.")


def demo_cycle():
    section("CASE 2 - Cycle with finalizers: deferred until collect")
    gc.disable()
    try:
        events = []

        class Tracked(Ephemeral):
            def __del__(self):
                events.append("finalizer:" + self.name)
                super().__del__()

        def make(name):
            obj = Tracked(name)

            def cb(dead_ref):
                events.append("callback:" + name)
                print(f"    [weakref-cb]  {name}: referent cleared")

            return obj, weakref.ref(obj, cb)

        a, wr_a = make("cycle-a")
        b, wr_b = make("cycle-b")
        a.peer = b
        b.peer = a                       # the cycle
        del a, b
        print("  externals dropped; NOTHING fired yet:")
        print(f"    weakrefs alive? a={wr_a() is not None} "
              f"b={wr_b() is not None}")
        print("  >> __del__ did NOT run at 'del': each object still has a")
        print("  >> nonzero count via the cycle edge.")
        print("  >> gc.collect(); watch the teardown sequence:")
        n = gc.collect()
        print(f"    (collect returned {n}; weakrefs alive? "
              f"a={wr_a() is not None} b={wr_b() is not None})")
        first_cb = min(i for i, e in enumerate(events)
                       if e.startswith("callback"))
        first_fin = min(i for i, e in enumerate(events)
                        if e.startswith("finalizer"))
        if first_cb < first_fin:
            print(f"  observed order: {events}")
            print("  THIS BUILD ran weakref callbacks before __del__.")
            print("  Older CPythons (<=3.12 style) ran the finalizers")
            print("  first, then cleared refs. Ordering is NOT portable;")
            print("  what IS guaranteed: nothing runs until the collector")
            print("  has decided the whole ring is garbage, each __del__")
            print("  runs at most once (PEP 442), and by callback time")
            print("  the referent is definitively dead.")
    finally:
        gc.enable()


GHOST = None


class Lazarus:
    def __del__(self):
        global GHOST
        GHOST = self                     # RESURRECTION
        print("    [finalizer]   Lazarus.__del__ ran; resurrected myself")


def demo_resurrection():
    global GHOST
    section("CASE 3 - Resurrection and finalize-once (gc.is_finalized)")
    laz = Lazarus()
    del laz                              # dies... comes back via GHOST
    ghost_alive = GHOST is not None
    finalized_once = gc.is_finalized(GHOST)
    print(f"  resurrected? {ghost_alive};  "
          f"gc.is_finalized(ghost) = {finalized_once}")
    # Reference: https://docs.python.org/3/library/gc.html#gc.is_finalized
    #   "Returns True if the given object has been finalized by the
    #    garbage collector."
    print("  >> dropping it again -- expect NO second '__del__' line:")
    del GHOST                            # second death: silent
    print("  (nothing printed above this line => finalized once ever)")
    print("  PEP 442 guarantee: at most one finalization per object, no")
    print("  matter how often it is resurrected.")


def main():
    demo_acyclic()
    demo_cycle()
    demo_resurrection()

    section("LESSONS FOR THE C COLLECTOR (Spec 02 preview)")
    print("""  Our tiny C GC will NOT run user code during sweep:
    1. sweep(): unlink unmarked blocks, push onto a to-finalize queue.
    2. collector returns; heap state is consistent again.
    3. drain queue: call registered C cleanup hooks (they may allocate,
       drop references, even re-enter the GC safely).
  Weakrefs map to: a registration table scanned conservatively like any
  other root storage; entries are nulled when their target is swept, and
  user callbacks run only in step 3 -- honoring the guarantee we just
  verified: no user code executes while the collector is mid-flight.""")


if __name__ == "__main__":
    main()
